#pragma once
// A recording from an analogue input. The caller pushes interleaved float blocks (later the engine's input callback);
// the session writes a crash-safe WAV (24-bit or float, RF64 beyond 4 GB) and keeps level statistics. No device is
// opened here: this is the offline core.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "deckhand/capture/wav.hpp"

namespace deckhand::capture {

struct CaptureConfig {
  std::string path;
  std::uint32_t sample_rate = 44100;  // the rate of the blocks that will be pushed (any rate; no conversion is done)
  std::uint16_t channels = 2;
  SampleFormat format = SampleFormat::Pcm24;
  double flush_seconds = 1.0;         // the header and stdio are refreshed this often (at most this much audio is at risk)
  bool async = true;                  // push() only fills a lock-free ring and a writer thread does the file I/O and the statistics
  double ring_seconds = 10.0;         // async ring size; a slow disk beyond this drops frames (counted)
  double clip_level = 0.9999;         // |sample| at or above this counts as clipped
  std::uint64_t rf64_threshold = kRf64Threshold;
};

struct CaptureStats {
  std::uint64_t frames = 0;           // written to the file so far
  double seconds = 0;
  std::vector<float> peak;            // per channel, linear
  float peak_all = 0;
  double rms = 0;                     // linear, all channels
  double rms_db = -200, peak_db = -200;
  std::uint64_t clip_samples = 0;
  std::vector<double> dc;             // per channel mean (DC offset)
  std::uint64_t dropped_frames = 0;   // lost to a full ring
  bool write_error = false;
  bool paused = false;
};

// Level since the last take_meter() call (for a live meter).
struct CaptureMeter {
  std::vector<float> peak;
  float rms = 0;
  std::uint64_t clip_samples = 0;
};

class CaptureSession {
 public:
  CaptureSession() = default;
  CaptureSession(const CaptureSession&) = delete;
  CaptureSession& operator=(const CaptureSession&) = delete;
  ~CaptureSession() { std::string e; stop(e); }

  // Creates the file (overwrites). Not real-time.
  bool start(const CaptureConfig& cfg, std::string& error);
  // Continues an earlier recording (clean or interrupted): repairs the header, drops a cut final frame, appends. Format
  // and rate come from the file; statistics are rebuilt by reading it back when `rescan` is set.
  bool resume(const std::string& path, std::string& error, bool rescan = true, const CaptureConfig* options = nullptr);

  // Producer side (the audio thread in async mode: wait-free, no allocation). Returns false when frames were dropped.
  bool push(const float* interleaved, std::uint64_t frames);
  void set_paused(bool paused) { paused_.store(paused); }  // paused blocks are discarded (between sides, say)

  bool stop(std::string& error);       // drains, writes the final header, closes
  bool is_open() const { return open_; }
  const std::string& path() const { return cfg_.path; }
  const CaptureConfig& config() const { return cfg_; }

  CaptureStats stats() const;
  CaptureMeter take_meter();
  void flush();                        // async: asks the writer to refresh the header now

 private:
  bool open_common(std::string& error);
  void consume(const float* block, std::uint64_t frames);  // statistics + file write (writer thread, or the caller in sync mode)
  void run();
  std::uint64_t pop(float* out, std::uint64_t frames);

  CaptureConfig cfg_;
  WavWriter writer_;
  bool open_ = false;
  std::atomic<bool> paused_{false}, stopping_{false}, flush_req_{false};
  std::thread thread_;

  std::vector<float> ring_;
  std::uint64_t cap_ = 0;
  std::atomic<std::uint64_t> head_{0}, tail_{0}, dropped_{0};

  mutable std::mutex mu_;  // statistics (writer thread vs readers)
  CaptureStats st_;
  std::vector<double> sum_;
  double sum_sq_ = 0;
  CaptureMeter meter_;
  double meter_sq_ = 0;
  std::uint64_t meter_frames_ = 0, since_flush_ = 0;
};

}  // namespace deckhand::capture
