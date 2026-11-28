#pragma once
// FLAC encoder written from the public format specification (RFC 9639). Streaming: push blocks of samples, close() rewrites
// STREAMINFO (total length, frame sizes, MD5 of the unencoded samples) and the SEEKTABLE, so a long recording is never held
// in memory twice. 4 to 24 bit, 1 to 8 channels, fixed-size frames (default 4096, the last one short), per-frame choice
// between independent / left-side / right-side / mid-side for stereo, constant / verbatim / fixed / LPC subframes with
// Rice-coded residuals (partition order search) and wasted-bits detection.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "deckhand/capture/wav.hpp"

namespace deckhand::capture {

// RFC 1321 MD5 (used for the STREAMINFO signature of the unencoded samples).
class Md5 {
 public:
  Md5();
  void update(const void* data, std::size_t n);
  std::array<std::uint8_t, 16> finish();  // the object must not be updated afterwards

 private:
  void block(const std::uint8_t* p);
  std::uint32_t s_[4];
  std::uint64_t len_ = 0;
  std::uint8_t buf_[64];
  std::size_t fill_ = 0;
};

struct FlacParams {
  std::uint32_t sample_rate = 44100;  // 1 .. 1048575
  std::uint32_t channels = 2;         // 1 .. 8
  std::uint32_t bits = 24;            // 4 .. 24 (16 and 24 are the ones the exporters use)
  std::uint32_t block_size = 4096;    // 16 .. 65535, every frame but the last
  int level = 5;                      // 0 = fixed predictors only, 1-4 = LPC up to order 8, 5-8 = LPC up to order 12 with more orders tried
  std::uint64_t total_frames_hint = 0;  // sizes the SEEKTABLE (one point about every 10 s); 0 = reserve room for 512 points
  std::uint32_t padding_bytes = 2048;   // trailing PADDING block so tags can be edited in place
};

class FlacWriter {
 public:
  FlacWriter();
  ~FlacWriter();  // closes the file without finalising if close() was not called
  FlacWriter(const FlacWriter&) = delete;
  FlacWriter& operator=(const FlacWriter&) = delete;

  // Creates `path` and writes the metadata (STREAMINFO placeholder, SEEKTABLE placeholder, VORBIS_COMMENT from `tags`, PADDING).
  bool create(const std::string& path, const FlacParams& p, const TrackTags* tags, std::string& error);
  // Interleaved integer samples, each within the signed `bits` range (values outside are clamped). `frames` are sample frames.
  bool write_int(const std::int32_t* interleaved, std::uint64_t frames);
  // Interleaved float samples (-1..1), converted exactly as the WAV writer does (round to nearest, 16: x32767, 24: x8388607).
  bool write_float(const float* interleaved, std::uint64_t frames);
  // Encodes the short final block, rewrites STREAMINFO and SEEKTABLE, closes the file.
  bool close();

  const std::string& error() const { return error_; }
  std::uint64_t frames_written() const;
  std::uint64_t bytes_written() const;  // file size so far

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
  std::string error_;
};

// The same float -> integer conversion the writer uses (for tests and callers that convert themselves).
std::int32_t flac_quantise(float x, std::uint32_t bits);

}  // namespace deckhand::capture
