#include "deckhand/app/next_tracks.hpp"

#include <algorithm>
#include <cstdlib>

namespace deckhand {

namespace {
bool flag(Library& lib, const char* key, bool fallback) {
  const std::string v = lib.setting(key);
  return v.empty() ? fallback : v == "1";
}

#if DECKHAND_WITH_API
api::Json track_summary(const TrackRow& t) {
  api::Json j = api::Json::object();
  j.set("id", t.id);
  j.set("title", t.title);
  j.set("artist", t.artist);
  j.set("genre", t.genre);
  j.set("key", t.key);
  j.set("bpm", t.bpm);
  j.set("rating", t.rating);
  j.set("duration_ms", t.duration_ms);
  return j;
}
#endif
}  // namespace

NextSettings next_settings(Library& lib) {
  NextSettings s;
  const std::string p = lib.setting("suggest.max_pitch");
  if (!p.empty()) {
    const double v = std::atof(p.c_str());
    if (v >= 0.5 && v <= 50) s.max_pitch = v / 100.0;
  }
  s.half_double = flag(lib, "suggest.half_double", s.half_double);
  s.harmonic_only = flag(lib, "suggest.harmonic_only", s.harmonic_only);
  s.exclude_queued = flag(lib, "suggest.exclude_queued", s.exclude_queued);
  s.exclude_played = flag(lib, "suggest.exclude_played", s.exclude_played);
  s.follow_master = flag(lib, "suggest.follow", s.follow_master);
  return s;
}

void save_next_settings(Library& lib, const NextSettings& s) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", s.max_pitch * 100.0);
  lib.set_setting("suggest.max_pitch", buf);
  lib.set_setting("suggest.half_double", s.half_double ? "1" : "0");
  lib.set_setting("suggest.harmonic_only", s.harmonic_only ? "1" : "0");
  lib.set_setting("suggest.exclude_queued", s.exclude_queued ? "1" : "0");
  lib.set_setting("suggest.exclude_played", s.exclude_played ? "1" : "0");
  lib.set_setting("suggest.follow", s.follow_master ? "1" : "0");
}

int master_deck(const IEngine& engine) {
  const MixerView mx = engine.mixer();
  int best = -1;
  double best_gain = -1;
  for (int d = 0; d < kMaxDecks; ++d) {
    const DeckView v = engine.deck(d);
    if (!v.loaded || v.track_id <= 0 || !v.playing) continue;
    const double xf = (d % 2 == 0) ? std::min(1.0, 2.0 * (1.0 - mx.crossfader)) : std::min(1.0, 2.0 * mx.crossfader);
    const double gain = static_cast<double>(v.fader) * v.fader * xf;
    if (gain > best_gain + 1e-9) {
      best_gain = gain;
      best = d;
    }
  }
  if (best >= 0) return best;
  for (int d = 0; d < kMaxDecks; ++d) {  // nothing plays: the loaded deck with the most recently touched position
    const DeckView v = engine.deck(d);
    if (v.loaded && v.track_id > 0 && v.position > 0) return d;
  }
  for (int d = 0; d < kMaxDecks; ++d) {
    const DeckView v = engine.deck(d);
    if (v.loaded && v.track_id > 0) return d;
  }
  return -1;
}

NextTrackOptions next_options(Library& lib, const PlayQueue* queue, const NextSettings& s, std::int64_t playlist_id, std::size_t limit) {
  NextTrackOptions o;
  o.weights = lib.suggest_weights();
  o.max_pitch = s.max_pitch;
  o.half_double = s.half_double;
  o.harmonic_only = s.harmonic_only;
  o.playlist_id = playlist_id;
  o.limit = limit;
  if (s.exclude_queued && queue) {
    for (const QueueEntry& e : queue->upcoming()) o.exclude.insert(e.track_id);
    if (const auto cur = queue->current()) o.exclude.insert(cur->track_id);
  }
  const auto sessions = lib.history_sessions();
  if (!sessions.empty()) {  // the newest session, newest first
    const auto entries = lib.history(sessions.front().id);
    for (auto it = entries.rbegin(); it != entries.rend() && o.recent.size() < 60; ++it)
      if (it->track_id > 0) o.recent.push_back(it->track_id);
    if (s.exclude_played)
      for (const HistoryEntry& e : entries)
        if (e.track_id > 0) o.exclude.insert(e.track_id);
  }
  o.recent_from_history = false;  // decided here: an empty list means "nothing played yet"
  return o;
}

#if DECKHAND_WITH_API
api::Json next_tracks_json(Library& lib, std::int64_t seed_id, const std::vector<NextTrack>& items) {
  api::Json j = api::Json::object();
  if (const auto seed = lib.track(seed_id)) j.set("seed", track_summary(*seed));
  api::Json arr = api::Json::array();
  for (const NextTrack& n : items) {
    const auto row = lib.track(n.track_id);
    if (!row) continue;
    api::Json o = api::Json::object();
    o.set("track", track_summary(*row));
    o.set("score", n.score);
    o.set("pitch_percent", n.pitch_change * 100.0);
    o.set("pair_count", n.pair_count);
    o.set("reverse_count", n.reverse_count);
    api::Json fs = api::Json::array();
    for (const NextTrackFactor& f : n.factors) {
      api::Json x = api::Json::object();
      x.set("id", f.id);
      x.set("points", f.points);
      x.set("value", f.value);
      x.set("detail", f.detail);
      if (!f.tag.empty()) x.set("tag", f.tag);
      fs.push_back(std::move(x));
    }
    o.set("factors", std::move(fs));
    arr.push_back(std::move(o));
  }
  j.set("items", std::move(arr));
  return j;
}
#endif

}  // namespace deckhand
