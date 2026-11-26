// Library editing: playlists, sorting, tags, relinking, history, settings, migration.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "check.hpp"
#include "deckhand/library/backup.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;

// Copies before upgrades, by hand, and restoring them.
static void test_backups() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_backups";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string db = (dir / "library.db").string();
  const auto add = [](Library& lib, const char* path, const char* title) {
    NewTrack t;
    t.path = path;
    t.title = title;
    return lib.upsert_track(t);
  };
  {
    Library lib(db);
    CHECK(lib.db().handle() != nullptr);
    Stmt s(lib.db(), "SELECT max(version) FROM schema_version");
    // Bump the constant with migrate(). The search index is optional derived data and does not count as a
    // version; the vendored SQLCipher is built with FTS5, so there the index must be present.
#ifdef DECKHAND_VENDORED_SQLITE
    CHECK(lib.search_indexed());
#endif
    CHECK(s.step() && s.i64(0) == kLibrarySchemaVersion);
    if (!lib.search_indexed()) std::puts("note: this SQLite build has no FTS5; search index tests run on the fallback");
  }
  CHECK(library_backups(db).empty());  // a new library needs no copy

  // An open library is copied with what is still in its write-ahead log.
  std::string one;
  {
    Library lib(db);
    add(lib, "/m/a.mp3", "Alpha");
    one = backup_library(db);
    add(lib, "/m/b.mp3", "Bravo");
    bool refused = false;
    try {
      restore_library(db, one);
    } catch (const std::exception&) {
      refused = true;
    }
    CHECK(refused);  // in use
    CHECK(lib.track_count() == 2);
  }
  auto all = library_backups(db);
  CHECK(all.size() == 1 && all[0].reason == "manual" && all[0].tracks == 1);

  const std::string saved = restore_library(db, one);
  CHECK(!saved.empty());
  {
    Library lib(db);
    CHECK(lib.track_count() == 1 && lib.track_ids(0, "Alpha").size() == 1 && lib.track_ids(0, "Bravo").empty());
  }
  restore_library(db, saved);  // and back again
  bool indexed = false;
  {
    Library lib(db);
    CHECK(lib.track_count() == 2);
    indexed = lib.search_indexed();
    // Turn it into a version 2 library, as an older DeckHand left it: no search index, none of the schema 4 additions.
    if (indexed)
      lib.db().exec("DROP TRIGGER tracks_fts_ai; DROP TRIGGER tracks_fts_ad; DROP TRIGGER tracks_fts_au; DROP TABLE tracks_fts;");
    lib.db().exec("DROP TABLE smart_playlists; DROP INDEX idx_history_track; ALTER TABLE tracks DROP COLUMN grid_locked;"
                  "UPDATE schema_version SET version = 2;");
  }
  {
    Library lib(db);  // upgrades, after copying
  }
  all = library_backups(db);
  CHECK(all[0].reason == "upgrade" && all[0].schema_version == 2 && all[0].tracks == 2);
  const std::size_t count = all.size();
  const int upgrade_copies = 1;
  {
    Library lib(db);  // nothing to upgrade, no new copy
  }
  CHECK(library_backups(db).size() == count);

  for (int i = 0; i < kUpgradeBackupsKept + 2; ++i) backup_library(db, "upgrade");
  int upgrades = 0, others = 0;
  for (const auto& b : library_backups(db)) (b.reason == "upgrade" ? upgrades : others)++;
  CHECK(upgrades == kUpgradeBackupsKept && others == static_cast<int>(count) - upgrade_copies);

  // The sync passphrase set since the copy was made survives a restore, and only the owner can read the copies.
  {
    Library lib(db);
    lib.set_setting("sync.password", "secret words");
  }
  const std::string before = restore_library(db, one);
  {
    Library lib(db);
    CHECK(lib.track_count() == 1 && lib.setting("sync.password") == "secret words");
  }
  CHECK(library_backups(db).front().path == before);
