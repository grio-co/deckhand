// The numeric kernel interface of the executor. Everything heavy goes through these few functions so another BLAS
// (OpenBLAS, MKL) can be slotted in by replacing kernels.cpp only.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>

namespace deckhand::stems::graph {

// Row-major C[M,N] = alpha * op(A) * op(B) + beta * C. lda/ldb/ldc are row strides of the stored matrices.
void sgemm(bool trans_a, bool trans_b, int M, int N, int K, float alpha, const float* A, int lda, const float* B, int ldb,
           float beta, float* C, int ldc);
// Always the portable implementation (used by tests to cross-check the system BLAS).
void sgemm_portable(bool trans_a, bool trans_b, int M, int N, int K, float alpha, const float* A, int lda, const float* B,
                    int ldb, float beta, float* C, int ldc);
// out[i] = exp(in[i]) for n values (vForce on macOS; std::exp elsewhere). in == out is allowed.
void vec_expf(const float* in, float* out, int n);

// Tests and benchmarks can make sgemm() use the portable code even where a system BLAS exists.
void set_force_portable_kernel(bool on);
bool portable_kernel_forced();
const char* kernel_backend_name();  // "accelerate" or "portable"
//  // "accelerate" or "portable"

// Small persistent pool. run() splits [0,n) into chunks of at least `grain` items, blocks until all are done, and runs
// inline when called from a pool thread (no nested parallelism) or when the pool has one thread.
class ThreadPool {
 public:
  explicit ThreadPool(int threads);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  int threads() const { return threads_; }
  void run(std::int64_t n, std::int64_t grain, const std::function<void(std::int64_t, std::int64_t)>& fn);

 private:
  void worker();
  int threads_;
  std::vector<std::thread> workers_;
  std::mutex m_;
  std::condition_variable cv_, done_cv_;
  const std::function<void(std::int64_t, std::int64_t)>* fn_ = nullptr;
  std::int64_t n_ = 0, chunk_ = 1;
  std::atomic<std::int64_t> next_{0};
  int active_ = 0;
  std::uint64_t epoch_ = 0;
  bool stop_ = false;
};

}  // namespace deckhand::stems::graph
