#pragma once
// Runs the analysis module on a decoded track and shapes the result for the library.
#include <string>
#include <vector>

#include "deckhand/analysis/analysis.hpp"
#include "deckhand/audio/track.hpp"
#include "deckhand/library/library.hpp"

namespace deckhand {

struct TrackAnalysis {
  bool has_grid = false, has_key = false, has_gain = false;
  std::vector<BeatSection> grid;
  double bpm = 0;
  std::string key;     // wheel notation, e.g. "8A"
  double gain_db = 0;  // trim that brings the track to the reference loudness
  bool has_audible = false;
  double first_audible_ms = 0, last_audible_ms = 0;
  double suggested_cue_ms = 0;  // load cue: first confident downbeat at or after the first sound
  bool has_phrases = false;     // phrase analysis succeeded (a track too short or silent has none)
  StoredPhrases phrases;        // shaped for Library::set_phrases
};

// Phrase data between the analysis module and the library's stored form.
StoredPhrases to_stored_phrases(const PhraseAnalysis& p);
PhraseAnalysis from_stored_phrases(const StoredPhrases& s);

TrackAnalysis analyse_track(const TrackAudio& audio);

// Where a track starts when it is loaded, in ms: the user's own main cue, else the analysis suggestion according to the
// setting `load_cue_mode` (first_downbeat (default) | first_sound | file_start), else 0. A track analysed before load cues
// existed gets a cheap level scan for its first sound here (no beat or key analysis) and keeps the result.
double initial_cue_ms(Library& lib, std::int64_t track_id, const TrackAudio& audio);

}  // namespace deckhand