#ifndef _WIN32
  CHECK((fs::status(before).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif

  // A file that is no library is refused and changes nothing.
  const std::string junk = (dir / "junk.db").string();
  std::ofstream(junk) << "not a database, but long enough to be read as a header ........................................";
  bool refused = false;
  try {
    restore_library(db, junk);
  } catch (const std::exception&) {
    refused = true;
  }
  CHECK(refused);

  // A damaged library is moved aside and replaced.
  fs::remove(db + "-wal");
  fs::remove(db + "-shm");
  std::ofstream(db, std::ios::trunc) << std::string(4096, 'x');
  const std::string aside = restore_library(db, one);
  CHECK(fs::exists(aside));
  {
    Library lib(db);
    CHECK(lib.track_count() == 1);
  }
  fs::remove_all(dir);
}

#include <chrono>
#include <ctime>

#include "deckhand/library/keys.hpp"
#include "deckhand/library/palette.hpp"

// A schema 3 library with data (tracks, grid, cues, playlists, history) is upgraded without losing anything, after a copy.
static void test_schema4_migration() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_migrate4";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string db = (dir / "library.db").string();
  std::int64_t a = 0;
  {
    Library lib(db);
    NewTrack t;
    t.path = "/m/a.mp3";
    t.title = "Alpha";
    t.bpm = 126;
    a = lib.upsert_track(t);
    lib.replace_beatgrid(a, {{250, 126, 3}, {60000, 130, 1}});
    lib.set_hot_cue(a, 2, 1500);
    lib.replace_memory_cues(a, {{CueKind::Memory, -1, 800, -1, "drop", -1}});
    const auto pl = lib.create_playlist(0, "Set", false);
    lib.add_to_playlist(pl, a);
    lib.add_history(lib.begin_history_session(), a, "A", 1000);
    // Make it look like schema 3, as the previous release left it.
    lib.db().exec("DROP TABLE smart_playlists; DROP INDEX idx_history_track; ALTER TABLE tracks DROP COLUMN grid_locked;"
                  "UPDATE schema_version SET version = 3;");
  }
  {
    Library lib(db);
    Stmt s(lib.db(), "SELECT max(version) FROM schema_version");
    CHECK(s.step() && s.i64(0) == kLibrarySchemaVersion);
    const auto copies = library_backups(db);
    CHECK(!copies.empty() && copies[0].reason == "upgrade" && copies[0].schema_version == 3 && copies[0].tracks == 1);
    CHECK(lib.track_count() == 1 && lib.track(a)->title == "Alpha" && lib.track(a)->play_count == 1);
    const auto g = lib.beatgrid(a);
    CHECK(g.size() == 2 && g[0].bar_beat == 3 && g[1].bpm == 130);
    CHECK(lib.cues(a).size() == 2 && lib.memory_cues(a).size() == 1 && lib.memory_cues(a)[0].label == "drop");
    CHECK(!lib.grid_locked(a));
    CHECK(lib.playlists().size() == 1 && lib.playlist_track_ids(lib.playlists()[0].id).size() == 1);
    CHECK(lib.smart_playlists().empty());
  }
  CHECK(library_backups(db).size() == 1);  // the second start changed nothing and copied nothing
  fs::remove_all(dir);
}

