// Playlist trash: delete / restore / purge of playlists, folders and smart playlists, undo and redo, schema 10 -> 11.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/library/backup.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {

struct Dir {
  fs::path p;
  explicit Dir(const char* name) {
    p = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
  }
  ~Dir() {
    std::error_code ec;
    fs::remove_all(p, ec);
  }
  std::string db() const { return (p / "library.db").string(); }
};

using Ids = std::vector<std::int64_t>;
using Names = std::vector<std::string>;

std::int64_t add(Library& lib, const std::string& name) {
  NewTrack t;
  t.path = "/music/" + name + ".mp3";
  t.title = name;
  t.artist = "Zed";
  t.bpm = 124;
  t.duration_ms = 200000;
  return lib.upsert_track(t);
}

std::int64_t count(Library& lib, const std::string& sql) {
  Stmt s(lib.db(), sql.c_str());
  return s.step() ? s.i64(0) : -1;
}

const PlaylistNode* find(const std::vector<PlaylistNode>& v, std::int64_t id) {
  for (const auto& n : v)
    if (n.id == id) return &n;
  return nullptr;
}

Names names_under(Library& lib, std::int64_t parent) {
  Names out;
  for (const auto& n : lib.playlists())
    if (n.parent == parent) out.push_back(n.name);
  return out;
}

void test_nested_folder_roundtrip() {
  Dir d("deckhand_test_plt_nested");
  Library lib(d.db());
  const auto a = add(lib, "a"), b = add(lib, "b"), c = add(lib, "c");
  const auto before = lib.create_playlist(0, "Before", false);
  const auto folder = lib.create_playlist(0, "Gigs", true);
  const auto after = lib.create_playlist(0, "After", false);
  const auto sub = lib.create_playlist(folder, "Club", true);
  const auto p1 = lib.create_playlist(folder, "Warmup", false);
  const auto p2 = lib.create_playlist(sub, "Peak", false);
  const auto p3 = lib.create_playlist(sub, "Closing", false);
  lib.set_playlist_tracks(p1, {c, a, b, a});
  lib.set_playlist_tracks(p2, {b, c});
  lib.set_playlist_tracks(p3, {a});
  lib.set_playlist_tracks(before, {a});
  lib.db().exec("INSERT INTO import_map(source, entity, source_id, local_id) VALUES ('src', 'playlist', 'x1', " + std::to_string(p2) + ")");
  lib.set_setting("shared.link." + std::to_string(p1), "shared\tS1\nparticipant\tP\ncounter\t3\nname\tWarmup\n");
  CHECK(names_under(lib, 0) == (Names{"Before", "Gigs", "After"}));

  const auto entry = lib.delete_playlist(folder);
  CHECK(entry > 0);
  CHECK(lib.playlists().size() == 2 && !find(lib.playlists(), folder) && !find(lib.playlists(), p2));
  CHECK(lib.playlist_track_ids(p1).empty() && lib.playlist_track_ids(p2).empty());
  CHECK(count(lib, "SELECT count(*) FROM import_map WHERE entity = 'playlist' AND local_id = " + std::to_string(p2)) == 0);
  CHECK(lib.setting("shared.link." + std::to_string(p1)).empty());  // a reused id must not inherit the link
  const auto trash = lib.deleted_playlists();
  CHECK(trash.size() == 1 && trash[0].id == entry && trash[0].kind == "folder" && trash[0].name == "Gigs");
  CHECK(trash[0].playlists == 3 && trash[0].folders == 2 && trash[0].tracks == 7 && trash[0].shared && trash[0].parent_id == 0);
  CHECK(trash[0].old_id == folder && trash[0].deleted_at > 0);

  std::int64_t restored = 0;
  CHECK(lib.restore_playlist(entry, &restored) && restored == folder);
  CHECK(lib.deleted_playlists().empty());
  const auto tree = lib.playlists();
  CHECK(tree.size() == 7 && find(tree, sub)->parent == folder && find(tree, p1)->parent == folder && find(tree, p2)->parent == sub);
  CHECK(find(tree, folder)->is_folder && find(tree, sub)->is_folder && !find(tree, p3)->is_folder);
  CHECK(names_under(lib, 0) == (Names{"Before", "Gigs", "After"}));
  CHECK(names_under(lib, folder) == (Names{"Club", "Warmup"}));
  CHECK(names_under(lib, sub) == (Names{"Peak", "Closing"}));
  CHECK(lib.playlist_track_ids(p1) == (Ids{c, a, b, a}));  // order and repeats
  CHECK(lib.playlist_track_ids(p2) == (Ids{b, c}) && lib.playlist_track_ids(p3) == (Ids{a}));
  CHECK(lib.playlist_track_ids(before) == (Ids{a}) && lib.playlist_track_ids(after).empty());
  CHECK(count(lib, "SELECT count(*) FROM import_map WHERE entity = 'playlist' AND source_id = 'x1' AND local_id = " + std::to_string(p2)) == 1);
  CHECK(lib.setting("shared.link." + std::to_string(p1)).find("shared\tS1") == 0);
  CHECK(!lib.restore_playlist(entry));  // gone from the trash now
  CHECK(count(lib, "SELECT count(*) FROM trash_playlist_nodes") == 0 && count(lib, "SELECT count(*) FROM trash_playlist_items") == 0);
}

