#pragma once
// Runs the analysis module on a decoded track and shapes the result for the library.
#include <string>
#include <vector>

#include "deckhand/audio/track.hpp"
#include "deckhand/library/library.hpp"

namespace deckhand {

struct TrackAnalysis {
  bool has_grid = false, has_key = false, has_gain = false;
  std::vector<BeatSection> grid;
  double bpm = 0;
  std::string key;     // wheel notation, e.g. "8A"
  double gain_db = 0;  // trim that brings the track to the reference loudness
};

TrackAnalysis analyse_track(const TrackAudio& audio);

}  // namespace deckhand
