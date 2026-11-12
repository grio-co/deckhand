// Library editing: playlists, sorting, tags, relinking, history, settings, migration.
#include <cstdio>
#include <filesystem>
#include <string>

#include "check.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;

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
  std::puts("test_library ok");
  return 0;
}