void test_position_and_ids_in_use() {
  Dir d("deckhand_test_plt_position");
  Library lib(d.db());
  const auto p0 = lib.create_playlist(0, "P0", false), p1 = lib.create_playlist(0, "P1", false), p2 = lib.create_playlist(0, "P2", false);
  const auto e = lib.delete_playlist(p2);
  // The freed id is taken by another playlist: the restored one gets a new id but the same place.
  const auto other = lib.create_playlist(0, "Other", false);
  CHECK(other == p2);
  std::int64_t restored = 0;
  CHECK(lib.restore_playlist(e, &restored) && restored != p2);
  CHECK(names_under(lib, 0) == (Names{"P0", "P1", "P2", "Other"}));
  // Middle of the list: back between its neighbours.
  const auto e1 = lib.delete_playlist(p1);
  CHECK(names_under(lib, 0) == (Names{"P0", "P2", "Other"}));
  CHECK(lib.restore_playlist(e1) && names_under(lib, 0) == (Names{"P0", "P1", "P2", "Other"}));
  (void)p0;
  // Deleting an unknown id changes nothing.
  CHECK(lib.delete_playlist(98765) == 0 && lib.deleted_playlists().empty());
}

void test_smart_playlist() {
  Dir d("deckhand_test_plt_smart");
  Library lib(d.db());
  SmartRule r1;
  r1.field = SmartRule::Field::Bpm;
  r1.lo = 120;
  r1.hi = 128;
  SmartRule r2;
  r2.field = SmartRule::Field::Genre;
  r2.text = "house";
  r2.negate = true;
  const auto s0 = lib.create_smart_playlist("First", true, {r1});
  const auto s1 = lib.create_smart_playlist("House", false, {r1, r2});
  const auto s2 = lib.create_smart_playlist("Last", true, {});
  const auto e = lib.delete_smart_playlist(s1);
  CHECK(e > 0 && lib.smart_playlists().size() == 2 && !lib.smart_playlist(s1));
  const auto trash = lib.deleted_playlists();
  CHECK(trash.size() == 1 && trash[0].kind == "smart" && trash[0].name == "House" && trash[0].playlists == 1);
  // A new one with the same name appears meanwhile.
  lib.create_smart_playlist("House", true, {});
  std::int64_t restored = 0;
  CHECK(lib.restore_playlist(e, &restored) && restored == s1);
  const auto back = lib.smart_playlist(restored);
  CHECK(back && back->name == "House (restored)" && !back->match_all && back->rules.size() == 2);
  CHECK(back->rules[0].field == SmartRule::Field::Bpm && back->rules[0].lo == 120 && back->rules[0].hi == 128);
  CHECK(back->rules[1].field == SmartRule::Field::Genre && back->rules[1].text == "house" && back->rules[1].negate);
  const auto all = lib.smart_playlists();
  CHECK(all.size() == 4 && all[0].id == s0 && all[1].id == s1);  // back at its old place, between First and Last
  (void)s2;
}

