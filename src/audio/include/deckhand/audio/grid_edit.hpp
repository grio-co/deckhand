#pragma once
// Beat grid editing: pure functions from one grid to the next, so the application, the API and the tests share them.
// A grid is a list of tempo sections in frames of the track's own sample rate (see GridSection). Every function
// returns a normalised grid (sorted, first beat >= 0) or, when the edit makes no sense, the grid unchanged.
#include <vector>

#include "deckhand/audio/engine.hpp"

namespace deckhand::gridedit {

using Grid = std::vector<GridSection>;

constexpr double kMinBpm = 30, kMaxBpm = 300;
constexpr int kMaxSections = 256;

// Sorted, invalid sections dropped, tempos clamped, and the first beat moved forward by whole beats until it is >= 0.
Grid normalise(Grid g, double sample_rate);
// Index of the section whose beats cover frame `at` (0 for an empty grid is undefined: check empty() first).
int section_at(const Grid& g, double at);
// Frame of the grid beat nearest to `at`, or -1 for an empty grid.
double nearest_beat(const Grid& g, double at, double sample_rate);
// Bar position (1..4) of the grid beat nearest to `at`; 0 for an empty grid.
int bar_position_at(const Grid& g, double at, double sample_rate);
// Beat frames inside [from, to] and their bar positions, for drawing.
struct BeatMark {
  double frame;
  int bar_pos;  // 1..4
};
void beats_in(const Grid& g, double from, double to, double length, double sample_rate, std::vector<BeatMark>& out);

Grid shift(const Grid& g, double frames, double sample_rate);                 // every beat moves by `frames`
Grid align(const Grid& g, double at, double sample_rate);                      // the beat nearest `at` moves onto `at`
Grid set_downbeat(const Grid& g, double at, double sample_rate);               // the beat nearest `at` becomes bar beat 1
Grid rotate_downbeat(const Grid& g, int steps);                                // which beat is bar 1 moves on by `steps`
Grid set_first_beat(const Grid& g, double at, double sample_rate);             // align() and set_downbeat() in one
// The section at `at` gets `bpm`. The first section pivots around the beat nearest `at` (that beat keeps its place); a later
// section keeps its start, which is where the tempo changes.
Grid set_bpm(const Grid& g, double at, double bpm, double sample_rate);
Grid bpm_delta(const Grid& g, double at, double delta, double sample_rate);
// Every section's tempo times `factor` (0.5 halves, 2 doubles); first beats keep their place.
Grid scale_bpm(const Grid& g, double factor, double sample_rate);
// A new tempo section starting at `at` (same tempo, bars continuing); `at` within a frame of an existing start does nothing.
Grid add_section(const Grid& g, double at, double sample_rate);
// Removes the section covering `at`; the previous one stretches over it. The last section is kept.
Grid remove_section(const Grid& g, double at, double sample_rate);
// Tempo from tap times in seconds (monotonic). Uses the taps after the last gap > 2.5 s, at most the last 8; 0 when
// fewer than 3 taps (two intervals) remain.
double tap_bpm(const std::vector<double>& times);
// Same grids (within a thousandth of a frame / a thousandth of a BPM)?
bool same(const Grid& a, const Grid& b);

}  // namespace deckhand::gridedit
