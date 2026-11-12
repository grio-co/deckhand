#pragma once
// Master-bus recorder: the audio thread pushes frames into a preallocated lock-free ring; a writer thread
// drains it to a 24-bit stereo WAV (RF64 beyond 4 GB) and refreshes the header about once a second.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "dsp.hpp"

namespace deckhand {

class Recorder {
 public:
  enum State : int { Idle = 0, Armed = 1, Recording = 2, Stopping = 3, Done = 4 };
  ~Recorder() { close(); }

  // Non-real-time. Opens the file and starts the writer thread (state Armed). Fails while still active.
  bool open(const std::string& path, std::uint32_t sample_rate, std::string& error);
  // Real-time (audio thread only).
  void start() { int s = Armed; state_.compare_exchange_strong(s, Recording); }
  void stop() {
    int s = Recording;
    if (!state_.compare_exchange_strong(s, Stopping)) {
      s = Armed;
      state_.compare_exchange_strong(s, Stopping);
    }
  }
  bool recording() const { return state_.load(std::memory_order_relaxed) == Recording; }
  void push(const float* interleaved, std::size_t frames) {
    const std::size_t n = ring_.push(interleaved, frames);
    if (n < frames) dropped_.fetch_add(frames - n, std::memory_order_relaxed);
  }
  // Non-real-time: waits for the writer to finish (it finishes after stop()) and releases it.
  void close();
  // Non-real-time and without a running audio thread: ends the recording directly, then closes.
  void abort();

  bool is_open() const { return state_.load() != Idle; }
  int state() const { return state_.load(); }
  double seconds() const { return sr_ > 0 ? static_cast<double>(frames_.load()) / sr_ : 0.0; }
  std::uint64_t dropped() const { return dropped_.load(); }
  std::uint64_t bytes() const { return frames_.load() * 6; }
  bool error() const { return error_.load(); }
  bool finished() const { return finished_.load(); }

 private:
  void run();
  void write_header(bool final_header);

  std::atomic<int> state_{Idle};
  std::atomic<std::uint64_t> frames_{0}, dropped_{0};
  std::atomic<bool> error_{false}, finished_{false};
  FrameRing ring_;
  std::thread thread_;
  std::FILE* file_ = nullptr;
  std::uint32_t sr_ = 44100;
};

}  // namespace deckhand