void test_parent_gone_and_clashes() {
  Dir d("deckhand_test_plt_parent");
  Library lib(d.db());
  const auto a = add(lib, "a");
  const auto folder = lib.create_playlist(0, "Folder", true);
  const auto child = lib.create_playlist(folder, "Mix", false);
  lib.set_playlist_tracks(child, {a});
  const auto e_child = lib.delete_playlist(child);
  // Same name again next to it, restored twice: Mix, Mix (restored), Mix (restored 2)
  lib.create_playlist(folder, "mix", false);
  std::int64_t r1 = 0;
  CHECK(lib.restore_playlist(e_child, &r1));
  CHECK(lib.playlists().size() == 3 && find(lib.playlists(), r1)->name == "Mix (restored)" && find(lib.playlists(), r1)->parent == folder);
  const auto e2 = lib.delete_playlist(r1);
  lib.create_playlist(folder, "Mix (restored)", false);
  std::int64_t r2 = 0;
  CHECK(lib.restore_playlist(e2, &r2) && find(lib.playlists(), r2)->name == "Mix (restored 2)");
  CHECK(lib.playlist_track_ids(r2) == (Ids{a}));

  // The folder is deleted too: the playlist comes back at the top level.
  const auto e3 = lib.delete_playlist(r2);
  const auto e_folder = lib.delete_playlist(folder);
  CHECK(lib.playlists().empty() && lib.deleted_playlists().size() == 2);
  std::int64_t r3 = 0;
  CHECK(lib.restore_playlist(e3, &r3));
  CHECK(find(lib.playlists(), r3)->parent == 0 && lib.playlist_track_ids(r3) == (Ids{a}));
  // Restoring the folder later does not pull it back in; a restored folder is a valid parent again for later restores.
  std::int64_t rf = 0;
  CHECK(lib.restore_playlist(e_folder, &rf) && find(lib.playlists(), rf)->is_folder);
  CHECK(find(lib.playlists(), r3)->parent == 0);

  // A playlist (not a folder) now sits at the old parent id: the restore must not nest into it.
  const auto folder2 = lib.create_playlist(0, "F2", true);
  const auto inner = lib.create_playlist(folder2, "Inner", false);
  const auto e4 = lib.delete_playlist(inner);
  lib.delete_playlist(folder2);
  const auto flat = lib.create_playlist(0, "Flat", false);
  CHECK(flat == folder2);  // the old parent id now belongs to a plain playlist
  std::int64_t r4 = 0;
  CHECK(lib.restore_playlist(e4, &r4));
  const auto tree4 = lib.playlists();
  const auto* n4 = find(tree4, r4);
  CHECK(n4 && n4->parent == 0);
}

void test_missing_tracks_skipped() {
  Dir d("deckhand_test_plt_tracks");
  Library lib(d.db());
  const auto a = add(lib, "a"), b = add(lib, "b"), c = add(lib, "c");
  const auto pl = lib.create_playlist(0, "Set", false);
  lib.set_playlist_tracks(pl, {a, b, c});
  const auto e = lib.delete_playlist(pl);
  lib.remove_track(b);
  std::int64_t r = 0;
  CHECK(lib.restore_playlist(e, &r));
  CHECK(lib.playlist_track_ids(r) == (Ids{a, c}));
  CHECK(count(lib, "SELECT max(position) FROM playlist_tracks WHERE playlist_id = " + std::to_string(r)) == 1);  // no gap
  // Purging a track also drops its place in trashed playlists (the id could be reused).
  const auto e2 = lib.delete_playlist(r);
  lib.remove_track(c);
  lib.db().exec("UPDATE trash_tracks SET deleted_at = deleted_at - 40 * 86400");
  CHECK(lib.purge_deleted(30) == 2 && lib.deleted_playlists().size() == 1);
  CHECK(count(lib, "SELECT count(*) FROM trash_playlist_items WHERE track_id = " + std::to_string(c)) == 0);
  CHECK(lib.restore_playlist(e2, &r) && lib.playlist_track_ids(r) == (Ids{a}));
}

