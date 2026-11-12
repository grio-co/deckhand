#pragma once
// Wait-free single-producer / single-consumer ring. Safe for the audio thread.
#include <array>
#include <atomic>
#include <cstddef>

namespace deckhand {

template <class T, std::size_t N>
class SpscRing {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");

 public:
  bool push(const T& v) {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    const std::size_t t = tail_.load(std::memory_order_acquire);
    if (h - t == N) return false;
    buf_[h & (N - 1)] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool pop(T& out) {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t h = head_.load(std::memory_order_acquire);
    if (t == h) return false;
    out = buf_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

 private:
  std::array<T, N> buf_{};
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
};

}  // namespace deckhand
