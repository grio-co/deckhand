#pragma once
// Vocal position detection: where the singing is, found from a separated vocals stem.
// The core is a streaming detector over PCM (pure, no I/O); detect_vocals_in_cache() feeds it from a cached stem file
// (src/stems, .dhstems). No model runs here: only stems that already exist are read.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace deckhand {

struct VocalRegion {
  double start_ms = 0, end_ms = 0;
};

struct VocalOptions {
  double frame_ms = 20;            // level measurement window
  double floor_db = -52;           // a stem whose loud parts stay below this has no vocals at all
  double on_below_peak_db = 22;    // a region starts when a frame is within this many dB of the stem's loud level
  double hysteresis_db = 6;        // ... and only ends when the level has been this much lower than the start threshold
  double hold_ms = 450;            // dips shorter than this (breaths, gaps between words) never split a region
  double merge_gap_ms = 700;       // regions closer than this are one
  double min_length_ms = 900;      // shorter regions are dropped (a stray syllable, stem bleed)
  double pad_ms = 0;               // widen each region by this much on both sides
};

// Streaming detector: feed the vocals stem in any chunking (mono, or the mean of left and right), then finish().
// The loud level is taken from the whole stem, so the thresholds are decided in finish().
class VocalDetector {
 public:
  VocalDetector(int sample_rate, const VocalOptions& options = {});
  void add(const float* mono, std::size_t frames);
  void add_stereo(const float* left, const float* right, std::size_t frames);  // right may be null
  std::vector<VocalRegion> finish();

 private:
  VocalOptions o_;
  int rate_;
  std::size_t frame_len_;
  double acc_ = 0;
  std::size_t in_frame_ = 0;
  std::vector<float> db_;  // one level per frame (dBFS)
};

// Interleaved PCM (channels >= 1; channels are averaged).
std::vector<VocalRegion> detect_vocal_regions(const float* interleaved, std::size_t frames, int channels, int sample_rate,
                                              const VocalOptions& options = {});

// Reads the vocals stem of the track with cache key `key` from a stem cache folder (the `root` of its CacheConfig) and
// detects. Returns false with `error` set when the entry is missing or unreadable. Works with or without a separation
// model in the build (DECKHAND_WITH_STEMS=OFF only removes the model, not the cache reader).
bool detect_vocals_in_cache(const std::string& cache_root, const std::string& key, std::vector<VocalRegion>& out,
                            std::string* error = nullptr, const VocalOptions& options = {});

}  // namespace deckhand
