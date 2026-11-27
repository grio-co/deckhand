// The trash and edit-history lists: grouping of the journal into undoable steps, restore, empty.
#include <cstdio>
#include <filesystem>

#include "../src/app/trash_history.hpp"
#include "check.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

int main() {
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_browser_trash";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  {
    Library lib((dir / "lib.db").string());
    NewTrack n;
    n.path = (dir / "a.mp3").string();
    n.title = "Alpha";
    const auto a = lib.upsert_track(n);
    n.path = (dir / "b.mp3").string();
    n.title = "Beta";
    const auto b = lib.upsert_track(n);

    TrackEdit e1;
    e1.title = "Alpha 2";
    lib.edit_track(a, e1);
    TrackEdit e2;  // two fields in one operation: one step
    e2.artist = "Zed";
    e2.rating = 4;
    lib.edit_track(a, e2);
    TrackEdit e3;
    e3.title = "Beta 2";
    lib.edit_track(b, e3);

    auto steps = track_history_steps(lib, a);
    CHECK(steps.size() == 2);
    CHECK(steps[0].fields.size() == 2 && steps[0].source == "user");
    CHECK(steps[1].fields.size() == 1 && steps[1].fields[0] == "title" && steps[1].first_old == "Alpha" && steps[1].first_new == "Alpha 2");
    CHECK(recent_history_steps(lib).size() == 3);
    CHECK(recent_history_steps(lib).front().track_id == b);

    const UndoResult u = undo_step(lib, steps[0]);
    CHECK(u.status == UndoStatus::Ok && u.applied == 2);
    CHECK(lib.track(a)->rating == 0 && lib.track(a)->artist.empty() && lib.track(a)->title == "Alpha 2");
    CHECK(undo_step(lib, steps[0]).status == UndoStatus::Conflict);  // already undone
    steps = track_history_steps(lib, a);
    CHECK(steps[0].source == "undo");

    // trash: remove, list, restore, empty
    lib.remove_track(a);
    lib.remove_track(b);
    auto trash = lib.deleted_tracks();
    CHECK(trash.size() == 2);
    std::int64_t back = 0;
    CHECK(lib.restore_track(a, &back) && back == a && lib.track(a));
    CHECK(lib.deleted_tracks().size() == 1);
    CHECK(empty_trash(lib) == 1);
    CHECK(lib.deleted_tracks().empty());
    CHECK(!lib.restore_track(b));
  }
  fs::remove_all(dir, ec);
  std::puts("browser trash and history: all checks passed");
  return 0;
}
