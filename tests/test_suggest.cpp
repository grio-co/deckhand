// Track suggestions (scoring factors alone and together, determinism, exclusions), the transition history (incremental,
// backfill, migration 8 -> 9) and the preparation of shared pieces that live in the library.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "check.hpp"
#include "deckhand/library/backup.hpp"
#include "deckhand/app/next_tracks.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;

namespace {

std::string temp_db(const char* name) {
  const std::string p = (std::filesystem::temp_directory_path() / name).string();
  for (const char* suffix : {"", "-wal", "-shm"}) std::remove((p + suffix).c_str());
  return p;
}

std::int64_t add(Library& lib, const std::string& name, double bpm, const char* key, const char* genre = "", int rating = 0) {
  NewTrack t;
  t.path = "/music/" + name + ".mp3";
  t.title = name;
  t.artist = "A";
  t.bpm = bpm;
  t.key = key;
  t.genre = genre;
  t.rating = rating;
  return lib.upsert_track(t);
}

NextTrackOptions only(const char* factor) {
  NextTrackOptions o;
  NextTrackWeights& w = o.weights;
  w.tempo = w.key = w.energy = w.genre = w.rating = w.history = w.explore = w.recent = 0;
  const std::string f = factor;
  if (f == "tempo") w.tempo = 10;
  if (f == "key") w.key = 10;
  if (f == "energy") w.energy = 10;
  if (f == "genre") w.genre = 10;
  if (f == "rating") w.rating = 10;
  if (f == "history") w.history = 10;
  if (f == "explore") w.explore = 10;
  if (f == "recent") w.recent = 10;
  o.harmonic_only = false;
  return o;
}

const NextTrackFactor* factor(const NextTrack& s, const char* id) {
  for (const NextTrackFactor& f : s.factors)
    if (f.id == id) return &f;
  return nullptr;
}

const NextTrack* find(const std::vector<NextTrack>& v, std::int64_t id) {
  for (const NextTrack& s : v)
    if (s.track_id == id) return &s;
  return nullptr;
}

StoredPhrases energy_phrases(float intro, float outro) {
  StoredPhrases p;
  p.gridded = true;
  p.total_bars = 32;
  for (int i = 0; i < 32; ++i) p.bar_energy.push_back(i < 16 ? intro : outro);
  return p;
}

void test_tempo() {
  Library lib(temp_db("deckhand_test_suggest_tempo.db"));
  const auto seed = add(lib, "seed", 120, "8A");
  const auto same = add(lib, "same", 120, "8A");
  const auto near = add(lib, "near", 123, "8A");   // 2.4 % to slow down
  const auto edge = add(lib, "edge", 128.5, "8A"); // beyond 6 %
  const auto half = add(lib, "half", 60, "8A");
  const auto dbl = add(lib, "double", 240, "8A");
  const auto odd = add(lib, "odd", 90, "8A");
  NextTrackOptions o = only("tempo");
  auto r = lib.suggest(seed, o);
  CHECK(find(r, same) && find(r, near) && find(r, half) && find(r, dbl));
  CHECK(!find(r, edge) && !find(r, odd) && !find(r, seed));
  CHECK(r[0].track_id == same);  // best tempo first
  const NextTrack* n = find(r, near);
  CHECK(std::fabs(n->pitch_change - (120.0 / 123 - 1)) < 1e-9);
  CHECK(factor(*n, "tempo")->tag == "direct" && std::fabs(factor(*n, "tempo")->detail - 3) < 1e-9);
  CHECK(factor(*find(r, half), "tempo")->tag == "half" && factor(*find(r, dbl), "tempo")->tag == "double");
  CHECK(factor(*find(r, same), "tempo")->points > factor(*n, "tempo")->points);
  CHECK(std::fabs(factor(*find(r, same), "tempo")->points - 10) < 1e-9);  // a perfect match gets the full weight
  o.half_double = false;
  r = lib.suggest(seed, o);
  CHECK(!find(r, half) && !find(r, dbl) && find(r, near));
  o.max_pitch = 0.07;  // 128.5 needs 5.9 %
  o.half_double = true;
  CHECK(find(lib.suggest(seed, o), edge));
}

void test_key() {
  Library lib(temp_db("deckhand_test_suggest_key.db"));
  const auto seed = add(lib, "seed", 120, "8A");
  const auto same = add(lib, "same", 120, "8A");
  const auto up = add(lib, "up", 120, "9A");
  const auto down = add(lib, "down", 120, "7A");
  const auto rel = add(lib, "rel", 120, "8B");
  const auto boost = add(lib, "boost", 120, "10A");
  const auto far = add(lib, "far", 120, "2A");
  const auto musical = add(lib, "musical", 120, "Am");  // 8A written the other way
  const auto nokey = add(lib, "nokey", 120, "");
  NextTrackOptions o = only("key");
  auto r = lib.suggest(seed, o);
  CHECK(factor(*find(r, same), "key")->tag == "same" && factor(*find(r, musical), "key")->tag == "same");
  CHECK(factor(*find(r, up), "key")->tag == "adjacent" && factor(*find(r, down), "key")->tag == "adjacent");
  CHECK(factor(*find(r, rel), "key")->tag == "relative" && factor(*find(r, boost), "key")->tag == "boost");
  CHECK(factor(*find(r, far), "key")->value == 0 && !factor(*find(r, nokey), "key"));
  CHECK(factor(*find(r, same), "key")->points > factor(*find(r, up), "key")->points);
  CHECK(factor(*find(r, up), "key")->points > factor(*find(r, boost), "key")->points);
  o.harmonic_only = true;  // the default: keys that do not mix are left out, unknown keys stay
  r = lib.suggest(seed, o);
  CHECK(!find(r, far) && find(r, nokey) && find(r, boost));
}

void test_energy_genre_rating() {
  Library lib(temp_db("deckhand_test_suggest_misc.db"));
  const auto seed = add(lib, "seed", 120, "8A", "House / Deep House", 0);
  const auto a = add(lib, "a", 120, "8A", "deep house", 5);
  const auto b = add(lib, "b", 120, "8A", "Techno", 1);
  const auto c = add(lib, "c", 120, "8A", "", 0);
  lib.set_phrases(seed, energy_phrases(0.2f, 0.8f));
  lib.set_phrases(a, energy_phrases(0.8f, 0.5f));   // intro matches the seed's outro
  lib.set_phrases(b, energy_phrases(0.1f, 0.5f));
  const auto es = lib.energy_summary(seed);
  CHECK(es && std::fabs(es->intro - 0.2) < 1e-6 && std::fabs(es->outro - 0.8) < 1e-6 && std::fabs(es->avg - 0.5) < 1e-6);
  {
    auto r = lib.suggest(seed, only("energy"));
    CHECK(r.size() == 3 && r[0].track_id == a && r[1].track_id == b && r[2].track_id == c);  // c has no phrase data: 0 points
    CHECK(std::fabs(r[0].score - 10) < 1e-6 && std::fabs(r[1].score - 3) < 1e-6 && r[2].score == 0 && !factor(r[2], "energy"));
    CHECK(std::fabs(factor(r[1], "energy")->detail - (0.1 - 0.8)) < 1e-6);
  }
  NextTrackOptions og = only("genre");
  auto r = lib.suggest(seed, og);
  CHECK(factor(*find(r, a), "genre")->tag == "deep house" && std::fabs(factor(*find(r, a), "genre")->value - 1) < 1e-9);
  CHECK(factor(*find(r, b), "genre")->value == 0 && !factor(*find(r, c), "genre"));
  r = lib.suggest(seed, only("rating"));
  CHECK(r[0].track_id == a && std::fabs(r[0].score - 10) < 1e-9 && std::fabs(find(r, b)->score - 2) < 1e-9 && find(r, c)->score == 0);
}

void test_history_recent_explore() {
  Library lib(temp_db("deckhand_test_suggest_hist.db"));
  const auto seed = add(lib, "seed", 120, "8A");
  const auto a = add(lib, "a", 120, "8A");
  const auto b = add(lib, "b", 120, "8A");
  const auto c = add(lib, "c", 120, "8A");
  // Session 1: seed -> a (three times over three sessions), b once.
  for (int i = 0; i < 3; ++i) {
    const auto s = lib.begin_history_session();
    lib.add_history(s, seed, "A", 1000 + i * 100000);
    lib.add_history(s, a, "B", 2000 + i * 100000);
  }
  {
    const auto s = lib.begin_history_session();
    lib.add_history(s, seed, "A", 900000);
    lib.add_history(s, b, "B", 901000);
  }
  CHECK(lib.transition(seed, a) && lib.transition(seed, a)->count == 3 && lib.transition(seed, b)->count == 1);
  CHECK(!lib.transition(a, seed));
  NextTrackOptions o = only("history");
  o.recent_from_history = false;
  auto r = lib.suggest(seed, o);
  CHECK(r[0].track_id == a && r[0].pair_count == 3 && std::fabs(r[0].score - 10) < 1e-9);  // three plays: the full weight
  CHECK(r[1].track_id == b && std::fabs(r[1].score - 5) < 1e-9 && find(r, c)->score == 0);
  CHECK(factor(r[0], "history")->detail == 3);
  // The other way round counts less.
  auto rr = lib.suggest(a, o);
  CHECK(find(rr, seed)->reverse_count == 3 && find(rr, seed)->score > 0 && find(rr, seed)->score < 10);
  // Exploration: c was never played, a, b and seed were.
  r = lib.suggest(seed, only("explore"));
  CHECK(find(r, c)->score == 10 && find(r, a)->score == 0 && find(r, b)->score == 0);
  // Recency: the newest session is {seed, b}; b is pushed down.
  NextTrackOptions orc = only("recent");
  r = lib.suggest(seed, orc);
  CHECK(find(r, b)->score < 0 && find(r, a)->score == 0 && find(r, c)->score == 0);
  orc.recent = {a, b};  // explicit list wins
  r = lib.suggest(seed, orc);
  CHECK(find(r, a)->score < find(r, b)->score + 1e-9 && find(r, a)->score < 0 && find(r, b)->score < 0 && find(r, a)->score <= find(r, b)->score);
  CHECK(factor(*find(r, a), "recent")->detail == 0 && factor(*find(r, b), "recent")->detail == 1);
}

void test_combined_and_determinism() {
  Library lib(temp_db("deckhand_test_suggest_comb.db"));
  const auto seed = add(lib, "seed", 124, "5A", "House", 0);
  std::vector<std::int64_t> ids;
  for (int i = 0; i < 40; ++i) {
    const char* keys[] = {"5A", "6A", "5B", "4A", "7A", "12B"};
    ids.push_back(add(lib, "t" + std::to_string(i), 118 + (i % 12), keys[i % 6], i % 3 ? "House" : "Techno", i % 6));
  }
  // The friendlier tempo and key wins when everything else is equal; the rating breaks the rest.
  NextTrackOptions o;
  o.limit = 0;
  const auto r1 = lib.suggest(seed, o);
  const auto r2 = lib.suggest(seed, o);
  CHECK(!r1.empty() && r1.size() == r2.size());
  for (std::size_t i = 0; i < r1.size(); ++i) CHECK(r1[i].track_id == r2[i].track_id && r1[i].score == r2[i].score);
  for (std::size_t i = 1; i < r1.size(); ++i) CHECK(r1[i - 1].score > r1[i].score || (r1[i - 1].score == r1[i].score && r1[i - 1].track_id < r1[i].track_id));
  double sum = 0;
  for (const NextTrackFactor& f : r1[0].factors) sum += f.points;
  CHECK(std::fabs(sum - r1[0].score) < 1e-9);  // the breakdown adds up
  for (const NextTrack& s : r1) CHECK(std::fabs(s.pitch_change) <= 0.06 + 1e-9);
  // Limit, exclusions, playlist.
  o.limit = 3;
  CHECK(lib.suggest(seed, o).size() == 3);
  o.limit = 0;
  o.exclude = {r1[0].track_id, r1[1].track_id};
  const auto ex = lib.suggest(seed, o);
  CHECK(ex.size() + 2 == r1.size() && !find(ex, r1[0].track_id) && !find(ex, r1[1].track_id));
  o.exclude.clear();
  const auto pl = lib.create_playlist(0, "set", false);
  lib.add_to_playlist(pl, ids[3]);
  lib.add_to_playlist(pl, ids[4]);
  lib.add_to_playlist(pl, seed);
  o.playlist_id = pl;
  const auto rp = lib.suggest(seed, o);
  CHECK(rp.size() <= 2 && !find(rp, seed));
  for (const NextTrack& s : rp) CHECK(s.track_id == ids[3] || s.track_id == ids[4]);
  // Weights from the settings.
  lib.set_setting("suggest.w.tempo", "0");
  CHECK(lib.suggest_weights().tempo == 0 && lib.suggest_weights().key == 25);
  CHECK(lib.suggest(99999).empty());
}

void test_transitions_backfill_and_migration() {
  const std::string path = temp_db("deckhand_test_suggest_mig.db");
  std::int64_t a = 0, b = 0, c = 0;
  {
    Library lib(path);
    a = add(lib, "a", 120, "8A");
    b = add(lib, "b", 120, "8A");
    c = add(lib, "c", 120, "8A");
    const auto s1 = lib.begin_history_session();
    lib.add_history(s1, a, "A", 1000);
    lib.add_history(s1, b, "B", 2000);
    lib.add_history(s1, b, "A", 3000);  // the same track again: not a transition
    lib.add_history(s1, c, "B", 4000);
    const auto s2 = lib.begin_history_session();
    lib.add_history(s2, c, "A", 5000);  // a new session starts fresh: nothing links c to a
    lib.add_history(s2, a, "B", 6000);
    CHECK(lib.transition(a, b)->count == 1 && lib.transition(b, c)->count == 1 && lib.transition(c, a)->count == 1);
    CHECK(!lib.transition(b, b) && !lib.transition(c, b));
    CHECK(lib.transition(c, a)->last_ms == 6000);
    lib.save_transition(a, c, "drop swap");
    CHECK(lib.transition(a, c)->saved && lib.transition(a, c)->count == 0 && lib.transition(a, c)->note == "drop swap");
    lib.save_transition(a, b, "smooth");  // an existing pair keeps its count
    CHECK(lib.transition(a, b)->count == 1 && lib.transition(a, b)->note == "smooth");
    CHECK(lib.transitions_from(a).size() == 2 && lib.transitions_from(a)[0].to_id == b);
    // Repair: wipe and rebuild from the history; saved pairs and notes stay.
    lib.db().exec("DELETE FROM transitions WHERE saved = 0; UPDATE transitions SET count = 7 WHERE from_id = " + std::to_string(a));
    lib.rebuild_transitions();
    CHECK(lib.transition(a, b)->count == 1 && lib.transition(a, b)->note == "smooth" && lib.transition(c, a)->count == 1);
    CHECK(lib.transition(a, c)->saved);
    // Downgrade to schema 8 as 1821e78 left it.
    lib.set_phrases(a, energy_phrases(0.3f, 0.6f));
    lib.db().exec("DROP TABLE transitions; DROP INDEX idx_tracks_bpm; DROP INDEX idx_tracks_key;"
                  "ALTER TABLE track_phrases DROP COLUMN energy_avg; ALTER TABLE track_phrases DROP COLUMN energy_intro;"
                  "ALTER TABLE track_phrases DROP COLUMN energy_outro; UPDATE schema_version SET version = 8;");
  }
  {
    Library lib(path);  // migrates 8 -> 10 and backfills from the history and the stored phrases
    Stmt s(lib.db(), "SELECT max(version) FROM schema_version");
    CHECK(s.step() && s.i64(0) == kLibrarySchemaVersion && kLibrarySchemaVersion == 11);
    CHECK(lib.transition(a, b)->count == 1 && lib.transition(b, c)->count == 1 && lib.transition(c, a)->count == 1);
    CHECK(!lib.transition(a, c));
    const std::string esql = "SELECT energy_intro, energy_outro FROM track_phrases WHERE track_id = " + std::to_string(a);
    Stmt e(lib.db(), esql.c_str());
    CHECK(e.step() && !e.is_null(0) && std::fabs(e.f64(0) - 0.3) < 1e-3 && std::fabs(e.f64(1) - 0.6) < 1e-3);
    const auto copies = library_backups(path);
    CHECK(!copies.empty() && copies[0].reason == "upgrade" && copies[0].schema_version == 8);
  }
  for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
}

// The JSON the REST routes return, and the settings behind its defaults.
void test_rest_shape() {
  Library lib(temp_db("deckhand_test_suggest_rest.db"));
  const auto seed = add(lib, "seed", 120, "8A", "House", 4);
  const auto a = add(lib, "a", 122, "9A", "House", 5);
  add(lib, "b", 118, "2B", "House", 0);  // key that does not mix: left out by default
  const auto s1 = lib.begin_history_session();
  lib.add_history(s1, seed, "A", 1000);
  lib.add_history(s1, a, "B", 2000);
  NextSettings ns = next_settings(lib);
  CHECK(ns.max_pitch == 0.06 && ns.half_double && ns.harmonic_only && ns.exclude_queued && !ns.exclude_played && ns.follow_master);
  ns.max_pitch = 0.08;
  ns.harmonic_only = false;
  save_next_settings(lib, ns);
  const NextSettings back = next_settings(lib);
  CHECK(std::fabs(back.max_pitch - 0.08) < 1e-9 && !back.harmonic_only);
  ns.harmonic_only = true;
  const NextTrackOptions o = next_options(lib, nullptr, ns, 0, 5);
  CHECK(o.limit == 5 && o.recent.size() == 2 && o.recent[0] == a && !o.recent_from_history);
  const api::Json j = next_tracks_json(lib, seed, lib.suggest(seed, o));
  CHECK(j["seed"]["id"].as_int() == seed && j["seed"]["title"].as_string() == "seed" && j["seed"]["bpm"].as_double() == 120);
  CHECK(j["items"].is_array() && j["items"].size() == 1);  // b does not mix
  const api::Json& it = j["items"].at(0);
  CHECK(it["track"]["id"].as_int() == a && it["pair_count"].as_int() == 1 && it["reverse_count"].as_int() == 0);
  CHECK(it["score"].is_number() && it["pitch_percent"].is_number() && it["factors"].is_array() && it["factors"].size() >= 4);
  bool tempo = false, key = false, history = false, recent = false;
  for (std::size_t i = 0; i < it["factors"].size(); ++i) {
    const api::Json& f = it["factors"].at(i);
    CHECK(f.contains("id") && f.contains("points") && f.contains("value") && f.contains("detail"));
    tempo = tempo || (f["id"].as_string() == "tempo" && f["tag"].as_string() == "direct");
    key = key || (f["id"].as_string() == "key" && f["tag"].as_string() == "adjacent");
    history = history || (f["id"].as_string() == "history" && f["detail"].as_int() == 1);
    recent = recent || f["id"].as_string() == "recent";
  }
  CHECK(tempo && key && history && recent);  // a was played this session, right after the seed
  std::string error;
  CHECK(api::Json::parse(j.dump(), &error).is_object() && error.empty());
  ns.exclude_played = true;
  CHECK(next_options(lib, nullptr, ns).exclude.count(a) == 1 && next_options(lib, nullptr, ns).exclude.count(seed) == 1);
}

}  // namespace

int main() {
  test_tempo();
  test_key();
  test_energy_genre_rating();
  test_history_recent_explore();
  test_combined_and_determinism();
  test_transitions_backfill_and_migration();
  test_rest_shape();
  std::puts("test_suggest ok");
  return 0;
}
