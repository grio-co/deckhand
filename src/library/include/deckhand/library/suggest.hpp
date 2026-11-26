#pragma once
// "What goes well after this?": a transparent score of every library track against a seed track.
// Every result carries its breakdown, so the interface can say why a track is suggested.
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace deckhand {

// Points each factor can add (the base factors share 85 points between them, scaled over the factors that have data).
// History, exploration and recency are added to or taken from the result. Adjustable in the settings as
// "suggest.w.<name>" (see load_suggest_weights()).
struct NextTrackWeights {
  double tempo = 30, key = 25, energy = 15, genre = 10, rating = 5;  // base factors
  double history = 25;   // played right after the seed before (at full: three or more times)
  double explore = 3;    // never played at all
  double recent = 40;    // played earlier in this session (most recent at full)
};

struct NextTrackOptions {
  double max_pitch = 0.06;          // largest tempo change (fraction) that is accepted to match the seed
  bool half_double = true;          // half time and double time count as a match
  std::int64_t playlist_id = 0;     // restrict the candidates to this playlist (0 = whole library)
  std::set<std::int64_t> exclude;   // queued tracks and the like (the seed itself is always excluded)
  std::vector<std::int64_t> recent; // tracks played in this session, newest first (pushed down)
  bool harmonic_only = true;        // leave out tracks whose key does not mix (unknown keys stay in)
  bool recent_from_history = true;  // `recent` empty: use the newest history session instead
  std::size_t limit = 20;
  NextTrackWeights weights;
};

struct NextTrackFactor {
  std::string id;       // tempo | key | energy | genre | rating | history | explore | recent
  double points = 0;    // contribution to the score (negative for recent)
  double value = 0;     // the factor's own 0..1 value
  double detail = 0;    // tempo: BPM difference after the match; energy: outro of the seed minus intro of the candidate;
                        // history: times played in that order; recent: how many tracks ago
  std::string tag;      // tempo: direct | half | double; key: same | adjacent | relative | boost; genre: the shared genre
};

struct NextTrack {
  std::int64_t track_id = 0;
  double score = 0;
  double pitch_change = 0;          // fraction the candidate must be changed by to match the seed (signed)
  int pair_count = 0;               // times the user played the candidate right after the seed
  int reverse_count = 0;            // ... the other way round
  std::vector<NextTrackFactor> factors;
};

struct EnergySummary {
  double avg = 0, intro = 0, outro = 0;  // 0..1
};

}  // namespace deckhand
