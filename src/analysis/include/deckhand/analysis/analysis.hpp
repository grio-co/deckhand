#pragma once
// Track analysis: beat grid (variable tempo), downbeats, musical key and loudness.
// Input is decoded PCM (interleaved stereo float). Every function is pure: no global state,
// safe to call from any background thread, and none throws.
#include <array>
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
  bool phrases = true;               // phrase segmentation (analyse() only; needs ~30 s of audible audio)
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

// ---- phrase structure -----------------------------------------------------------------------------------------
// One section of the track. Bars are counted from the origin downbeat (bar 0 = first downbeat at/after the first
// sound; a pickup before it is not a bar). start_bar/end_bar are -1 when the track had no usable grid.
struct PhraseSection {
  double start_ms = 0;
  double end_ms = 0;
  int start_bar = -1;
  int end_bar = -1;                  // exclusive
  int bars = 0;                      // 0 when ungridded
  int energy = 5;                    // 1..10, relative to this track's own range
  // Rich guess: intro, build, drop, breakdown, verse, chorus, bridge, outro. Heuristic; trust the boundary more
  // than the label (label_confidence 0..1).
  std::string label;
  double label_confidence = 0;
  // Safe energy-style label: intro, up, down, chorus (= high energy) or outro.
  std::string kind;
  double boundary_confidence = 0;    // 0..1, strength of the change at start_ms (1 for the first section)
  bool phrase_aligned = false;       // start_bar is a multiple of 4
  double kick = 0;                   // fraction of beats with a kick, 0..1 (-1: not measured, ungridded)
};

struct PhraseAnalysis {
  bool ok = false;
  std::string error;                 // "track too short for phrase analysis", ...
  bool gridded = false;              // sections are on the bar grid
  bool time_based = false;           // no usable grid: 2 s blocks, no bar snapping (positions are approximate)
  double origin_ms = 0;              // bar 0 (first downbeat after the first sound; first sound when ungridded)
  double pickup_ms = 0;              // first sound -> origin when the music starts before the first downbeat
  double bar_ms = 0;                 // mean bar length (2000 when ungridded)
  int total_bars = 0;                // bars covered (0 when ungridded)
  std::vector<double> bar_starts_ms; // total_bars + 1 entries (block starts + end when ungridded)
  std::vector<float> bar_energy;     // 0..1 per bar/block, for drawing an energy strip under the waveform
  std::vector<PhraseSection> sections;
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
  PhraseAnalysis phrases;            // filled by analyse() when options.phrases (ok=false when too short / silent)
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

// ---- phrase analysis and cue suggestions ---------------------------------------------------------------------
// analyse() already fills AnalysisResult::phrases; this is for callers that have a grid and the audible range.
PhraseAnalysis analyse_phrases(const float* pcm, std::uint64_t frames, std::uint32_t sample_rate, const BeatGrid& grid,
                               double first_audible_ms, double last_audible_ms, const AnalysisOptions& options = {});

// Summary of where a user puts cues relative to phrase starts, built from tracks they already cued.
struct CueHabit {
  int samples = 0;                        // cues summarised
  std::array<int, 33> bar_offset_hist{};  // index = offset + 16: bars from the nearest phrase start (negative = before)
  int drop_samples = 0;                   // tracks with a first drop and a cue within 16 bars before it
  double mean_bars_before_drop = 0;       // mean of those distances in whole bars (positive = before the drop)
};
struct HabitSample {
  PhraseAnalysis phrases;
  std::vector<double> cue_ms;             // the user's existing cue positions on that track
};
CueHabit build_cue_habit(const std::vector<HabitSample>& samples);

struct CueOptions {
  int max_hot_cues = 8;                   // hot-cue suggestions returned (memory candidates come on top)
  int mix_out_bars = 16;                  // 8, 16 or 32: the mix-out phrase must leave at least this many bars
  bool memory_cues = true;                // add a memory-cue candidate at every phrase boundary
  bool use_habit = false;
  CueHabit habit;
  double habit_strength = 1.0;            // 0..1 share of the user's habitual offset applied
  int habit_min_samples = 5;              // below this the habit is ignored
};

struct CueSuggestion {
  double position_ms = 0;                 // on a downbeat at a phrase start (shifted by whole bars when a habit applies)
  std::string type;                       // load, mix_in, first_drop, breakdown, last_drop, mix_out, outro, memory
  std::string name;                       // display name
  int color = 0;                          // palette index 0..7: load 0, mix_in 1, drop 2, breakdown 3, last_drop 4, mix_out 5, outro 6, memory 7
  int rank = 0;                           // 1 = most useful; hot cues first, then memory candidates by position
  bool memory = false;                    // memory-cue candidate (phrase boundary) rather than a hot cue
  double confidence = 0;                  // 0..1
  int bar = -1;                           // bar index from the origin downbeat (-1 ungridded)
  double anchor_ms = 0;                   // the phrase start this cue belongs to (differs from position when shifted)
  int bar_shift = 0;                      // whole bars applied by the habit (negative = earlier)
};
// Deterministic. Returns hot cues sorted by rank, then memory candidates. Empty when there are no phrases.
std::vector<CueSuggestion> suggest_cues(const PhraseAnalysis& phrases, double first_audible_ms, double last_audible_ms,
                                        const CueOptions& options = {});
std::vector<CueSuggestion> suggest_cues(const AnalysisResult& result, const CueOptions& options = {});

}  // namespace deckhand