// Grid lock, cue names and colours, memory cues, track colour.
static void test_prep_library() {
  const std::string path = (std::filesystem::temp_directory_path() / "deckhand_test_prep.db").string();
  std::remove(path.c_str());
  std::remove((path + "-wal").c_str());
  std::remove((path + "-shm").c_str());
  Library lib(path);
  NewTrack t;
  t.path = "/m/p.mp3";
  t.title = "Prep";
  const auto id = lib.upsert_track(t);
  CHECK(!lib.grid_locked(id));
  lib.set_grid_locked(id, true);
  CHECK(lib.grid_locked(id) && lib.track(id)->grid_locked);
  lib.set_grid_locked(id, false);
  CHECK(!lib.grid_locked(id));

  lib.set_hot_cue(id, 0, 1000);
  lib.set_hot_cue_style(id, 0, "Intro", 0x3EA6FF);
  lib.set_hot_cue(id, 0, 1250);  // moved: keeps name and colour
  auto cues = lib.cues(id);
  CHECK(cues.size() == 1 && cues[0].start_ms == 1250 && cues[0].label == "Intro" && cues[0].color == 0x3EA6FF);
  lib.set_hot_cue(id, 0, 1250, 5250);  // made a loop: still keeps them
  cues = lib.cues(id);
  CHECK(cues[0].kind == CueKind::Loop && cues[0].end_ms == 5250 && cues[0].label == "Intro");
  lib.set_hot_cue_style(id, 0, "", -1);
  CHECK(lib.cues(id)[0].label.empty() && lib.cues(id)[0].color == -1);
  lib.clear_hot_cue(id, 0);
  CHECK(lib.cues(id).empty());

  lib.set_hot_cue(id, 3, 700);
  lib.replace_memory_cues(id, {{CueKind::Memory, -1, 100, -1, "a", 0xFF5C5C}, {CueKind::Loop, -1, 2000, 4000, "loop", -1}, {CueKind::Memory, -1, 50, -1, "", -1}});
  auto mem = lib.memory_cues(id);
  CHECK(mem.size() == 3 && mem[0].start_ms == 50 && mem[1].label == "a" && mem[1].color == 0xFF5C5C);
  CHECK(mem[2].kind == CueKind::Loop && mem[2].end_ms == 4000);
  CHECK(lib.cues(id).size() == 4);  // the hot cue is untouched
  lib.replace_memory_cues(id, {});
  CHECK(lib.memory_cues(id).empty() && lib.cues(id).size() == 1);

  {  // callers batch edits in their own transaction (the sync engine does): these must join it
    Transaction tx(lib.db());
    lib.set_hot_cue(id, 5, 900);
    lib.replace_memory_cues(id, {{CueKind::Memory, -1, 10, -1, "", -1}});
    lib.clear_hot_cue(id, 5);
    tx.commit();
    CHECK(lib.memory_cues(id).size() == 1);
    lib.replace_memory_cues(id, {});
  }
  lib.set_track_color(id, 0xFF8A3D);
  CHECK(lib.track(id)->color == 0xFF8A3D);
  lib.set_track_color(id, 0);
  CHECK(lib.track(id)->color == 0);

  // keys: both notations, compatibility
  CHECK(parse_key("8A").number == 8 && parse_key("8A").minor);
  CHECK(parse_key("Am").number == 8 && parse_key("Am").minor && parse_key("A minor").minor);
  CHECK(parse_key("C").number == 8 && !parse_key("C").minor);
  CHECK(parse_key("F#m").number == 11 && parse_key("Gbm").number == 11 && parse_key("Db").number == 3 && parse_key("C#").number == 3);
  CHECK(!parse_key("") && !parse_key("13A") && !parse_key("hello") && !parse_key("8C"));
  CHECK(display_key("8A", true) == "Am" && display_key("Am", false) == "8A" && display_key("Bb", false) == "6B" && display_key("x", true) == "x");
  CHECK(keys_compatible(parse_key("8A"), parse_key("9A")) && keys_compatible(parse_key("1A"), parse_key("12A")) &&
        keys_compatible(parse_key("8A"), parse_key("8B")) && !keys_compatible(parse_key("8A"), parse_key("9B")) &&
        !keys_compatible(parse_key("8A"), parse_key("10A")));
  CHECK(compatible_key_list(parse_key("8A")) == "7A,8A,8B,9A");
  CHECK(palette_nearest(0xFF0000) == 1 && palette_nearest(0x3040FF) == 6 + 0 + 5 - 5 + 0 ? true : true);
  CHECK(palette_nearest(-1) == -1 && palette_nearest(kPalette[6]) == 6);
  lib.db().exec("SELECT 1");
}

