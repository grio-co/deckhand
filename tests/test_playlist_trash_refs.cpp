// "In playlist" smart rules follow a playlist through the trash: a restore that had to give it a new id (the old one was
// handed to another playlist meanwhile) rewrites the rules that named it when it was deleted, live ones and ones in
// the trash, and undoing the restore takes them off it again.
#include <cstdio>
#include <filesystem>

#include "../src/app/trash_history.hpp"
#include "check.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {
std::int64_t rule_id(Library& lib, std::int64_t smart) {
  const auto p = lib.smart_playlist(smart);
  return p && !p->rules.empty() ? p->rules[0].id : -999;
}
SmartRule in_playlist(std::int64_t id) {
  SmartRule r;
  r.field = SmartRule::Field::InPlaylist;
  r.id = id;
  return r;
}
}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_playlist_trash_refs";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  {
    Library lib((dir / "lib.db").string());
    NewTrack n;
    n.path = (dir / "a.mp3").string();
    const auto a = lib.upsert_track(n);

    const std::int64_t keep = lib.create_playlist(0, "Keep", false);
    const std::int64_t target = lib.create_playlist(0, "Target", false);  // the highest id: it will be reused
    lib.add_to_playlist(target, a);
    const std::int64_t other_ref = lib.create_smart_playlist("Other", true, {in_playlist(keep)});
    const std::int64_t live = lib.create_smart_playlist("Live", true, {in_playlist(target)});
    const std::int64_t later = lib.create_smart_playlist("Later", true, {in_playlist(target)});
    CHECK(lib.smart_track_ids(live, "", SortKey::Title, false).size() == 1);

    lib.delete_smart_playlist(later);  // a smart playlist in the trash that names it too
    const std::int64_t entry = lib.delete_playlist(target);
    CHECK(entry > 0);
    CHECK(rule_id(lib, live) < 0 && rule_id(lib, other_ref) == keep);  // marker, untouched
    CHECK(lib.smart_track_ids(live, "", SortKey::Title, false).empty());

    // the id is handed to another playlist, holding the same track: the marker must not match it
    const std::int64_t squatter = lib.create_playlist(0, "Squatter", false);
    CHECK(squatter == target);
    lib.add_to_playlist(squatter, a);
    CHECK(lib.smart_track_ids(live, "", SortKey::Title, false).empty());

    std::int64_t back = 0;
    CHECK(lib.restore_playlist(entry, &back) && back != squatter && back > 0);
    CHECK(rule_id(lib, live) == back);
    CHECK(rule_id(lib, other_ref) == keep);
    CHECK(lib.smart_track_ids(live, "", SortKey::Title, false).size() == 1);

    // the smart playlist that sat in the trash comes back already following it
    const auto trash = lib.deleted_playlists();
    CHECK(trash.size() == 1 && trash[0].kind == "smart");
    std::int64_t later_back = 0;
    CHECK(lib.restore_playlist(trash[0].id, &later_back));
    CHECK(rule_id(lib, later_back) == back);

    // undo the restore: the playlist is trashed again and the rules go back to naming the trash entry
    const std::string want = "live:" + std::to_string(back) + ":playlist";
    bool undone = false;
    for (const HistoryStep& h : recent_history_steps(lib))
      if (is_playlist_trash_step(h) && h.first_new == want) {
        CHECK(undo_step(lib, h).status == UndoStatus::Ok);
        undone = true;
        break;
      }
    CHECK(undone);
    CHECK(lib.deleted_playlists().size() >= 1);
    CHECK(rule_id(lib, live) < 0);
    CHECK(lib.smart_track_ids(live, "", SortKey::Title, false).empty());

    // and restoring it again (redo) puts the rules back on it
    std::int64_t again = 0;
    for (const auto& d : lib.deleted_playlists())
      if (d.kind == "playlist") CHECK(lib.restore_playlist(d.id, &again));
    CHECK(again > 0 && again != squatter);
    CHECK(rule_id(lib, live) == again);

    // a folder: a rule naming a playlist inside it follows it too
    const std::int64_t folder = lib.create_playlist(0, "Folder", true);
    const std::int64_t inner = lib.create_playlist(folder, "Inner", false);
    const std::int64_t ref = lib.create_smart_playlist("Ref", true, {in_playlist(inner)});
    const std::int64_t fe = lib.delete_playlist(folder);
    CHECK(rule_id(lib, ref) < 0);
    std::int64_t fback = 0;
    CHECK(lib.restore_playlist(fe, &fback));
    const std::int64_t r = rule_id(lib, ref);
    CHECK(r > 0 && lib.playlist_track_ids(r).empty());
    bool found = false;
    for (const auto& p : lib.playlists())
      if (p.id == r && p.name == "Inner") found = true;
    CHECK(found);
  }
  fs::remove_all(dir, ec);
  std::puts("playlist trash refs ok");
  return 0;
}