void test_undo_redo() {
  Dir d("deckhand_test_plt_undo");
  Library lib(d.db());
  const auto a = add(lib, "a"), b = add(lib, "b");
  const auto folder = lib.create_playlist(0, "F", true);
  const auto pl = lib.create_playlist(folder, "P", false);
  lib.set_playlist_tracks(pl, {b, a});
  SmartRule r;
  r.field = SmartRule::Field::Rating;
  r.lo = 3;
  const auto smart = lib.create_smart_playlist("S", true, {r});

  std::int64_t batch = 0;
  {
    EditScope scope("user");  // one batch for both deletes
    lib.delete_playlist(folder);
    lib.delete_smart_playlist(smart);
    batch = scope.batch();
  }
  CHECK(batch != 0 && lib.playlists().empty() && lib.smart_playlists().empty());
  const auto edits = lib.recent_edits(10);
  CHECK(edits.size() >= 2 && edits[1].field == "playlist_trash" && edits[0].field == "playlist_trash" && edits[0].track_id == 0 && edits[0].batch_id == batch);

  auto u = lib.undo_batch(batch);
  CHECK(u.status == UndoStatus::Ok && u.applied == 2 && u.batch_id != 0);
  CHECK(lib.deleted_playlists().empty() && lib.playlists().size() == 2 && lib.smart_playlists().size() == 1);
  CHECK(lib.playlist_track_ids(pl) == (Ids{b, a}) && find(lib.playlists(), pl)->parent == folder);
  CHECK(lib.smart_playlists()[0].rules.size() == 1 && lib.smart_playlists()[0].rules[0].lo == 3);

  // Redo = undo the undo: both go back to the trash.
  auto redo = lib.undo_batch(u.batch_id);
  CHECK(redo.status == UndoStatus::Ok && redo.applied == 2);
  CHECK(lib.playlists().empty() && lib.smart_playlists().empty() && lib.deleted_playlists().size() == 2);
  // And once more: back again.
  auto again = lib.undo_batch(redo.batch_id);
  CHECK(again.status == UndoStatus::Ok && lib.playlists().size() == 2 && lib.deleted_playlists().empty());
  CHECK(lib.playlist_track_ids(pl) == (Ids{b, a}));

  // A restore from the panel is journalled too, and can be undone.
  const auto e = lib.delete_playlist(pl);
  std::int64_t back = 0;
  CHECK(lib.restore_playlist(e, &back));
  const auto last = lib.recent_edits(1);
  CHECK(last.size() == 1 && last[0].field == "playlist_trash");
  CHECK(lib.undo_edit(last[0].id).status == UndoStatus::Ok && !find(lib.playlists(), back));

  // Purged from the trash: the undo reports it is gone and changes nothing.
  const auto last2 = lib.recent_edits(1);
  CHECK(lib.deleted_playlists().size() == 1);
  lib.purge_deleted_playlists(0);
  const auto st = lib.undo_edit(last2[0].id).status;
  CHECK(st == UndoStatus::NotFound || st == UndoStatus::Conflict);
  CHECK(lib.deleted_playlists().empty() && !find(lib.playlists(), pl));
}

void test_purge() {
  Dir d("deckhand_test_plt_purge");
  {
    Library lib(d.db());
    const auto a = add(lib, "a");
    const auto p1 = lib.create_playlist(0, "Old", false), p2 = lib.create_playlist(0, "New", false), p3 = lib.create_playlist(0, "Third", false);
    lib.set_playlist_tracks(p1, {a});
    const auto e1 = lib.delete_playlist(p1);
    const auto e2 = lib.delete_playlist(p2);
    lib.db().exec("UPDATE trash_playlist_entries SET deleted_at = deleted_at - 40 * 86400 WHERE id = " + std::to_string(e1));
    CHECK(lib.purge_deleted_playlists(30) == 1);
    CHECK(lib.deleted_playlists().size() == 1 && lib.deleted_playlists()[0].id == e2);
    CHECK(count(lib, "SELECT count(*) FROM trash_playlist_items") == 0 && count(lib, "SELECT count(*) FROM trash_playlist_nodes WHERE entry_id = " + std::to_string(e1)) == 0);
    CHECK(!lib.purge_deleted_playlist(e1) && lib.purge_deleted_playlist(e2) && lib.deleted_playlists().empty());
    const auto e3 = lib.delete_playlist(p3);
    lib.db().exec("UPDATE trash_playlist_entries SET deleted_at = deleted_at - 40 * 86400 WHERE id = " + std::to_string(e3));
  }
  {
    Library lib(d.db());  // opening purges like it does for tracks
    CHECK(lib.deleted_playlists().empty());
    const auto p = lib.create_playlist(0, "Fresh", false);
    lib.delete_playlist(p);
  }
  {
    Library lib(d.db());
    CHECK(lib.deleted_playlists().size() == 1);
    CHECK(lib.purge_deleted(0) == 0 && lib.deleted_playlists().empty());  // the track purge also drops playlists (same retention)
  }
}

