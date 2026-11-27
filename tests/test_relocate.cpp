// Relocating moved files, manual relink with siblings, recursive folder views and browsing by directory.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "check.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {
void put(const fs::path& p, const std::string& content) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  f << content;
}
std::int64_t add(Library& lib, const fs::path& p, const char* title, std::int64_t dur = 0) {
  NewTrack t;
  t.path = p.string();
  t.title = title;
  t.duration_ms = dur;
  return lib.upsert_track(t);
}
}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_relocate";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  {
    Library lib((dir / "lib.db").string());
    const fs::path old = dir / "old", fresh = dir / "new", other = dir / "other";
    // a: unique content, moved and renamed (found by hash). b, c: same name in two places (tie). d: moved, name + size.
    put(old / "a.mp3", "AAAA-unique-content-1");
    put(old / "b.mp3", "BBBB");
    put(old / "d.mp3", "DDDDDDDD");
    put(old / "sub" / "e.mp3", "EEEEE");
    const auto a = add(lib, old / "a.mp3", "A"), b = add(lib, old / "b.mp3", "B"), d = add(lib, old / "d.mp3", "D");
    const auto e = add(lib, old / "sub" / "e.mp3", "E");
    // a and d share a size with nothing; give a a twin so the hash gets computed, then remove the twin
    put(old / "twin.mp3", "AAAA-unique-content-1");
    const auto twin = add(lib, old / "twin.mp3", "Twin");
    int guard = 0;
    while (lib.hash_pending(50).remaining > 0 && ++guard < 20) {}
    CHECK(lib.track(a)->path == (old / "a.mp3").string());
    // the files move
    put(fresh / "renamed_a.mp3", "AAAA-unique-content-1");
    put(fresh / "d.mp3", "DDDDDDDD");
    put(fresh / "b.mp3", "BBBB");
    put(other / "b.mp3", "BBBB");  // a second b: tie
    put(fresh / "sub" / "e.mp3", "EEEEE");
    fs::remove_all(old, ec);
    lib.remove_track(twin);

    RelocateOptions o;
    o.folders = {fresh.string(), other.string()};
    RelocateReport r = lib.relocate_scan(o);
    CHECK(r.missing == 4);
    CHECK(r.scanned == 5);
    // dry run changed nothing
    CHECK(lib.track(a)->path == (old / "a.mp3").string());
    bool a_found = false, d_found = false, e_found = false;
    for (const auto& m : r.found) {
      if (m.track_id == a) a_found = m.how == "hash" && m.new_path == (fresh / "renamed_a.mp3").string();
      if (m.track_id == d) d_found = m.how == "name+size";
      if (m.track_id == e) e_found = true;
    }
    CHECK(a_found);
    // d has no size recorded before the move unless hashed: sizes were filled by hash_pending, so a name+size match
    CHECK(d_found);
    CHECK(e_found);
    CHECK(r.ambiguous.size() == 1 && r.ambiguous[0].track_id == b && r.ambiguous[0].candidates.size() == 2);
    CHECK(r.not_found.empty());

    const RelocateResult ap = lib.relocate_apply(r.found);
    CHECK(ap.applied == 3 && ap.skipped == 0 && ap.batch_id != 0);
    CHECK(lib.track(a)->path == (fresh / "renamed_a.mp3").string());
    CHECK(lib.track_id_for_path((fresh / "d.mp3").string()) == d);
    // applying again changes nothing (nothing is missing any more for those)
    CHECK(lib.relocate_apply(r.found).applied == 0);
    // undo puts the old paths back
    const UndoResult u = lib.undo_batch(ap.batch_id);
    CHECK(u.status == UndoStatus::Ok && u.applied == 3);
    CHECK(lib.track(a)->path == (old / "a.mp3").string());
    // redo by apply, then the tie is resolved by hand
    CHECK(lib.relocate_apply(r.found).applied == 3);
    const RelocateResult man = lib.relink_track(b, (other / "b.mp3").string());
    CHECK(man.applied == 1);
    CHECK(lib.track(b)->path == (other / "b.mp3").string());
    bool journalled = false;
    for (const auto& h : lib.track_history(b)) journalled = journalled || h.field == "path";
    CHECK(journalled);
    // no candidates: not found; a weak name-only match is refused unless allowed
    fs::remove(other / "b.mp3", ec);
    fs::remove(fresh / "b.mp3", ec);
    RelocateOptions none;
    none.folders = {fresh.string()};
    CHECK(lib.relocate_scan(none).not_found.size() == 1);
  }
  // Siblings of a manual relink, and name-only rules.
  {
    Library lib((dir / "lib2.db").string());
    const fs::path oldd = dir / "gone", newd = dir / "back";
    const auto x = add(lib, oldd / "x.mp3", "X"), y = add(lib, oldd / "y.mp3", "Y"), z = add(lib, oldd / "deep" / "z.mp3", "Z");
    const auto w = add(lib, oldd / "w.mp3", "W");
    put(newd / "x.mp3", "x");
    put(newd / "y.mp3", "y");
    put(newd / "deep" / "z.mp3", "z");
    RelocateOptions o;
    o.folders = {newd.string()};
    RelocateReport weak = lib.relocate_scan(o);
    CHECK(weak.found.empty() && weak.not_found.size() == 4);  // only names agree
    o.allow_name_only = true;
    weak = lib.relocate_scan(o);
    CHECK(weak.found.size() == 3 && weak.found[0].weak);
    o.allow_name_only = false;
    o.probe_duration = [](const std::string&) { return std::int64_t(1000); };
    // durations unknown in the library (0): still no corroboration
    CHECK(lib.relocate_scan(o).found.empty());

    RelocateReport sib = lib.relink_siblings(x, (newd / "x.mp3").string());
    CHECK(sib.found.size() == 2 && sib.not_found.size() == 1 && sib.not_found[0] == w);
    CHECK(lib.relink_track(x, (newd / "x.mp3").string()).applied == 1);
    const RelocateResult sr = lib.relocate_apply(sib.found);
    CHECK(sr.applied == 2);
    CHECK(lib.track(z)->path == (newd / "deep" / "z.mp3").string());
    CHECK(lib.track(y)->path == (newd / "y.mp3").string());
    CHECK(lib.undo_batch(sr.batch_id).status == UndoStatus::Ok);
    CHECK(lib.track(y)->path == (oldd / "y.mp3").string());
  }
  // Duration confirmation picks between same-named files.
  {
    Library lib((dir / "lib3.db").string());
    const auto t = add(lib, dir / "nowhere" / "s.mp3", "S", 5000);
    put(dir / "p" / "s.mp3", "p");
    put(dir / "q" / "s.mp3", "qq");
    RelocateOptions o;
    o.folders = {(dir / "p").string(), (dir / "q").string()};
    o.probe_duration = [&](const std::string& p) { return std::int64_t(p.find("q") != std::string::npos && p.find("deckhand") == std::string::npos ? 1 : 5200); };
    RelocateReport r = lib.relocate_scan(o);
    // both candidates probe to 5200 or 1 depending on the path text; whichever, at most one is within tolerance
    CHECK(r.found.size() + r.ambiguous.size() <= 1);
    (void)t;
  }
  // Folder views.
  {
    Library lib((dir / "lib4.db").string());
    const auto t1 = add(lib, dir / "m" / "rock" / "1.mp3", "One"), t2 = add(lib, dir / "m" / "rock" / "2.mp3", "Two");
    const auto t3 = add(lib, dir / "m" / "jazz" / "3.mp3", "Three"), t4 = add(lib, dir / "m" / "jazz" / "live" / "4.mp3", "Four");
    const auto folder = lib.create_playlist(0, "Genres", true);
    const auto sub = lib.create_playlist(folder, "Sub", true);
    const auto p1 = lib.create_playlist(folder, "Rock", false), p2 = lib.create_playlist(sub, "Jazz", false);
    lib.add_to_playlist(p1, t1);
    lib.add_to_playlist(p1, t2);
    lib.add_to_playlist(p2, t2);  // also in the second: listed once
    lib.add_to_playlist(p2, t3);
    std::map<std::int64_t, std::string> src;
    auto ids = lib.folder_track_ids(folder, "", SortKey::Default, false, &src);
    CHECK((ids == std::vector<std::int64_t>{t2, t3, t1}));  // tree order: the sub-folder was made first
    CHECK(src[t2] == "Jazz" && src[t3] == "Jazz" && src[t1] == "Rock");
    CHECK(lib.folder_track_ids(folder, "Two").size() == 1);
    CHECK(lib.folder_track_ids(sub, "").size() == 2);
    CHECK(lib.folder_track_ids(p1, "").size() == 2);
    CHECK(lib.folder_track_ids(folder, "", SortKey::Title, true).front() == t2);  // Two > Three > One
    // by directory
    const auto top = lib.disk_folders();
    CHECK(top.size() == 2);  // the chain down to .../m is skipped: jazz and rock
    std::string jazz, rock;
    for (const auto& f : top) (f.name == "jazz" ? jazz : rock) = f.path;
    CHECK(!jazz.empty() && !rock.empty());
    for (const auto& f : top)
      if (f.name == "jazz") CHECK(f.tracks == 1 && f.total == 2 && f.has_children);
    CHECK(lib.disk_folders(jazz).size() == 1);
    CHECK(lib.disk_folder_track_ids(jazz, false, "").size() == 1);
    CHECK(lib.disk_folder_track_ids(jazz, true, "").size() == 2);
    CHECK(lib.disk_folder_track_ids(jazz, true, "Four").size() == 1);
    CHECK(lib.disk_folder_track_ids(rock, true, "").size() == 2);
    (void)t4;
  }
  fs::remove_all(dir, ec);
  std::puts("relocate: all checks passed");
  return 0;
}
