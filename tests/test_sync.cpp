// Sync: devices are separate Library databases sharing one folder. Convergence is checked with a canonical dump.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../src/sync/format.hpp"
#include "check.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/sync/sync.hpp"

using namespace deckhand;
using namespace deckhand::sync;
namespace fs = std::filesystem;

namespace {

long run_id() {
  static const long id = static_cast<long>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007);
  return id;
}
std::int64_t g_now = 1'700'000'000'000ll;
void adv(std::int64_t ms = 1000) { g_now += ms; }

struct Dev {
  std::string name, dir, music;
  std::int64_t skew = 0;
  std::size_t max_log = 1u << 20;
  bool hints = false;
  std::function<bool(const char*)> fault;
  std::string folder;
  std::unique_ptr<Library> lib;
  std::unique_ptr<SyncEngine> eng;

  void start() {
    lib = std::make_unique<Library>(dir + "/library.db");
    SyncConfig c;
    c.folder = folder;
    c.device_name = name;
    c.clock = [this] { return g_now + skew; };
    c.max_log_bytes = max_log;
    c.trust_path_hints = hints;
    c.fault = [this](const char* s) { return fault && fault(s); };
    eng = std::make_unique<SyncEngine>(*lib, c);
  }
  void stop() {
    eng.reset();
    lib.reset();
  }
};

