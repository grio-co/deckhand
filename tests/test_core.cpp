#include "check.hpp"
#include <cstdio>
#include <filesystem>

#include "deckhand/core/spsc.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/log/log.hpp"

using namespace deckhand;

int main() {
  log::set_level(log::Level::Debug);
  CHECK(log::enabled(log::Level::Info));
  CHECK(!log::enabled(log::Level::Trace));

  SpscRing<int, 4> ring;
  for (int i = 0; i < 4; ++i) CHECK(ring.push(i));
  CHECK(!ring.push(9));
  int v = -1;
  CHECK(ring.pop(v) && v == 0);

  const auto path = std::filesystem::temp_directory_path() / "deckhand_test_library.db";
  for (const char* s : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + s);
  {
    Library lib(path.string());
    NewTrack t;
    t.path = "/music/a.flac";
    t.title = "Alpha";
    t.artist = "Tester";
    t.bpm = 124.0;
    const auto id = lib.upsert_track(t);
    t.title = "Alpha (edit)";
    CHECK(lib.upsert_track(t) == id);  // same path updates, never duplicates
    CHECK(lib.track_count() == 1);
    const auto row = lib.track(id);
    CHECK(row && row->title == "Alpha (edit)" && row->path == "/music/a.flac" && row->format == "flac");
    lib.set_hot_cue(id, 0, 1000.0);
    lib.set_hot_cue(id, 0, 2000.0);
    CHECK(lib.cues(id).size() == 1 && lib.cues(id)[0].start_ms == 2000.0);
    lib.replace_beatgrid(id, {{120.0, 124.0, 1}});
    CHECK(lib.beatgrid(id).size() == 1);
    CHECK(lib.tracks(0, "alph").size() == 1 && lib.tracks(0, "zzz").empty());
  }
  for (const char* s : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + s);
  std::puts("core ok");
  return 0;
}
