#include "kernels.hpp"

#include <algorithm>
#include <cstring>

#if defined(__APPLE__) && !defined(DH_GRAPH_PORTABLE)
#define ACCELERATE_NEW_LAPACK 1
#include <Accelerate/Accelerate.h>
#define DH_HAVE_ACCELERATE 1
#endif

namespace deckhand::stems::graph {

void sgemm_portable(bool ta, bool tb, int M, int N, int K, float alpha, const float* A, int lda, const float* B, int ldb,
                    float beta, float* C, int ldc) {
  if (M <= 0 || N <= 0) return;
  for (int i = 0; i < M; ++i) {
    float* c = C + std::size_t(i) * ldc;
    if (beta == 0.f) std::memset(c, 0, std::size_t(N) * sizeof(float));
    else if (beta != 1.f) for (int j = 0; j < N; ++j) c[j] *= beta;
  }
  if (K <= 0) return;
  // Pack B^T access: when tb, B is [N,K]; copy a K-block into a [kb,N] panel so the inner loop is contiguous.
  constexpr int KB = 128;
  std::vector<float> panel;
  if (tb) panel.resize(std::size_t(KB) * N);
  for (int k0 = 0; k0 < K; k0 += KB) {
    int kb = std::min(KB, K - k0);
    const float* Bp;
    int ldp;
    if (tb) {
      for (int j = 0; j < N; ++j)
        for (int k = 0; k < kb; ++k) panel[std::size_t(k) * N + j] = B[std::size_t(j) * ldb + k0 + k];
      Bp = panel.data();
      ldp = N;
    } else {
      Bp = B + std::size_t(k0) * ldb;
      ldp = ldb;
    }
    for (int i = 0; i < M; ++i) {
      float* c = C + std::size_t(i) * ldc;
      for (int k = 0; k < kb; ++k) {
        float a = alpha * (ta ? A[std::size_t(k0 + k) * lda + i] : A[std::size_t(i) * lda + k0 + k]);
        const float* b = Bp + std::size_t(k) * ldp;
        for (int j = 0; j < N; ++j) c[j] += a * b[j];
      }
    }
  }
}

void sgemm(bool ta, bool tb, int M, int N, int K, float alpha, const float* A, int lda, const float* B, int ldb, float beta,
           float* C, int ldc) {
#if defined(DH_HAVE_ACCELERATE)
  if (M <= 0 || N <= 0) return;
  if (K <= 0) { sgemm_portable(ta, tb, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc); return; }
  cblas_sgemm(CblasRowMajor, ta ? CblasTrans : CblasNoTrans, tb ? CblasTrans : CblasNoTrans, M, N, K, alpha, A, lda, B, ldb,
              beta, C, ldc);
#else
  sgemm_portable(ta, tb, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#endif
}

const char* kernel_backend_name() {
#if defined(DH_HAVE_ACCELERATE)
  return "accelerate";
#else
  return "portable";
#endif
}

// ---- thread pool ------------------------------------------------------------------------------------------------
namespace { thread_local bool t_in_pool = false; }

ThreadPool::ThreadPool(int threads) : threads_(std::max(1, threads)) {
  for (int i = 1; i < threads_; ++i) workers_.emplace_back([this] { worker(); });
}
ThreadPool::~ThreadPool() {
  { std::lock_guard<std::mutex> l(m_); stop_ = true; }
  cv_.notify_all();
  for (auto& t : workers_) t.join();
}

void ThreadPool::worker() {
  t_in_pool = true;
  std::uint64_t seen = 0;
  std::unique_lock<std::mutex> l(m_);
  for (;;) {
    cv_.wait(l, [&] { return stop_ || epoch_ != seen; });
    if (stop_) return;
    seen = epoch_;
    const auto* fn = fn_;
    if (!fn) continue;  // the run already finished without us
    std::int64_t n = n_, chunk = chunk_;
    ++active_;
    l.unlock();
    for (;;) {
      std::int64_t b = next_.fetch_add(chunk);
      if (b >= n) break;
      (*fn)(b, std::min(n, b + chunk));
    }
    l.lock();
    if (--active_ == 0) done_cv_.notify_all();
  }
}

void ThreadPool::run(std::int64_t n, std::int64_t grain, const std::function<void(std::int64_t, std::int64_t)>& fn) {
  if (n <= 0) return;
  if (threads_ == 1 || t_in_pool || n <= grain) { fn(0, n); return; }
  std::int64_t chunk = std::max<std::int64_t>(grain, (n + threads_ * 4 - 1) / (threads_ * 4));
  {
    std::lock_guard<std::mutex> l(m_);
    fn_ = &fn; n_ = n; chunk_ = chunk;
    next_.store(0);
    ++epoch_;
  }
  cv_.notify_all();
  t_in_pool = true;  // the caller participates; nested run() calls go inline
  for (;;) {
    std::int64_t b = next_.fetch_add(chunk);
    if (b >= n) break;
    fn(b, std::min(n, b + chunk));
  }
  t_in_pool = false;
  std::unique_lock<std::mutex> l(m_);
  // Wait for workers that picked up this epoch to finish; late workers find no work (next_ >= n_).
  done_cv_.wait(l, [&] { return active_ == 0; });
  fn_ = nullptr;
}

}  // namespace deckhand::stems::graph
