#pragma once
// "What goes well after this?": the application side of Library::suggest(): which deck is on air, the user's settings,
// exclusions from the queue and this session, and the JSON the API returns.
#include <cstdint>
#include <string>
#include <vector>

// Without the REST API (DECKHAND_WITH_API=0, e.g. iOS) there is no JSON layer: only the model stays.
#ifndef DECKHAND_WITH_API
#define DECKHAND_WITH_API 1
#endif
#if DECKHAND_WITH_API
#include "deckhand/api/json.hpp"
#endif
#include "deckhand/app/play_queue.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/library/library.hpp"

namespace deckhand {

struct NextSettings {
  double max_pitch = 0.06;          // setting "suggest.max_pitch" in percent
  bool half_double = true;          // "suggest.half_double"
  bool harmonic_only = true;        // "suggest.harmonic_only"
  bool exclude_queued = true;       // "suggest.exclude_queued"
  bool exclude_played = false;      // "suggest.exclude_played": also leave out what was played in this session
  bool follow_master = true;        // "suggest.follow": the pane follows the deck on air (else the browser selection)
};
NextSettings next_settings(Library& lib);
void save_next_settings(Library& lib, const NextSettings& s);

// The deck (0-based) whose track is on air: the playing deck that is loudest at the master, else the loaded deck that
// plays, else -1. Decks without a track are never chosen.
int master_deck(const IEngine& engine);

NextTrackOptions next_options(Library& lib, const PlayQueue* queue, const NextSettings& settings, std::int64_t playlist_id = 0,
                              std::size_t limit = 20);

// {"seed": {...}, "items": [{"track": {...}, "score", "pitch_percent", "pair_count", "reverse_count", "factors": [...]}]}
#if DECKHAND_WITH_API
api::Json next_tracks_json(Library& lib, std::int64_t seed_id, const std::vector<NextTrack>& items);
#endif

}  // namespace deckhand
