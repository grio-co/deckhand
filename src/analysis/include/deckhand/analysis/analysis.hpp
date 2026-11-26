#pragma once
// Track analysis: beat grid (variable tempo), downbeats, musical key and loudness.
// Input is decoded PCM (interleaved stereo float). Every function is pure: no global state,
// safe to call from any background thread, and none throws.
#include <cstdint>
#include <string>
#include <vector>

namespace deckhand {

// One constant-tempo stretch of the beat grid.
struct TempoSection {
  double first_beat_ms = 0;  // time of the first beat of this section
  double bpm = 0;            // snapped to 0.01 (and to an integer when within 0.05)
  int bar_beat = 1;          // 1..4: position in the bar of that first beat (1 = downbeat)
};

struct AnalysisOptions {
  bool beats = true;
  bool key = true;
  bool loudness = true;
  double min_bpm = 60.0;             // tempo search range
  double max_bpm = 200.0;
  double preferred_min_bpm = 78.0;   // octave errors are resolved towards this range
  double preferred_max_bpm = 176.0;
  bool fluid_grid = true;            // follow tempo drift/changes with extra sections (off: coarse segmentation as before)
};

struct BeatGrid {
  bool ok = false;
  std::string error;                 // set when !ok
  std::vector<TempoSection> sections; // one for steady tempo, several when tempo changes/drifts
  std::vector<double> beats_ms;      // every beat of the grid, from the first beat >= 0 to the end
  double bpm = 0;                    // tempo of the longest section
  double confidence = 0;             // 0..1 (how well beats fit the grid)
  int downbeat_phase = 0;            // beats_ms[i] is a downbeat when i % 4 == downbeat_phase
  double downbeat_confidence = 0;    // 0..1
  bool steady = true;                // judged one steady tempo (a single section); false = fluid (several sections)
  double stability = 1.0;            // 0..1: 1 = metronomic, lower = the tempo wanders / the grid needed many sections
};

struct KeyResult {
  bool ok = false;
  std::string error;
  int key_index = -1;                // 0..11 major (C..B), 12..23 minor (C..B)
  std::string name;                  // "Am", "F#"
  std::string wheel;                 // "8A", "4B" (Camelot style)
  double confidence = 0;             // 0..1
  double tuning_cents = 0;           // detected tuning offset from A=440
};

struct LoudnessResult {
  bool ok = false;
  std::string error;
  double integrated_lufs = -70.0;    // ITU-R BS.1770 K-weighted, gated
  double sample_peak = 0;            // linear, max |sample|
  double sample_peak_dbfs = -200.0;
};

struct AnalysisResult {
  bool ok = false;                   // every requested part succeeded
  std::string error;                 // first failure, if any
  double duration_ms = 0;
  BeatGrid beats;
  KeyResult key;
  LoudnessResult loudness;
  // Audible extent and load cue (always filled by analyse(), whatever options are set).
  // has_audible is false for digital silence / pure noise floor; the three times are then 0.
  bool has_audible = false;
  double first_audible_ms = 0;       // first moment the track is really audible ("the start"), 0..duration
  double last_audible_ms = 0;        // end of the last audible sound (>= first_audible_ms)
  double suggested_cue_ms = 0;       // load cue: first confident downbeat at/after first sound, else first sound
};

struct AudibleRange {
  bool ok = false;
  double first_ms = 0;
  double last_ms = 0;
};
// Level-based start/end of the audible part. Threshold is relative to the track's own loudness (about -30 dB
// below its loud peaks, never below -56 dBFS) and must hold for a few ms, so a lone click/pop, dither or a quiet
// noise-floor lead-in does not count; DC offset is removed; a fade-in counts from where it becomes clearly audible.
AudibleRange find_audible_range(const float* pcm, std::uint64_t frames, std::uint32_t sample_rate);

// `pcm` is interleaved stereo, `frames` sample frames long.
BeatGrid analyse_beats(const float* pcm, std::uint64_t frames, std::uint32_t sample_rate,
                       const AnalysisOptions& options = {});
KeyResult detect_key(const float* pcm, std::uint64_t frames, std::uint32_t sample_rate);
LoudnessResult measure_loudness(const float* pcm, std::uint64_t frames, std::uint32_t sample_rate);
AnalysisResult analyse(const float* pcm, std::uint64_t frames, std::uint32_t sample_rate,
                       const AnalysisOptions& options = {});

// Helpers.
std::string key_name(int key_index);      // "C", "Am", ... ("" when out of range)
std::string key_wheel(int key_index);     // "8B", "8A", ...
// Gain in dB to bring the track to `target_lufs` without the sample peak exceeding `ceiling_dbfs`.
double auto_gain_db(const LoudnessResult& loudness, double target_lufs = -14.0,
                    double ceiling_dbfs = -1.0);

}  // namespace deckhand