struct Scenario {
  std::string root, folder;
  std::vector<std::unique_ptr<Dev>> devs;
  explicit Scenario(const std::string& name) {
    root = (fs::temp_directory_path() / ("dh_sync_test_" + std::to_string(run_id()) + "_" + name)).string();
    std::error_code ec;
    fs::remove_all(root, ec);
    folder = root + "/cloud";
    fs::create_directories(folder);
  }
  ~Scenario() {
    devs.clear();
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  Dev& add(const std::string& name, std::int64_t skew = 0, std::size_t max_log = 1u << 20, bool hints = false) {
    auto d = std::make_unique<Dev>();
    d->name = name;
    d->dir = root + "/" + name;
    d->music = d->dir + "/music";
    d->skew = skew;
    d->max_log = max_log;
    d->hints = hints;
    d->folder = folder;
    fs::create_directories(d->music);
    d->start();
    devs.push_back(std::move(d));
    return *devs.back();
  }
  int files_in_folder() const {
    int n = 0;
    for (const auto& e : fs::recursive_directory_iterator(folder))
      if (e.is_regular_file()) ++n;
    return n;
  }
};

void write_audio(const std::string& path, int seed) {
  std::string bytes(2000, '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((seed * 131 + static_cast<int>(i) * 7 + (static_cast<int>(i) >> 5) * seed) & 0xFF);
  const std::string tag = "seed:" + std::to_string(seed) + ";";
  bytes.replace(0, tag.size(), tag);
  std::ofstream(path, std::ios::binary) << bytes;
}
std::string file_for(Dev& d, int seed) { return d.music + "/trk" + std::to_string(seed) + "_" + d.name + ".mp3"; }

std::int64_t add_track(Dev& d, int seed, const std::string& title, const std::string& artist = "Artist", double bpm = 120) {
  write_audio(file_for(d, seed), seed);
  NewTrack t;
  t.path = file_for(d, seed);
  t.title = title;
  t.artist = artist;
  t.bpm = bpm;
  t.duration_ms = 200000 + seed;
  return d.lib->upsert_track(t);
}
std::string fp_of_seed(int seed) {
  static std::string dir = (fs::temp_directory_path() / ("dh_sync_fp_" + std::to_string(run_id()))).string();
  fs::create_directories(dir);
  const std::string p = dir + "/s" + std::to_string(seed);
  write_audio(p, seed);
  return SyncEngine::fingerprint_file(p);
}
// Local row of the track made from this seed on this device.
std::int64_t tid(Dev& d, int seed) {
  if (const auto id = d.lib->track_id_for_path(file_for(d, seed))) return *id;
  Stmt s(d.lib->db(), "SELECT track_id FROM sync_track WHERE fp = ?1 ORDER BY track_id");
  s.bind(1, fp_of_seed(seed));
  return s.step() ? s.i64(0) : 0;
}

SyncResult sync(Dev& d) {
  adv();
  const auto r = d.eng->sync_once();
  if (!r.ok) std::fprintf(stderr, "sync on %s failed: %s\n", d.name.c_str(), r.error.c_str());
  CHECK(r.ok);
  return r;
}
void sync_all(Scenario& s, int rounds = 3) {
  for (int i = 0; i < rounds; ++i)
    for (auto& d : s.devs) sync(*d);
}

std::string fmt(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.17g", v);
  return b;
}
std::string cell(const Stmt& s, int col) {
  if (s.is_null(col)) return "~";
  const std::string t = s.text(col);
  return t.empty() || t == "0" || t == "0.0" ? "~" : t;  // NULL, '' and 0 all mean "unset"
}

std::string dump(Dev& d) {
  Db& db = d.lib->db();
  std::ostringstream o;
  {
    Stmt s(db,
           "SELECT t.id, s.fp, t.title, t.artist, t.album, t.genre, t.label, t.comment, t.key, t.format, t.year,"
           " t.rating, t.color, t.duration_ms, t.bitrate, t.sample_rate, t.date_added, t.bpm, t.gain_db"
           " FROM tracks t JOIN sync_track s ON s.track_id = t.id ORDER BY s.fp, t.id");
    while (s.step()) {
      o << "T " << s.text(1);
      for (int c = 2; c <= 18; ++c) o << '|' << cell(s, c);
      for (const auto& g : d.lib->beatgrid(s.i64(0))) o << " G" << fmt(g.first_beat_ms) << ',' << fmt(g.bpm) << ',' << g.bar_beat;
      Stmt c(db, "SELECT kind, slot, start_ms, end_ms, label, color FROM cues WHERE track_id = ?1 ORDER BY kind, coalesce(slot, -1), start_ms");
      c.bind(1, s.i64(0));
      while (c.step()) o << " C" << cell(c, 0) << ':' << cell(c, 1) << ':' << cell(c, 2) << ':' << cell(c, 3) << ':' << cell(c, 4) << ':' << cell(c, 5);
      o << '\n';
    }
  }
  const std::function<void(std::int64_t, int)> tree = [&](std::int64_t parent, int depth) {
    Stmt s(db, "SELECT id, name, is_folder FROM playlists WHERE coalesce(parent_id, 0) = ?1 ORDER BY position, name");
    s.bind(1, parent);
    std::vector<std::tuple<std::int64_t, std::string, bool>> rows;
    while (s.step()) rows.emplace_back(s.i64(0), s.text(1), s.i64(2) != 0);
    for (const auto& [id, name, folder] : rows) {
      o << std::string(static_cast<std::size_t>(depth), ' ') << (folder ? "F " : "P ") << name;
      Stmt e(db, "SELECT coalesce(s.fp, 'nofp') FROM playlist_tracks p LEFT JOIN sync_track s ON s.track_id = p.track_id WHERE p.playlist_id = ?1 ORDER BY p.position");
      e.bind(1, id);
      while (e.step()) o << ' ' << e.text(0).substr(0, 12) << e.text(0).substr(e.text(0).size() > 6 ? e.text(0).size() - 4 : 0);
      o << '\n';
      if (depth < 20) tree(id, depth + 1);
    }
  };
  tree(0, 0);
  {
    std::vector<std::string> lines;
    Stmt s(db,
           "SELECT hs.started_at, h.deck, coalesce(s.fp, '-'), h.started_at, h.played_ms FROM history h"
           " JOIN history_sessions hs ON hs.id = h.session_id LEFT JOIN sync_track s ON s.track_id = h.track_id");
    while (s.step()) lines.push_back("H " + s.text(0) + '|' + s.text(1) + '|' + s.text(2) + '|' + s.text(3) + '|' + s.text(4));
    std::sort(lines.begin(), lines.end());
    for (const auto& l : lines) o << l << '\n';
  }
  {
    Stmt s(db, "SELECT key, value FROM settings WHERE key IN ('ui.language', 'ui.skin', 'cue.autoload') ORDER BY key");
    while (s.step()) o << "S " << s.text(0) << '=' << s.text(1) << '\n';
  }
  return o.str();
}

void expect_equal(Scenario& s) {
  const std::string first = dump(*s.devs[0]);
  for (std::size_t i = 1; i < s.devs.size(); ++i) {
    const std::string other = dump(*s.devs[i]);
    if (other != first && getenv("DH_DEBUG")) {
      for (auto& d : s.devs) {
        std::fprintf(stderr, "== regs %s\n", d->name.c_str());
        Stmt r(d->lib->db(), "SELECT kind, eid, field, wall, ctr, dev, vs, vi FROM sync_reg WHERE kind IN ('E') ORDER BY eid, field");
        while (r.step()) std::fprintf(stderr, "%s %s %s %s.%s %s %s %s\n", r.text(0).c_str(), r.text(1).c_str(), r.text(2).c_str(), r.text(3).c_str(), r.text(4).c_str(), r.text(5).c_str(), r.text(6).c_str(), r.text(7).c_str());
      }
    }
    if (other != first) std::fprintf(stderr, "---- %s\n%s---- %s\n%s----\n", s.devs[0]->name.c_str(), first.c_str(), s.devs[i]->name.c_str(), other.c_str());
    CHECK(other == first);
  }
}

std::int64_t count(Dev& d, const std::string& sql) {
  Stmt s(d.lib->db(), sql.c_str());
  return s.step() ? s.i64(0) : -1;
}
std::vector<std::int64_t> list_seeds(Dev& d, std::int64_t playlist, const std::vector<int>& seeds) {
  std::vector<std::int64_t> out;
  for (const auto id : d.lib->playlist_track_ids(playlist))
    for (const int sd : seeds)
      if (id == tid(d, sd)) out.push_back(sd);
  return out;
}
std::int64_t playlist_named(Dev& d, const std::string& name) {
  Stmt s(d.lib->db(), "SELECT id FROM playlists WHERE name = ?1");
  s.bind(1, name);
  return s.step() ? s.i64(0) : 0;
}
void edit_title(Dev& d, std::int64_t id, const std::string& t) {
  TrackEdit e;
  e.title = t;
  d.lib->edit_track(id, e);
}
std::string title_of(Dev& d, int seed) {
  const auto t = d.lib->track(tid(d, seed));
  return t ? t->title : "<none>";
}

// ---- tests -------------------------------------------------------------------------------------------------------

void test_one_way_all_entities() {
  Scenario s("oneway");
  Dev& a = s.add("A");
  Dev& b = s.add("B");
  const auto t1 = add_track(a, 1, "One", "Ann", 128), t2 = add_track(a, 2, "Two", "Bob", 90), t3 = add_track(a, 3, "Three", "Cy");
  TrackEdit e;
  e.genre = "House";
  e.album = "LP";
  e.comment = "hello \n world";
  e.rating = 4;
  e.year = 1999;
  e.key = "8A";
  a.lib->edit_track(t1, e);
  a.lib->set_gain_db(t1, -3.5);
  a.lib->replace_beatgrid(t1, {{120.5, 128.0, 1}, {60000.25, 129.5, 3}});
  a.lib->set_hot_cue(t1, 0, 1000.5);
  a.lib->set_hot_cue(t1, 3, 5000, 9000);  // a loop on a slot
  Stmt(a.lib->db(), "INSERT INTO cues(track_id, kind, slot, start_ms, label, color) VALUES (1, 'memory', NULL, 777.125, 'drop', 255)").run();
  const auto folder = a.lib->create_playlist(0, "Sets", true);
  const auto sub = a.lib->create_playlist(folder, "Friday", false);
  const auto top = a.lib->create_playlist(0, "Warmup", false);
  a.lib->add_to_playlist(sub, t1);
  a.lib->add_to_playlist(sub, t2);
  a.lib->add_to_playlist(sub, t1);  // duplicates are legal
  a.lib->add_to_playlist(top, t3);
  const auto sess = a.lib->begin_history_session();
  const auto h = a.lib->add_history(sess, t2, "A", 1'699'999'000'000);
  a.lib->set_history_played(h, 31000);
  a.lib->set_setting("ui.language", "de");
  a.lib->set_setting("obs.password", "hunter2");
  a.lib->set_setting("api.token", "abc");
  a.lib->set_setting("audio.device", "Built-in");
  a.lib->set_setting("sync.folder", "/x");

  const auto r = sync(a);
  CHECK(r.changes_captured > 30 && r.files_written >= 1 && r.records_written == r.changes_captured);
  const auto rb = sync(b);
  CHECK(rb.records_applied == r.records_written && rb.tracks_missing == 3);
  sync(a);
  expect_equal(s);
  CHECK(count(b, "SELECT count(*) FROM tracks") == 3 && count(b, "SELECT count(*) FROM playlists") == 3);
  CHECK(b.lib->setting("ui.language") == "de");
  CHECK(b.lib->setting("obs.password").empty() && b.lib->setting("api.token").empty() && b.lib->setting("audio.device").empty() &&
        b.lib->setting("sync.folder").empty());
  CHECK(list_seeds(b, playlist_named(b, "Friday"), {1, 2}) == (std::vector<std::int64_t>{1, 2, 1}));
  CHECK(b.lib->beatgrid(tid(b, 1)).size() == 2 && b.lib->gain_db(tid(b, 1)).value_or(0) == -3.5);
  CHECK(b.lib->track(tid(b, 1))->title == "One" && b.lib->track(tid(b, 1))->rating == 4);
  CHECK(a.eng->devices().size() == 2);

  // The audio files are not synced: B's rows are "missing" until the files appear, then relink by content.
  CHECK(b.eng->status().tracks_missing == 3);
  for (int sd : {1, 2}) write_audio(b.music + "/found_" + std::to_string(sd) + ".flac", sd);
  fs::create_directories(b.music + "/sub");
  write_audio(b.music + "/sub/other.mp3", 99);  // unrelated file is ignored
  CHECK(b.eng->relink_missing({b.music}) == 2);
  CHECK(b.eng->status().tracks_missing == 1);
  CHECK(fs::exists(b.lib->track(tid(b, 1))->path));
  write_audio(b.music + "/sub/three.mp3", 3);
  CHECK(b.eng->relink_missing({b.music}) == 1 && b.eng->status().tracks_missing == 0);
  sync_all(s);
  expect_equal(s);

  // And the other way round: B edits, A follows.
  adv();
  edit_title(b, tid(b, 3), "Three (B)");
  b.lib->add_to_playlist(playlist_named(b, "Warmup"), tid(b, 1));
  sync_all(s);
  CHECK(title_of(a, 3) == "Three (B)" && a.lib->playlist_track_ids(playlist_named(a, "Warmup")).size() == 2);
  expect_equal(s);
}

void test_path_hint_relink() {
  Scenario s("hints");
  Dev& a = s.add("A");
  Dev& b = s.add("B", 0, 1u << 20, true);  // a shared mount: the other device's path also exists here
  add_track(a, 1, "One");
  sync_all(s);
  CHECK(b.eng->status().tracks_missing == 0);
  CHECK(b.lib->track(tid(b, 1))->path == file_for(a, 1));
  expect_equal(s);
}

void test_concurrent_fields() {
  Scenario s("fields");
  Dev& a = s.add("A");
  Dev& b = s.add("B");
  const auto ta = add_track(a, 1, "Base"), tb = add_track(b, 1, "Base");  // same file, different paths
  sync_all(s);
  CHECK(count(a, "SELECT count(*) FROM tracks") == 1 && count(b, "SELECT count(*) FROM tracks") == 1);  // matched by content
  adv();
  TrackEdit ea;
  ea.title = "From A";
  ea.rating = 3;
  a.lib->edit_track(ta, ea);
  adv();
  TrackEdit eb;
  eb.genre = "Techno";
  eb.rating = 5;  // same field: B is later
  b.lib->edit_track(tb, eb);
  a.lib->set_hot_cue(ta, 1, 1000);
  b.lib->set_hot_cue(tb, 2, 2000);
  sync_all(s);
  expect_equal(s);
  const auto row = a.lib->track(tid(a, 1));
  CHECK(row->title == "From A" && row->genre == "Techno" && row->rating == 5);
  CHECK(a.lib->cues(tid(a, 1)).size() == 2);
  // Truly simultaneous equal clocks: still identical on both.
  const auto stamp_now = g_now;
  a.lib->set_bpm(ta, 100);
  b.lib->set_bpm(tb, 101);
  g_now = stamp_now;
  sync(a);
  g_now = stamp_now;
  sync(b);
  sync_all(s);
  expect_equal(s);
}

void test_playlists_merge() {
  Scenario s("playlists");
  Dev& a = s.add("A");
  Dev& b = s.add("B");
  Dev& c = s.add("C");
  for (int i = 1; i <= 8; ++i) add_track(a, i, "T" + std::to_string(i));
  const auto pl = a.lib->create_playlist(0, "Mix", false);
  for (int i = 1; i <= 3; ++i) a.lib->add_to_playlist(pl, tid(a, i));
  sync_all(s);
  const auto pa = playlist_named(a, "Mix"), pb = playlist_named(b, "Mix"), pc = playlist_named(c, "Mix");
  // Add the remaining tracks everywhere first so every device can reference them.
  for (int i = 4; i <= 8; ++i) {
    add_track(b, i, "T" + std::to_string(i));
    add_track(c, i, "T" + std::to_string(i));
  }
  sync_all(s);
  adv();
  a.lib->add_to_playlist(pa, tid(a, 4));          // A appends 4
  a.lib->move_in_playlist(pa, 0, 3);             // ...and moves track 1 to the end: 2 3 4 1 (index math: 1 2 3 4 -> 2 3 4 1)
  b.lib->add_to_playlist(pb, tid(b, 5));          // B appends 5
  b.lib->remove_from_playlist(pb, 1);             // ...and removes track 2
  b.lib->add_to_playlist(pb, tid(b, 6), 0);       // ...and prepends 6
  c.lib->add_to_playlist(pc, tid(c, 7));          // C appends 7 and duplicates track 3
  c.lib->add_to_playlist(pc, tid(c, 3));
  sync_all(s);
  expect_equal(s);
  const auto got = list_seeds(a, playlist_named(a, "Mix"), {1, 2, 3, 4, 5, 6, 7});
  const std::multiset<std::int64_t> have(got.begin(), got.end());
  CHECK(have.count(2) == 0);                                       // removal kept
  for (int keep : {1, 4, 5, 6, 7}) CHECK(have.count(keep) == 1);   // every addition kept, the moved track once
  CHECK(have.count(3) == 2);                                       // legal duplicate
  // Both devices move the same track to the end concurrently: it must not be duplicated.
  adv();
  a.lib->move_in_playlist(pa, 0, static_cast<int>(a.lib->playlist_track_ids(pa).size()) - 1);
  adv();
  b.lib->move_in_playlist(pb, 0, static_cast<int>(b.lib->playlist_track_ids(pb).size()) - 1);
  const auto before = a.lib->playlist_track_ids(pa).size();
  sync_all(s);
  expect_equal(s);
  CHECK(a.lib->playlist_track_ids(playlist_named(a, "Mix")).size() == before);
  // Concurrent reorders of different tracks.
  adv();
  a.lib->move_in_playlist(pa, 1, 0);
  b.lib->move_in_playlist(pb, 2, 4);
  sync_all(s);
  expect_equal(s);
  CHECK(a.lib->playlist_track_ids(playlist_named(a, "Mix")).size() == before);
  // Concurrent inserts at the very same spot keep both, same order everywhere.
  adv();
  a.lib->add_to_playlist(pa, tid(a, 8), 1);
  b.lib->add_to_playlist(pb, tid(b, 2), 1);
  sync_all(s);
  expect_equal(s);
  CHECK(a.lib->playlist_track_ids(playlist_named(a, "Mix")).size() == before + 2);
  // Playlist deleted on one device while another adds to it: delete wins everywhere.
  adv();
  a.lib->delete_playlist(pa);
  b.lib->add_to_playlist(pb, tid(b, 1));
  sync_all(s);
  expect_equal(s);
  CHECK(playlist_named(a, "Mix") == 0 && playlist_named(c, "Mix") == 0);
}

void test_folder_cycles() {
  Scenario s("cycles");
  Dev& a = s.add("A");
  Dev& b = s.add("B");
  Dev& c = s.add("C");
  const auto fx = a.lib->create_playlist(0, "X", true), fy = a.lib->create_playlist(0, "Y", true), fz = a.lib->create_playlist(0, "Z", true);
  (void)fz;
  sync_all(s);
  adv();
  CHECK(a.lib->move_playlist(playlist_named(a, "X"), playlist_named(a, "Y"), 0));  // X inside Y
  adv();
  CHECK(b.lib->move_playlist(playlist_named(b, "Y"), playlist_named(b, "X"), 0));  // Y inside X: a cycle once merged
  adv();
  CHECK(c.lib->move_playlist(playlist_named(c, "Z"), playlist_named(c, "Y"), 0));  // Z inside Y as well
  sync_all(s);
  expect_equal(s);
  CHECK(count(a, "SELECT count(*) FROM playlists") == 3);
  // No cycle: every folder reaches the top level.
  for (auto& d : s.devs) {
    CHECK(count(*d, "SELECT count(*) FROM playlists WHERE coalesce(parent_id, 0) = 0") >= 1);
    CHECK(count(*d,
                "WITH RECURSIVE up(id, depth) AS (SELECT id, 0 FROM playlists UNION ALL SELECT p.parent_id, depth + 1 FROM up JOIN playlists p"
                " ON p.id = up.id WHERE p.parent_id IS NOT NULL AND depth < 10) SELECT max(depth) FROM up") < 5);
  }
  (void)fx;
  (void)fy;
  // Deleting a folder deletes what is inside it; concurrent move out of it keeps the later decision deterministic.
  adv();
  const auto top = [&](Dev& d) { return count(d, "SELECT id FROM playlists WHERE coalesce(parent_id, 0) = 0 ORDER BY position LIMIT 1"); };
  a.lib->delete_playlist(top(a));
  sync_all(s);
  expect_equal(s);
}

void test_delete_vs_edit() {
  Scenario s("delete");
  Dev& a = s.add("A");
  Dev& b = s.add("B");
  add_track(a, 1, "Keep");
  const auto t2 = add_track(a, 2, "Doomed");
  add_track(b, 1, "Keep");
  add_track(b, 2, "Doomed");
  const auto pl = a.lib->create_playlist(0, "L", false);
  a.lib->add_to_playlist(pl, t2);
  a.lib->add_to_playlist(pl, tid(a, 1));
  sync_all(s);
  adv();
  a.lib->remove_track(tid(a, 2));
  adv();
  edit_title(b, tid(b, 2), "Edited meanwhile");
  sync_all(s);
  expect_equal(s);
  CHECK(count(a, "SELECT count(*) FROM tracks") == 1 && count(b, "SELECT count(*) FROM tracks") == 1);
  CHECK(b.lib->playlist_track_ids(playlist_named(b, "L")).size() == 1);
  // Rescanning the file brings the track back, and that propagates.
  adv();
  add_track(b, 2, "Back again");
  sync_all(s);
  expect_equal(s);
  CHECK(count(a, "SELECT count(*) FROM tracks") == 2 && title_of(a, 2) == "Back again");
}

void test_idempotent() {
  Scenario s("idem");
  Dev& a = s.add("A");
  Dev& b = s.add("B");
  for (int i = 1; i <= 5; ++i) add_track(a, i, "T" + std::to_string(i));
  const auto pl = a.lib->create_playlist(0, "P", false);
  for (int i = 1; i <= 5; ++i) a.lib->add_to_playlist(pl, tid(a, i));
  a.lib->set_hot_cue(tid(a, 1), 0, 100);
  sync_all(s);
  expect_equal(s);
  const std::string d0 = dump(a);
  const int files = s.files_in_folder();
  for (int i = 0; i < 3; ++i)
    for (auto& d : s.devs) {
      const auto r = sync(*d);
      CHECK(r.changes_captured == 0 && r.records_written == 0 && r.files_written == 0 && r.records_applied == 0);
    }
  CHECK(s.files_in_folder() == files);
  CHECK(dump(a) == d0 && dump(b) == d0);
  CHECK(a.eng->status().pending_records == 0 && a.eng->status().last_error.empty());
}

void test_crash_safety() {
  {  // a log truncated mid-record is reported and ignored; the valid part is used; the full file later wins
    Scenario s("trunc");
    Dev& a = s.add("A", 0, 3000);  // several small files
    Dev& b = s.add("B");
    for (int i = 1; i <= 12; ++i) add_track(a, i, "T" + std::to_string(i));
    sync(a);
    const std::string adir = s.folder + "/DeckHandSync/v1/" + a.eng->status().device_id;
    std::vector<std::string> logs;
    for (const auto& e : fs::directory_iterator(adir)) logs.push_back(e.path().string());
    std::sort(logs.begin(), logs.end());
    CHECK(logs.size() >= 3);
    for (const auto& l : logs) CHECK(fs::file_size(l) < 5000);  // small files
    std::string original;
    CHECK(detail::read_file(logs.back(), original));
    fs::resize_file(logs.back(), original.size() - 40);  // cut the trailer and part of the last record
    const auto r = sync(b);
    CHECK(r.files_corrupt == 1 && !r.warnings.empty());
    const auto partial = count(b, "SELECT count(*) FROM tracks");
    CHECK(partial >= 8 && partial <= 12);
    {  // the owner's full file arrives later (the cloud finished uploading)
      std::ofstream(logs.back(), std::ios::binary | std::ios::trunc) << original;
    }
    const auto r2 = sync(b);
    CHECK(r2.files_corrupt == 0);
    CHECK(count(b, "SELECT count(*) FROM tracks") == 12);
    sync_all(s);
    expect_equal(s);
    // Garbage, temp leftovers, a future format version and a stray empty file do not hurt.
    std::ofstream(adir + "/00000500.dhlog", std::ios::binary) << "garbage garbage garbage";
    std::ofstream(adir + "/.00000501.dhlog.tmp", std::ios::binary) << "half";
    std::string future = original;
    future[4] = 2;
    std::ofstream(adir + "/00000502.dhlog", std::ios::binary) << future;
    std::ofstream(adir + "/00000503.dhlog", std::ios::binary).close();
    const auto r3 = sync(b);
    CHECK(r3.ok && r3.warnings.size() >= 3 && count(b, "SELECT count(*) FROM tracks") == 12);
  }
  {  // unknown record types are skipped; unknown future records do not break a complete file
    detail::FileHeader h;
    h.device_id = "feedfeedfeedfeed";
    h.device_name = "Future";
    h.seq = 1;
    h.created_ms = 5;
    detail::FileWriter w(h);
    detail::RegRec r;
    r.kind = 'G';
    r.eid = "ui.skin";
    r.field = "v";
    r.st = {1'700'000'000'500ll, 0, h.device_id};
    r.v = detail::Val::text("neon");
    w.add(r);
    std::string bytes = w.finish();
    bytes.resize(bytes.size() - 14);  // drop the trailer (count 1 byte + crc 4 + type 1 + framing 8)
    const std::string payload = "xyz";
    const auto put32 = [&](std::string& o, std::uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<char>(v >> (8 * i))); };
    std::string body(1, static_cast<char>(0x7E));
    body += payload;
    put32(bytes, static_cast<std::uint32_t>(body.size()));
    bytes += body;
    put32(bytes, detail::crc32(body.data(), body.size()));
    std::string trailer(1, static_cast<char>(detail::kRecTrailer));
    trailer.push_back(2);  // two records counted
    put32(trailer, detail::crc32(bytes.data(), bytes.size()));
    put32(bytes, static_cast<std::uint32_t>(trailer.size()));
    bytes += trailer;
    put32(bytes, detail::crc32(trailer.data(), trailer.size()));
    Scenario s("unknown");
    Dev& a = s.add("A");
    fs::create_directories(s.folder + "/DeckHandSync/v1/feedfeedfeedfeed");
    std::ofstream(s.folder + "/DeckHandSync/v1/feedfeedfeedfeed/00000001.dhlog", std::ios::binary) << bytes;
    const auto res = sync(a);
    CHECK(res.files_corrupt == 0 && res.warnings.empty());
    CHECK(a.lib->setting("ui.skin") == "neon");
  }
  {  // kill between capture and write: the pending records survive a restart
    Scenario s("kill1");
    Dev& a = s.add("A");
    Dev& b = s.add("B");
    add_track(a, 1, "One");
    CHECK(a.eng->capture().ok);
    CHECK(a.eng->status().pending_records > 0 && s.files_in_folder() == 0);
    a.stop();
    a.start();
    CHECK(a.eng->status().pending_records > 0);
    sync_all(s);
    expect_equal(s);
    CHECK(count(b, "SELECT count(*) FROM tracks") == 1);
  }
  {  // crash right after a file was renamed into place but before the pending flags were cleared
    Scenario s("kill2");
    Dev& a = s.add("A");
    Dev& b = s.add("B");
    add_track(a, 1, "One");
    add_track(a, 2, "Two");
    a.fault = [](const char* st) { return std::string(st) == "write.file"; };
    adv();
    CHECK(!a.eng->sync_once().ok);
    a.fault = nullptr;
    a.stop();
    a.start();
    sync_all(s);  // re-exports the same records in a new file; merging them twice changes nothing
    expect_equal(s);
    CHECK(count(b, "SELECT count(*) FROM tracks") == 2);
    for (auto& d : s.devs) {
      const auto r = sync(*d);
      CHECK(r.records_written == 0 && r.records_applied == 0);
    }
  }
  {  // crash inside apply, before commit: nothing half-applied; the retry succeeds
    Scenario s("kill3");
    Dev& a = s.add("A");
    Dev& b = s.add("B");
    add_track(a, 1, "One");
    add_track(a, 2, "Two");
    a.lib->create_playlist(0, "P", false);
    sync(a);
    b.fault = [](const char* st) { return std::string(st) == "apply.commit"; };
    adv();
    CHECK(!b.eng->sync_once().ok);
    CHECK(count(b, "SELECT count(*) FROM tracks") == 0 && count(b, "SELECT count(*) FROM sync_files") == 0);
    b.fault = nullptr;
    sync_all(s);
    expect_equal(s);
    CHECK(count(b, "SELECT count(*) FROM tracks") == 2);
  }
  {  // folder not available (USB stick unplugged): an error, not a crash, and nothing is lost
    Scenario s("nofolder");
    Dev& a = s.add("A");
    add_track(a, 1, "One");
    a.stop();
    a.folder = s.root + "/not-mounted";
    a.start();
    const auto r = a.eng->sync_once();
    CHECK(!r.ok && !r.error.empty() && !a.eng->status().last_error.empty());
    CHECK(!fs::exists(a.folder));
  }
}

void test_clock_skew() {
  Scenario s("skew");
  Dev& a = s.add("A");
  Dev& b = s.add("B", -3'600'000);  // an hour behind
  add_track(a, 1, "Start");
  add_track(b, 1, "Start");
  sync_all(s);
  adv();
  edit_title(a, tid(a, 1), "A edit");
  sync(a);
  sync(b);  // B has now seen A's edit
  adv();
  edit_title(b, tid(b, 1), "B later edit");  // later in causality, an hour earlier on the wall clock
  sync_all(s);
  CHECK(title_of(a, 1) == "B later edit" && title_of(b, 1) == "B later edit");
  // Restarting the behind device must not rewind its logical clock.
  b.stop();
  b.start();
  adv();
  edit_title(b, tid(b, 1), "B again");
  sync_all(s);
  CHECK(title_of(a, 1) == "B again");
  // A still ahead edits again after seeing B's change: its edit is causally later and wins.
  adv();
  edit_title(a, tid(a, 1), "A final");
  sync_all(s);
  CHECK(title_of(a, 1) == "A final" && title_of(b, 1) == "A final");
  // Concurrent edits of one field converge to the same value on both.
  adv();
  TrackEdit ea, eb;
  ea.genre = "from A";
  eb.genre = "from B";
  a.lib->edit_track(tid(a, 1), ea);
  b.lib->edit_track(tid(b, 1), eb);
  sync_all(s);
  expect_equal(s);
}

void test_compaction() {
  Scenario s("compact");
  Dev& a = s.add("A", 0, 2500);
  s.add("B");
  for (int i = 1; i <= 10; ++i) add_track(a, i, "T" + std::to_string(i));
  const auto pl = a.lib->create_playlist(0, "P", false);
  for (int i = 1; i <= 10; ++i) a.lib->add_to_playlist(pl, tid(a, i));
  a.lib->set_hot_cue(tid(a, 1), 0, 10);
  sync_all(s);
  adv();
  edit_title(a, tid(a, 2), "Edited");
  a.lib->remove_track(tid(a, 3));
  sync_all(s);
  expect_equal(s);
  const std::string adir = s.folder + "/DeckHandSync/v1/" + a.eng->status().device_id;
  const auto count_files = [&] { return std::distance(fs::directory_iterator(adir), fs::directory_iterator()); };
  CHECK(count_files() > 3);
  adv();
  CHECK(a.eng->compact().ok);
  CHECK(count_files() == 1);
  CHECK((*fs::directory_iterator(adir)).path().extension() == ".dhsnap");
  // A device that never saw the old logs joins from the snapshot.
  Dev& c = s.add("C");
  sync(c);
  CHECK(count(c, "SELECT count(*) FROM tracks") == 9);
  sync_all(s);
  expect_equal(s);
  CHECK(title_of(c, 2) == "Edited");
  // Edits after compaction go to a new tail log; everyone follows. Compacting twice is fine too.
  adv();
  edit_title(a, tid(a, 4), "After snapshot");
  c.lib->add_to_playlist(playlist_named(c, "P"), tid(c, 1));
  sync_all(s);
  CHECK(a.eng->compact().ok);
  CHECK(c.eng->compact().ok);
  Dev& d = s.add("D");
  sync_all(s);
  expect_equal(s);
  CHECK(title_of(d, 4) == "After snapshot" && d.lib->playlist_track_ids(playlist_named(d, "P")).size() == 10);
  CHECK(count(d, "SELECT count(*) FROM tracks") == 9);  // the tombstone survived compaction
  const auto devs = d.eng->devices();
  CHECK(devs.size() == 4);
  for (const auto& dv : devs) CHECK(!dv.name.empty() && dv.files >= 1);
}

void test_big_library() {
  Scenario s("big");
  Dev& a = s.add("A");
  Dev& b = s.add("B");
  constexpr int kN = 5000;
  {
    Transaction tx(a.lib->db());
    for (int i = 1; i <= kN; ++i) {
      write_audio(file_for(a, i), i);
      NewTrack t;
      t.path = file_for(a, i);
      t.title = "Track " + std::to_string(i);
      t.artist = "Artist " + std::to_string(i % 97);
      t.bpm = 80 + i % 90;
      t.duration_ms = 180000 + i;
      a.lib->upsert_track(t);
    }
    tx.commit();
  }
  {
    Transaction tx(a.lib->db());
    const auto pl = a.lib->create_playlist(0, "Everything", false);
    std::vector<std::int64_t> ids;
    for (int i = 1; i <= kN; ++i) ids.push_back(i);
    a.lib->set_playlist_tracks(pl, ids);
    for (int i = 1; i <= kN; i += 5) a.lib->set_hot_cue(i, 0, 1000 + i);
    tx.commit();
  }
  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };
  auto t0 = Clock::now();
  const auto ra = sync(a);
  const double ta = ms(t0);
  t0 = Clock::now();
  const auto rb = sync(b);
  const double tb = ms(t0);
  t0 = Clock::now();
  const auto rb2 = sync(b);
  const double tb2 = ms(t0);
  t0 = Clock::now();
  sync(a);
  const double ta2 = ms(t0);
  std::printf("5000 tracks: A first sync %.0f ms (%d records, %d files), B first sync %.0f ms (%d applied), repeat B %.0f ms, repeat A %.0f ms\n",
              ta, ra.records_written, ra.files_written, tb, rb.records_applied, tb2, ta2);
  CHECK(rb.records_applied == ra.records_written && rb2.records_applied == 0 && rb2.records_written == 0);
  CHECK(count(b, "SELECT count(*) FROM tracks") == kN && count(b, "SELECT count(*) FROM playlist_tracks") == kN);
  CHECK(ta < 30000 && tb < 30000);
  expect_equal(s);
  // A small edit afterwards is cheap.
  adv();
  edit_title(a, 7, "Seven");
  t0 = Clock::now();
  const auto rc = sync(a);
  sync(b);
  std::printf("single edit round trip %.0f ms (%d record)\n", ms(t0), rc.records_written);
  CHECK(rc.records_written == 1 && title_of(b, 7) == "Seven");
}

}  // namespace

int main() {
  test_one_way_all_entities();
  test_path_hint_relink();
  test_concurrent_fields();
  test_playlists_merge();
  test_folder_cycles();
  test_delete_vs_edit();
  test_idempotent();
  test_crash_safety();
  test_clock_skew();
  test_compaction();
  test_big_library();
  std::error_code ec;
  fs::remove_all(fs::temp_directory_path() / ("dh_sync_fp_" + std::to_string(run_id())), ec);
  std::puts("sync: all checks passed");
  return 0;
}
