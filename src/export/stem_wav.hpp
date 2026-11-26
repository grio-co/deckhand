#pragma once
// Internal: reading ".dhstems" files and rendering stem mixes to WAV (used by export.cpp and export_stems.cpp).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "bytes.hpp"
#include "deckhand/export/export.hpp"

namespace deckhand::usbexport::detail {

// Layout (see stems/cache.hpp): 128-byte header | stem-major, channel-major planar PCM (16 or 24 bit LE; one float
// scale in the header) | 16-byte trailer. 4 stems (vocals, drums, bass, other), 2 channels.
class StemFile {
 public:
  StemFile() = default;
  ~StemFile();
  StemFile(const StemFile&) = delete;
  StemFile& operator=(const StemFile&) = delete;
  bool open(const std::string& path, std::string& error);
  int sample_rate() const { return rate_; }
  int bits() const { return bits_; }
  std::uint64_t frames() const { return frames_; }
  // Frames past the end read as zeros. false on I/O error.
  bool read(int stem, int channel, std::uint64_t start, std::size_t n, float* out);
  // Sum of the stems in `mask` (bit i = stem i), as doubles, for frames [start, start+n).
  bool read_mix(unsigned mask, std::uint64_t start, std::size_t n, std::vector<double>& l, std::vector<double>& r);

 private:
  std::FILE* f_ = nullptr;
  int rate_ = 0, bits_ = 16;
  float scale_ = 1;
  std::uint64_t frames_ = 0;
};

unsigned stem_mask_of(unsigned single_mix);  // StemMix -> stem bits

std::uint64_t wav_bytes(std::uint64_t frames, int bits);
Bytes wav_header(int rate, int bits, std::uint64_t frames);

// Renders the mix to `dest` (24- or 16-bit stereo PCM WAV, clamped, no dither).
bool render_stem_wav(const StemRender& r, const std::string& dest, std::string& error);
// True when `existing` is byte-identical to what render_stem_wav would write.
bool stem_wav_matches(const StemRender& r, const std::string& existing);
// True when `path` is a plain PCM WAV (44-byte header) of this rate/bits, stereo, consistent sizes: the shape of
// the files render_stem_wav writes.
bool looks_like_stem_wav(const std::string& path, int rate, int bits);
// Three-band 150 points/s waveform of the mix: same filters and scaling as the application's build_waveform.
bool stem_waveform(const StemRender& r, std::vector<WaveSample>& out, std::string& error);

}  // namespace deckhand::usbexport::detail