void test_migration_from_10() {
  Dir d("deckhand_test_plt_migrate");
  std::int64_t pl = 0, smart = 0;
  {
    Library lib(d.db());
    const auto a = add(lib, "a");
    pl = lib.create_playlist(0, "Keep", false);
    lib.set_playlist_tracks(pl, {a});
    smart = lib.create_smart_playlist("Smart", true, {});
    // As schema 10 left it.
    lib.db().exec("DROP TABLE trash_playlist_entries; DROP TABLE trash_playlist_nodes; DROP TABLE trash_playlist_items;"
                  "DROP TABLE trash_playlist_import; UPDATE schema_version SET version = 10;");
  }
  {
    Library lib(d.db());
    CHECK(count(lib, "SELECT max(version) FROM schema_version") == 11 && kLibrarySchemaVersion == 11);
    const auto copies = library_backups(d.db());
    CHECK(!copies.empty() && copies[0].reason == "upgrade" && copies[0].schema_version == 10);
    CHECK(lib.playlists().size() == 1 && lib.playlist_track_ids(pl).size() == 1 && lib.smart_playlists().size() == 1);
    CHECK(lib.deleted_playlists().empty());
    const auto e = lib.delete_playlist(pl);
    const auto e2 = lib.delete_smart_playlist(smart);
    CHECK(e > 0 && e2 > 0 && lib.deleted_playlists().size() == 2);
    CHECK(lib.restore_playlist(e) && lib.restore_playlist(e2) && lib.playlist_track_ids(pl).size() == 1);
  }
}

void test_large_playlist_timing() {
  Dir d("deckhand_test_plt_large");
  Library lib(d.db());
  Ids ids;
  for (int i = 0; i < 10000; ++i) ids.push_back(add(lib, "t" + std::to_string(i)));
  const auto folder = lib.create_playlist(0, "Big", true);
  const auto pl = lib.create_playlist(folder, "Ten thousand", false);
  const auto pl2 = lib.create_playlist(folder, "Reversed", false);
  lib.set_playlist_tracks(pl, ids);
  Ids rev(ids.rbegin(), ids.rend());
  lib.set_playlist_tracks(pl2, rev);
  const auto t0 = std::chrono::steady_clock::now();
  const auto e = lib.delete_playlist(folder);
  const auto t1 = std::chrono::steady_clock::now();
  std::int64_t restored = 0;
  CHECK(lib.restore_playlist(e, &restored));
  const auto t2 = std::chrono::steady_clock::now();
  const auto ms = [](auto a, auto b) { return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count()); };
  std::printf("10k-track playlist x2: delete %lld ms, restore %lld ms\n", ms(t0, t1), ms(t1, t2));
  CHECK(lib.playlist_track_ids(pl) == ids && lib.playlist_track_ids(pl2) == rev);
  CHECK(ms(t0, t1) < 5000 && ms(t1, t2) < 5000);
}

}  // namespace

int main() {
  test_nested_folder_roundtrip();
  test_position_and_ids_in_use();
  test_smart_playlist();
  test_parent_gone_and_clashes();
  test_missing_tracks_skipped();
  test_undo_redo();
  test_purge();
  test_migration_from_10();
  test_large_playlist_timing();
  std::puts("test_playlist_trash ok");
  return 0;
}
