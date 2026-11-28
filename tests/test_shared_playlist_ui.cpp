// The glue of the shared-playlist panel and status route: folder from the settings, link detection, share / join /
// sync / leave between two libraries through a temp folder, and what the status reports.
#include <cstdio>
#include <filesystem>
#include <memory>

#include "check.hpp"
#include "shared_playlist_ui.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {
std::int64_t add(Library& lib, const std::string& artist, const std::string& title) {
  NewTrack t;
  t.path = "/music/" + artist + " - " + title + ".mp3";
  t.artist = artist;
  t.title = title;
  t.duration_ms = 200000;
  return lib.upsert_track(t);
}
}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_shared_ui";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "folder");
  {
    auto a = std::make_unique<Library>((dir / "a.db").string());
    auto b = std::make_unique<Library>((dir / "b.db").string());
    const std::string folder = (dir / "folder").string();

    CHECK(shared_folder(*a).empty());
    a->set_setting("sync.folder", "/sync");
    CHECK(shared_folder(*a) == "/sync");
    a->set_setting("shared.folder", folder);  // the panel's own choice wins
    CHECK(shared_folder(*a) == folder);
    b->set_setting("shared.folder", folder);
    a->set_setting("sync.device_name", "Alice");
    CHECK(shared_settings_config(*a).label == "Alice" && shared_settings_config(*b, "Bob").label == "Bob");

    const auto t1 = add(*a, "X", "One"), t2 = add(*a, "Y", "Two");
    add(*b, "X", "One");  // B has only the first
    const auto pl = a->create_playlist(0, "Friday", false);
    a->add_to_playlist(pl, t1);
    a->add_to_playlist(pl, t2);

    const sync::SharedConfig ca = shared_settings_config(*a), cb = shared_settings_config(*b, "Bob");
    SharedStatus st = shared_status(*a, pl, ca);
    CHECK(!st.linked && st.folder_ok && st.shared_id.empty());
    CHECK(shared_join_choices(*b, cb).empty());

    SharedOutcome o = shared_share(*a, pl, ca);
    CHECK(o.ok && !o.shared_id.empty());
    CHECK(!shared_share(*a, pl, ca).ok);  // already shared
    st = shared_status(*a, pl, ca);
    CHECK(st.linked && st.shared_id == o.shared_id && st.entries == 2 && st.missing == 0 && !st.participants.empty());
    CHECK(shared_link_id(*a, pl) == o.shared_id);

    // B sees it, joins it, and has one of the two tracks
    const auto choices = shared_join_choices(*b, cb);
    CHECK(choices.size() == 1 && choices[0].shared_id == o.shared_id && choices[0].name == "Friday");
    o = shared_join(*b, choices[0].shared_id, 0, cb);
    CHECK(o.ok && o.playlist_id > 0);
    st = shared_status(*b, o.playlist_id, cb);
    CHECK(st.linked && st.entries == 2 && st.missing == 1);
    CHECK(b->playlist_track_ids(o.playlist_id).size() == 1);
    CHECK(shared_join_choices(*b, cb).empty());  // joined: no longer on offer

    // leaving: unlinked, the playlist stays, it is on offer again; the status route sees an unreachable folder
    shared_leave(*b, o.playlist_id);
    CHECK(!shared_status(*b, o.playlist_id, cb).linked && b->playlist_track_ids(o.playlist_id).size() == 1);
    CHECK(shared_join_choices(*b, cb).size() == 1);
    sync::SharedConfig bad = cb;
    bad.folder = (dir / "nowhere").string();
    CHECK(!shared_status(*a, pl, bad).folder_ok && shared_status(*a, pl, bad).linked);
    CHECK(shared_join_choices(*b, bad).empty());
    CHECK(shared_sync(*a, pl, ca).ok);
  }
  fs::remove_all(dir, ec);
  std::puts("test_shared_playlist_ui: ok");
  return 0;
}
