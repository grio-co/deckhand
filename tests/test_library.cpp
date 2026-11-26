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
    CHECK(s.step() && s.i64(0) == kLibrarySchemaVersion);  // bump the constant with migrate()
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
  {
    Library lib(db);
    CHECK(lib.track_count() == 2);
    // Turn it into a version 2 library, as an older DeckHand left it.
    lib.db().exec(
        "DROP TRIGGER tracks_fts_ai; DROP TRIGGER tracks_fts_ad; DROP TRIGGER tracks_fts_au; DROP TABLE tracks_fts;"
        "UPDATE schema_version SET version = 2;");
  }
  {
    Library lib(db);  // upgrades, after copying
  }
  all = library_backups(db);
  CHECK(all[0].reason == "upgrade" && all[0].schema_version == 2 && all[0].tracks == 2);
  const std::size_t count = all.size();
  {
    Library lib(db);  // nothing to upgrade, no new copy
  }
  CHECK(library_backups(db).size() == count);

  for (int i = 0; i < kUpgradeBackupsKept + 2; ++i) backup_library(db, "upgrade");
  int upgrades = 0, others = 0;
  for (const auto& b : library_backups(db)) (b.reason == "upgrade" ? upgrades : others)++;
  CHECK(upgrades == kUpgradeBackupsKept && others == static_cast<int>(count) - 1);

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
  std::puts("test_library ok");
  return 0;
}
