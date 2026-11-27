#pragma once
// Random-access sample sources: an in-memory buffer (a decoded track) or a WAV file on disk. Splitting, clean-up and
// export read through this so a long vinyl side never has to sit in memory whole.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "deckhand/capture/wav.hpp"

namespace deckhand::capture {

class SampleSource {
 public:
  virtual ~SampleSource() = default;
  virtual std::uint32_t channels() const = 0;
  virtual std::uint32_t sample_rate() const = 0;
  virtual std::uint64_t frames() const = 0;
  // Reads up to `count` interleaved frames from `start`; returns the number read.
  virtual std::uint64_t read(std::uint64_t start, std::uint64_t count, float* out) = 0;
};

// Borrows the buffer (it must outlive the source).
class MemorySource : public SampleSource {
 public:
  MemorySource(const float* data, std::uint64_t frames, std::uint32_t channels, std::uint32_t rate)
      : data_(data), frames_(frames), ch_(channels), rate_(rate) {}
  std::uint32_t channels() const override { return ch_; }
  std::uint32_t sample_rate() const override { return rate_; }
  std::uint64_t frames() const override { return frames_; }
  std::uint64_t read(std::uint64_t start, std::uint64_t count, float* out) override;

 private:
  const float* data_;
  std::uint64_t frames_;
  std::uint32_t ch_, rate_;
};

class WavSource : public SampleSource {
 public:
  bool open(const std::string& path, std::string& error) { return reader_.open(path, error); }
  std::uint32_t channels() const override { return reader_.info().fmt.channels; }
  std::uint32_t sample_rate() const override { return reader_.info().fmt.sample_rate; }
  std::uint64_t frames() const override { return reader_.info().frames; }
  std::uint64_t read(std::uint64_t start, std::uint64_t count, float* out) override { return reader_.read(start, count, out); }

 private:
  WavReader reader_;
};

}  // namespace deckhand::capture
