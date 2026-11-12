#pragma once
// Small shared DSP helpers for the audio module (internal; not installed).
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace deckhand {

// 4-point Hermite interpolation of a stereo frame at `pos` (frames); silence outside [-1, frames).
inline void hermite_frame(const float* pcm, std::int64_t frames, double pos, float& l, float& r) {
  const std::int64_t i = static_cast<std::int64_t>(std::floor(pos));
  if (i < -1 || i >= frames) {
    l = r = 0;
    return;
  }
  const float t = static_cast<float>(pos - static_cast<double>(i));
  float out[2];
  for (int c = 0; c < 2; ++c) {
    const auto at = [&](std::int64_t k) { return (k < 0 || k >= frames) ? 0.0f : pcm[k * 2 + c]; };
    const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    out[c] = ((c3 * t + c2) * t + c1) * t + x0;
  }
  l = out[0];
  r = out[1];
}

// Wait-free single-producer / single-consumer ring of stereo frames (interleaved floats).
class FrameRing {
 public:
  void init(std::size_t frames_pow2) {
    buf_.assign(frames_pow2 * 2, 0.0f);
    cap_ = frames_pow2;
    head_.store(0);
    tail_.store(0);
  }
  std::size_t capacity() const { return cap_; }
  // Producer: writes as many whole frames as fit; returns the number written.
  std::size_t push(const float* src, std::size_t frames) {
    const std::uint64_t h = head_.load(std::memory_order_relaxed);
    const std::uint64_t t = tail_.load(std::memory_order_acquire);
    const std::size_t room = cap_ - static_cast<std::size_t>(h - t);
    const std::size_t n = frames < room ? frames : room;
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t k = static_cast<std::size_t>((h + i) & (cap_ - 1)) * 2;
      buf_[k] = src[i * 2];
      buf_[k + 1] = src[i * 2 + 1];
    }
    head_.store(h + n, std::memory_order_release);
    return n;
  }
  // Consumer.
  std::size_t pop(float* dst, std::size_t max_frames) {
    const std::uint64_t t = tail_.load(std::memory_order_relaxed);
    const std::uint64_t h = head_.load(std::memory_order_acquire);
    std::size_t n = static_cast<std::size_t>(h - t);
    if (n > max_frames) n = max_frames;
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t k = static_cast<std::size_t>((t + i) & (cap_ - 1)) * 2;
      dst[i * 2] = buf_[k];
      dst[i * 2 + 1] = buf_[k + 1];
    }
    tail_.store(t + n, std::memory_order_release);
    return n;
  }
  std::size_t size() const {
    return static_cast<std::size_t>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
  }

 private:
  std::vector<float> buf_;
  std::size_t cap_ = 0;
  std::atomic<std::uint64_t> head_{0}, tail_{0};
};

}  // namespace deckhand
