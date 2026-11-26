#include "kernels.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__APPLE__) && !defined(DH_GRAPH_PORTABLE)
#define ACCELERATE_NEW_LAPACK 1
#include <Accelerate/Accelerate.h>
#define DH_HAVE_ACCELERATE 1
#endif

namespace deckhand::stems::graph {

// Portable fallback: cache-blocked, four output rows per pass so every loaded B row feeds four fused multiply-add streams, and the
// inner loop runs over contiguous columns so the compiler vectorises it (NEON, SSE, AVX). Not a BLAS, but within a small factor
// of one for the matrix shapes the models use.
void sgemm_portable(bool ta, bool tb, int M, int N, int K, float alpha, const float* A, int lda, const float* B, int ldb,
                    float beta, float* C, int ldc) {
  if (M <= 0 || N <= 0) return;
  for (int i = 0; i < M; ++i) {
    float* c = C + std::size_t(i) * ldc;
    if (beta == 0.f) std::memset(c, 0, std::size_t(N) * sizeof(float));
    else if (beta != 1.f) for (int j = 0; j < N; ++j) c[j] *= beta;
  }
  if (K <= 0) return;
  constexpr int KB = 256, NB = 1024;
  std::vector<float> panel;
  if (tb) panel.resize(std::size_t(KB) * NB);
  auto a_at = [&](int i, int k) { return alpha * (ta ? A[std::size_t(k) * lda + i] : A[std::size_t(i) * lda + k]); };
  for (int n0 = 0; n0 < N; n0 += NB) {
    const int nb = std::min(NB, N - n0);
    for (int k0 = 0; k0 < K; k0 += KB) {
      const int kb = std::min(KB, K - k0);
      const float* Bp;
      std::size_t ldp;
      if (tb) {  // B is [N,K]: copy the block transposed so rows of the panel are contiguous in j
        for (int j = 0; j < nb; ++j) {
          const float* src = B + std::size_t(n0 + j) * ldb + k0;
          for (int k = 0; k < kb; ++k) panel[std::size_t(k) * nb + j] = src[k];
        }
        Bp = panel.data();
        ldp = std::size_t(nb);
      } else {
        Bp = B + std::size_t(k0) * ldb + n0;
        ldp = std::size_t(ldb);
      }
      int i = 0;
      for (; i + 4 <= M; i += 4) {
        // 4 x 16 register tiles: 64 accumulators stay in vector registers across the whole k loop
        int j = 0;
        for (; j + 16 <= nb; j += 16) {
          float acc[4][16] = {};
          for (int k = 0; k < kb; ++k) {
            const float a[4] = {a_at(i, k0 + k), a_at(i + 1, k0 + k), a_at(i + 2, k0 + k), a_at(i + 3, k0 + k)};
            const float* __restrict b = Bp + std::size_t(k) * ldp + j;
            for (int r = 0; r < 4; ++r)
              for (int jj = 0; jj < 16; ++jj) acc[r][jj] += a[r] * b[jj];
          }
          for (int r = 0; r < 4; ++r) {
            float* __restrict c = C + std::size_t(i + r) * ldc + n0 + j;
            for (int jj = 0; jj < 16; ++jj) c[jj] += acc[r][jj];
          }
        }
        if (j < nb) {  // remaining columns
          float* __restrict c0 = C + std::size_t(i) * ldc + n0;
          float* __restrict c1 = c0 + ldc;
          float* __restrict c2 = c1 + ldc;
          float* __restrict c3 = c2 + ldc;
          for (int k = 0; k < kb; ++k) {
            const float a0 = a_at(i, k0 + k), a1 = a_at(i + 1, k0 + k), a2 = a_at(i + 2, k0 + k), a3 = a_at(i + 3, k0 + k);
            const float* __restrict b = Bp + std::size_t(k) * ldp;
            for (int jj = j; jj < nb; ++jj) {
              const float bj = b[jj];
              c0[jj] += a0 * bj; c1[jj] += a1 * bj; c2[jj] += a2 * bj; c3[jj] += a3 * bj;
            }
          }
        }
      }
      for (; i < M; ++i) {
        float* __restrict c = C + std::size_t(i) * ldc + n0;
        for (int k = 0; k < kb; ++k) {
          const float a = a_at(i, k0 + k);
          const float* __restrict b = Bp + std::size_t(k) * ldp;
          for (int j = 0; j < nb; ++j) c[j] += a * b[j];
        }
      }
    }
  }
}

namespace { std::atomic<bool> g_force_portable{false}; }
void set_force_portable_kernel(bool on) { g_force_portable.store(on); }
bool portable_kernel_forced() { return g_force_portable.load(); }

void sgemm(bool ta, bool tb, int M, int N, int K, float alpha, const float* A, int lda, const float* B, int ldb, float beta,
           float* C, int ldc) {
#if defined(DH_HAVE_ACCELERATE)
  if (g_force_portable.load()) { sgemm_portable(ta, tb, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc); return; }
  if (M <= 0 || N <= 0) return;
  if (K <= 0) { sgemm_portable(ta, tb, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc); return; }
  cblas_sgemm(CblasRowMajor, ta ? CblasTrans : CblasNoTrans, tb ? CblasTrans : CblasNoTrans, M, N, K, alpha, A, lda, B, ldb,
              beta, C, ldc);
#else
  sgemm_portable(ta, tb, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#endif
}

void vec_expf(const float* in, float* out, int n) {
#if defined(DH_HAVE_ACCELERATE)
  if (!g_force_portable.load()) { vvexpf(out, in, &n); return; }
#endif
  for (int i = 0; i < n; ++i) out[i] = std::exp(in[i]);
}

const char* kernel_backend_name() {
#if defined(DH_HAVE_ACCELERATE)
  return g_force_portable.load() ? "portable" : "accelerate";
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
