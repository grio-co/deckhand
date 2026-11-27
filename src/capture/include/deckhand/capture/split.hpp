#pragma once
// Track splitting for a vinyl side: finds the gaps between tracks in the presence of surface noise, clicks and rumble.
//
// Two stages so the caller can re-run cheaply: analyse_splits() reads the audio once and builds a click-robust level
// envelope (10 ms hops); find_splits() decides from the envelope and the settings in milliseconds. The result is a list of
// SplitPoints (cut positions, with confidence) that the caller edits with SplitPlan (add / move / remove) and turns into
// pieces with plan_pieces().
#include <cstdint>
#include <vector>

#include "deckhand/capture/source.hpp"

namespace deckhand::capture {

struct SplitSettings {
  double min_gap_s = 1.0;        // shortest stretch of groove noise that counts as a gap between tracks
  double min_track_s = 20.0;     // shorter pieces are merged (the weaker neighbouring gap is dropped)
  double min_depth_db = 15.0;    // a gap must be at least this far below the programme level
  double adapt_db = 12.0;        // gap level is at most this far above the side's noise floor
  double max_gap_spread_db = 4.0;// a gap is stationary: its level may not wander more than this (inter-quartile)
  double min_confidence = 0.25;  // weaker gaps are not reported
  double onset_db = 6.0;         // audible = this far above the surrounding noise (sustained)
  double fade_db = 2.0;          // fade tails and quiet intros count while this far above the noise...
  double fade_tolerance_s = 3.0; // ...for at most this long beyond the onset / before the end
  double lead_in_pad_s = 0.05;   // kept before the first audible sound of a track
  double lead_out_pad_s = 0.25;  // kept after the last audible sound
  double rumble_hz = 100.0;      // the envelope ignores everything below this (turntable rumble, warp)
};

struct SplitLevels {
  double floor_db = -100, programme_db = -100, gap_threshold_db = -100;
  bool splittable = false;       // false: the side has no gaps deep enough (a continuous recording)
};

struct SplitAnalysis {
  std::uint32_t sample_rate = 0;
  std::uint64_t frames = 0;
  std::uint32_t hop_frames = 0;
  std::vector<float> env_db;     // one value per hop
  double hop_seconds() const { return sample_rate ? static_cast<double>(hop_frames) / sample_rate : 0; }
};

struct SplitPoint {
  enum class Origin { Auto, User };
  std::uint64_t frame = 0;       // where to cut
  double confidence = 1.0;       // 0..1; user points are 1
  Origin origin = Origin::Auto;
  std::uint64_t gap_start = 0, gap_end = 0;  // the silent stretch around the cut (audible end of the previous track / start of the next)
  double noise_db = -100;        // groove noise level in the gap
};

struct Piece {
  std::uint64_t start = 0, end = 0;  // [start, end) in frames
  bool empty = false;                // nothing audible (lead-in or run-out groove only)
  double confidence = 1.0;           // of the split point that starts it (1 for the first)
};

SplitAnalysis analyse_splits(SampleSource& src, const SplitSettings& s = {});
SplitLevels split_levels(const SplitAnalysis& a, const SplitSettings& s = {});
std::vector<SplitPoint> find_splits(const SplitAnalysis& a, const SplitSettings& s = {});

// Audible part of [from, to) with pads, trimming lead-in and lead-out noise. empty when nothing audible.
bool audible_range(const SplitAnalysis& a, const SplitSettings& s, std::uint64_t from, std::uint64_t to, std::uint64_t& start, std::uint64_t& end);

// The quietest 10 ms hop within `radius_s` of `frame` (for dropping a user point on the groove gap).
std::uint64_t nearest_quiet_frame(const SplitAnalysis& a, std::uint64_t frame, double radius_s = 1.0);

// `contiguous`: pieces tile the audible range with no frame lost between them (cut at the split points, only the first
// start and the last end trimmed), so exports concatenate bit-exactly. Otherwise each piece is trimmed on both sides.
std::vector<Piece> plan_pieces(const SplitAnalysis& a, const SplitSettings& s, const std::vector<SplitPoint>& points, bool contiguous);

// Editable list. Re-running keeps what the user did: user points stay, and an automatic point the user removed is not
// brought back by a later run.
struct SplitPlan {
  std::vector<SplitPoint> points;          // sorted by frame
  std::vector<std::uint64_t> vetoes;       // frames of removed automatic points
  double veto_radius_s = 1.0;
  std::uint32_t sample_rate = 44100;

  void set_auto(const std::vector<SplitPoint>& found);  // replaces the automatic points (the re-run)
  std::size_t add(std::uint64_t frame);                 // returns the index it landed at
  bool move(std::size_t index, std::uint64_t frame);    // becomes a user point
  bool remove(std::size_t index);
  void clear_user_edits() { vetoes.clear(); }
};

}  // namespace deckhand::capture