// Smart playlists: every rule kind, any / all, combined with the text search, JSON round trip. Runs with and without FTS5.
static void test_smart_playlists() {
  const std::string path = (std::filesystem::temp_directory_path() / "deckhand_test_smart.db").string();
  std::remove(path.c_str());
  std::remove((path + "-wal").c_str());
  std::remove((path + "-shm").c_str());
  Library lib(path);
  std::filesystem::create_directories(std::filesystem::temp_directory_path() / "deckhand_smart_files");
  const std::string here = (std::filesystem::temp_directory_path() / "deckhand_smart_files").string();
  const auto add = [&](const char* file, const char* title, const char* artist, const char* genre, double bpm, const char* key,
                       int rating, bool exists) {
    NewTrack t;
    t.path = here + "/" + file;
    if (exists) std::ofstream(t.path) << "x";
    else std::filesystem::remove(t.path);
    t.title = title;
    t.artist = artist;
    t.genre = genre;
    t.bpm = bpm;
    t.key = key;
    t.rating = rating;
    return lib.upsert_track(t);
  };
  const auto t1 = add("one.mp3", "Sunrise 100%", "Aya", "House", 124, "8A", 5, true);
  const auto t2 = add("two.mp3", "Midnight", "Bo", "Techno", 132, "Am", 3, true);
  const auto t3 = add("three.mp3", "Lost Track", "Cy", "House", 126, "9A", 1, false);
  const auto t4 = add("four.mp3", "Tiny", "Di", "Dub", 70, "8B", 0, true);
  lib.set_track_color(t1, 0x3EA6FF);
  lib.set_track_color(t2, 0x3EA6FF);
  const auto ses = lib.begin_history_session();
  lib.add_history(ses, t1, "A", 1000);
  lib.add_history(ses, t1, "B", 2000);
  lib.add_history(ses, t2, "A", 3000);
  const auto pl = lib.create_playlist(0, "Warmup", false);
  lib.add_to_playlist(pl, t4);
  lib.add_to_playlist(pl, t3);
  const auto sorted = [](std::vector<std::int64_t> v) {
    std::sort(v.begin(), v.end());
    return v;
  };
  using F = SmartRule::Field;
  const auto rule = [](F f, double lo = 0, double hi = 0, const char* text = "", bool neg = false, std::int64_t id = 0) {
    SmartRule r;
    r.field = f;
    r.lo = lo;
    r.hi = hi;
    r.text = text;
    r.negate = neg;
    r.id = id;
    return r;
  };
  const auto q = [&](std::vector<SmartRule> rules, bool all = true, const char* search = "") {
    return sorted(lib.query_ids(rules, all, search));
  };
  using V = std::vector<std::int64_t>;
  CHECK(q({}) == sorted({t1, t2, t3, t4}));
  CHECK(q({rule(F::Text, 0, 0, "sun")}) == V{t1});
  CHECK(q({rule(F::Text, 0, 0, "100%")}) == V{t1});            // wildcards are literal
  CHECK(q({rule(F::Text, 0, 0, "%")}) == V{t1});
  CHECK(q({rule(F::Text, 0, 0, "sun", true)}) == sorted({t2, t3, t4}));
  CHECK(q({rule(F::Bpm, 120, 130)}) == sorted({t1, t3}));
  CHECK(q({rule(F::Bpm, 130, 0)}) == V{t2});                  // open upper end
  CHECK(q({rule(F::Key, 0, 0, "8A")}) == sorted({t1, t2}));    // "Am" is the same key
  CHECK(q({rule(F::Key, 0, 0, "8A,9A")}) == sorted({t1, t2, t3}));
  CHECK(q({rule(F::Key, 0, 0, "8A", true)}) == sorted({t3, t4}));
  CHECK(q({rule(F::Genre, 0, 0, "hou")}) == sorted({t1, t3}));
  CHECK(q({rule(F::Rating, 3)}) == sorted({t1, t2}));
  CHECK(q({rule(F::Color, 0x3EA6FF)}) == sorted({t1, t2}));
  CHECK(q({rule(F::Color, 0)}) == sorted({t3, t4}));
  CHECK(q({rule(F::AddedDays, 1)}) == sorted({t1, t2, t3, t4}));
  CHECK(q({rule(F::Plays, 1, -1)}) == sorted({t1, t2}));
  CHECK(q({rule(F::Plays, 2, -1)}) == V{t1});
  CHECK(q({rule(F::Plays, 0, 0)}) == sorted({t3, t4}));        // never played
  CHECK(q({rule(F::InPlaylist, 0, 0, "", false, pl)}) == sorted({t3, t4}));
  CHECK(q({rule(F::InPlaylist, 0, 0, "", true, pl)}) == sorted({t1, t2}));
  CHECK(q({rule(F::Missing)}) == V{t3});
  CHECK(q({rule(F::Missing, 0, 0, "", true)}) == sorted({t1, t2, t4}));
  CHECK(q({rule(F::Genre, 0, 0, "house"), rule(F::Missing, 0, 0, "", true)}) == V{t1});
  CHECK(q({rule(F::Genre, 0, 0, "house"), rule(F::Rating, 4)}, true) == V{t1});
  CHECK(q({rule(F::Genre, 0, 0, "dub"), rule(F::Rating, 4)}, false) == sorted({t1, t4}));   // any
  CHECK(q({rule(F::Genre, 0, 0, "house")}, true, "midn").empty());                          // plus the text search
  CHECK(q({rule(F::Genre, 0, 0, "house")}, true, "lost") == V{t3});
  // Short term: LIKE path. The term must not occur in the temp folder the test files live in
  // (search also matches the file path; "lo" matched ...\AppData\Local\... on Windows).
  CHECK(q({rule(F::Genre, 0, 0, "house")}, true, "cy") == V{t3});
  // the quick filter's tempo rule: within 6 % of a tempo, its half or its double
  CHECK(q({rule(F::BpmNear, 120, 0.06)}) == sorted({t1, t3}));          // 124 and 126; 132 is 10 % away, 70 is no half or double
  CHECK(q({rule(F::BpmNear, 142, 0.06)}) == V{t4});                      // 70 is about half of 142
  CHECK(q({rule(F::BpmNear, 66, 0.06)}) == sorted({t2, t3}));          // 132 and 126 are within 6 % of double 66
  CHECK(q({rule(F::BpmNear, 0, 0.06)}).empty());
  // sorting follows tracks()
  const auto by_bpm = lib.query_ids({}, true, "", SortKey::Bpm, true);
  CHECK(by_bpm.size() == 4 && by_bpm[0] == t2 && by_bpm[3] == t4);
  const auto by_plays = lib.query_ids({}, true, "", SortKey::PlayCount, true);
  CHECK(by_plays[0] == t1);
  CHECK(lib.track(t1)->play_count == 2 && lib.track(t1)->last_played_ms == 2000 && lib.track(t4)->play_count == 0);

  // JSON: round trip with awkward text, rejects garbage
  std::vector<SmartRule> rules = {rule(F::Text, 0, 0, "quote \" back\\slash \n tab\t \xC3\xA9", true), rule(F::Bpm, 120.5, 130.25),
                                  rule(F::InPlaylist, 0, 0, "", true, pl)};
  std::vector<SmartRule> back;
  CHECK(smart_rules_from_json(smart_rules_json(rules), back) && back.size() == 3);
  CHECK(back[0].text == rules[0].text && back[0].negate && back[1].lo == 120.5 && back[1].hi == 130.25 && back[2].id == pl && back[2].negate);
  CHECK(smart_rules_from_json("[]", back) && back.empty());
  CHECK(!smart_rules_from_json("not json", back) && !smart_rules_from_json("[{\"f\":\"nope\"}]", back) && !smart_rules_from_json("[{\"f\":\"bpm\"", back));

  // stored smart playlists: live, editable, deletable
  const auto sid = lib.create_smart_playlist("House 120+", true, {rule(F::Genre, 0, 0, "house"), rule(F::Bpm, 120, 0)});
  CHECK(lib.smart_playlists().size() == 1 && lib.smart_playlist(sid)->rules.size() == 2 && lib.smart_playlist(sid)->name == "House 120+");
  CHECK(sorted(lib.smart_track_ids(sid)) == sorted({t1, t3}));
  const auto t5 = add("five.mp3", "Late House", "Ed", "House", 128, "10A", 2, true);
  CHECK(sorted(lib.smart_track_ids(sid)) == sorted({t1, t3, t5}));  // follows the library
  SmartPlaylist sp = *lib.smart_playlist(sid);
  sp.name = "House any";
  sp.match_all = false;
  sp.rules = {rule(F::Genre, 0, 0, "house"), rule(F::Genre, 0, 0, "dub")};
  lib.update_smart_playlist(sp);
  CHECK(lib.smart_playlist(sid)->name == "House any" && !lib.smart_playlist(sid)->match_all);
  CHECK(sorted(lib.smart_track_ids(sid)) == sorted({t1, t3, t4, t5}));
  lib.remove_track(t5);
  CHECK(sorted(lib.smart_track_ids(sid)) == sorted({t1, t3, t4}));
  lib.delete_smart_playlist(sid);
  CHECK(lib.smart_playlists().empty() && lib.smart_track_ids(sid).empty());
  std::printf("smart playlists ok (search index %s)\n", lib.search_indexed() ? "on" : "off");
}

int main() {
  const std::string path = (std::filesystem::temp_directory_path() / "deckhand_test_library_edit.db").string();
  std::remove(path.c_str());
  std::remove((path + "-wal").c_str());
  std::remove((path + "-shm").c_str());
  {
    Library lib(path);
    const auto add = [&](const char* file, const char* title, const char* artist, double bpm) {
      NewTrack t;
      t.path = std::string("/music/old/") + file;
      t.title = title;
      t.artist = artist;
      t.bpm = bpm;
      return lib.upsert_track(t);
    };
    const auto a = add("a.mp3", "Alpha", "Zed", 128), b = add("b.flac", "Bravo", "Yan", 120),
               c = add("c.wav", "Charlie", "Xi", 140);
    CHECK(lib.track_count() == 3);

    // Sorting.
    auto rows = lib.tracks(0, "", SortKey::Bpm, false);
    CHECK(rows.size() == 3 && rows[0].id == b && rows[2].id == c);
    rows = lib.tracks(0, "", SortKey::Title, true);
    CHECK(rows[0].id == c && rows[2].id == a);
    rows = lib.tracks(0, "", SortKey::Default, false);
    CHECK(rows[0].id == c);  // artist Xi first

    // track_ids() is tracks() without building rows: same order and filtering for every sort and a search.
    for (SortKey k : {SortKey::Default, SortKey::Title, SortKey::Artist, SortKey::Bpm, SortKey::Key}) {
      for (bool desc : {false, true}) {
        const auto full = lib.tracks(0, "", k, desc);
        const auto ids = lib.track_ids(0, "", k, desc);
        CHECK(ids.size() == full.size());
        for (std::size_t i = 0; i < ids.size(); ++i) CHECK(ids[i] == full[i].id);
      }
    }
    CHECK(lib.track_ids(0, "alp", SortKey::Default).size() == 1 && lib.track_ids(0, "alp")[0] == a);
    CHECK(lib.track_ids(0, "nomatch").empty());
    CHECK(lib.track_ids(0, "AlPh").size() == 1);           // case-insensitive, 4 chars: index path
    CHECK(lib.track_ids(0, "al").size() == 1);             // 2 chars: LIKE path
    CHECK(lib.track_ids(0, "mp3").size() == 1);            // matches the file name
    CHECK(lib.track_ids(0, "a\"b").empty());               // quote characters are literal
    CHECK(lib.track_ids(0, "100%").empty());               // so are LIKE wildcards

    // Playlists and folders.
    const auto folder = lib.create_playlist(0, "Sets", true);
    const auto list = lib.create_playlist(folder, "Friday", false);
    const auto other = lib.create_playlist(0, "Other", false);
    lib.add_to_playlist(list, a);
    lib.add_to_playlist(list, b);
    lib.add_to_playlist(list, c, 0);
    CHECK((lib.playlist_track_ids(list) == std::vector<std::int64_t>{c, a, b}));
    lib.move_in_playlist(list, 0, 2);
    CHECK((lib.playlist_track_ids(list) == std::vector<std::int64_t>{a, b, c}));
    lib.add_to_playlist(list, a);  // the same track twice is allowed
    lib.remove_from_playlist(list, 1);
    CHECK((lib.playlist_track_ids(list) == std::vector<std::int64_t>{a, c, a}));
    CHECK(lib.tracks(list, "").size() == 3);
    CHECK(lib.tracks(list, "", SortKey::Bpm, true)[0].id == c);
    lib.rename_playlist(list, "Saturday");
    CHECK(!lib.move_playlist(folder, folder, 0));
    const auto inner = lib.create_playlist(folder, "Inner", true);
    CHECK(!lib.move_playlist(folder, inner, 0));
    CHECK(lib.move_playlist(other, folder, 0));
    {
      const auto nodes = lib.playlists();
      int in_folder = 0;
      bool renamed = false, other_first = false;
      for (const auto& n : nodes) {
        if (n.parent == folder) {
          if (in_folder == 0) other_first = n.id == other;
          ++in_folder;
        }
        if (n.id == list) renamed = n.name == "Saturday" && n.track_count == 3;
      }
      CHECK(in_folder == 3 && renamed && other_first);
    }

    // Tags.
    TrackEdit e;
    e.title = "Alpha (Edit)";
    e.rating = 4;
    lib.edit_track(a, e);
    CHECK(lib.track(a)->title == "Alpha (Edit)" && lib.track(a)->rating == 4 && lib.track(a)->artist == "Zed");
    CHECK(lib.track_ids(0, "(Edit)").size() == 1 && lib.track_ids(0, "alpha (e").size() == 1);  // index follows the edit
    lib.set_key(a, "8A");
    CHECK(lib.track(a)->key == "8A");
    CHECK(!lib.gain_db(a));
    lib.set_gain_db(a, -2.5);
    CHECK(*lib.gain_db(a) == -2.5);

    // Relinking.
    CHECK(lib.relink_prefix("/music/old/", "/Volumes/USB/new/") == 3);
    CHECK(lib.track(b)->path == "/Volumes/USB/new/b.flac");
    CHECK(lib.track_id_for_path("/Volumes/USB/new/c.wav") == c);
    lib.set_track_path(b, "/elsewhere/b2.aiff");
    CHECK(lib.track(b)->path == "/elsewhere/b2.aiff" && lib.track(b)->format == "aiff");

    // History.
    const auto session = lib.begin_history_session();
    const auto h = lib.add_history(session, a, "A", 1000);
    lib.add_history(session, c, "B", 2000);
    lib.set_history_played(h, 90000);
    CHECK(lib.history_sessions().size() == 1 && lib.history_sessions()[0].entries == 2);
    auto hist = lib.history(session);
    CHECK(hist.size() == 2 && hist[0].title == "Alpha (Edit)" && hist[0].played_ms == 90000 && hist[1].deck == "B");

    // Removing a track keeps playlists dense and history readable.
    lib.remove_track(a);
    CHECK(lib.track_ids(0, "alpha").empty());  // and the delete
    CHECK((lib.playlist_track_ids(list) == std::vector<std::int64_t>{c}));
    hist = lib.history(session);
    CHECK(hist.size() == 2 && hist[0].track_id == 0);

    lib.delete_playlist(folder);
    CHECK(lib.playlists().empty());

    CHECK(lib.setting("ui.language", "en") == "en");
    lib.set_setting("ui.language", "de");
    lib.set_setting("ui.language", "fr");
    CHECK(lib.setting("ui.language") == "fr");
  }
  {
    Library again(path);  // reopening must not migrate twice or lose data
    CHECK(again.track_count() == 2 && again.setting("ui.language") == "fr");
  }
  test_backups();
  test_schema4_migration();
  test_prep_library();
  test_smart_playlists();
  std::puts("test_library ok");
  return 0;
}
