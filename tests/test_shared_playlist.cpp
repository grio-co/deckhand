// Sharing one playlist between two libraries that are not the same library, through a folder (two Library files, one
// temp folder): tracks are matched by hash or artist / title / duration, adds, removals and moves merge, a track the
// other side lacks is shown as missing, and nothing breaks when a sync is repeated or a file is cut off.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "check.hpp"
#include "deckhand/sync/shared_playlist.hpp"

using namespace deckhand;
using namespace deckhand::sync;
namespace fs = std::filesystem;

namespace {

std::int64_t add(Library& lib, const std::string& root, const std::string& artist, const std::string& title, std::int64_t dur_ms,
                 const char* hash = nullptr) {
  NewTrack t;
  t.path = root + "/" + artist + " - " + title + ".mp3";
  t.artist = artist;
  t.title = title;
  t.duration_ms = dur_ms;
  const std::int64_t id = lib.upsert_track(t);
  if (hash) {
    Stmt u(lib.db(), "UPDATE tracks SET content_hash = ?1 WHERE id = ?2");
    u.bind(1, hash).bind(2, id);
    u.run();
  }
  return id;
}

std::vector<std::string> titles(Library& lib, std::int64_t pl) {
  std::vector<std::string> out;
  for (const std::int64_t id : lib.playlist_track_ids(pl)) out.push_back(lib.track(id)->title);
  return out;
}

struct Side {
  std::string db;
  std::unique_ptr<Library> lib;
  std::string root;
  SharedConfig cfg;
  std::int64_t clock = 0;
};

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_shared_playlist";
  fs::remove_all(dir);
  fs::create_directories(dir / "shared");
  const auto make = [&](const char* name, const char* label, const char* root, std::int64_t clock0) {
    auto s = std::make_unique<Side>();
    s->db = (dir / (std::string(name) + ".db")).string();
    s->lib = std::make_unique<Library>(s->db);
    s->root = root;
    s->cfg.folder = (dir / "shared").string();
    s->cfg.label = label;
    s->clock = clock0;
    Side* raw = s.get();
    s->cfg.clock = [raw] { return raw->clock += 1000; };
    static int n = 0;
    s->cfg.new_id = [name, raw] { return std::string(name) + std::to_string(++n) + std::to_string(raw->clock); };
    return s;
  };
  auto A = make("alice", "Alice", "/alice/music", 1000000);
  auto B = make("bob", "Bob", "/bob/files", 2000000);  // different folders and file names
  Library& a = *A->lib;
  Library& b = *B->lib;

  // Alice has t1..t5; Bob has t1, t2, t4, t5 (t3 is missing) and one of his own. t2 matches by hash, the others by name.
  const auto a1 = add(a, A->root, "Aya", "One", 200000), a2 = add(a, A->root, "Aya", "Two", 180000, "h-two");
  const auto a3 = add(a, A->root, "Ben", "Three", 240000), a4 = add(a, A->root, "Cy", "Four", 210000);
  const auto a5 = add(a, A->root, "Di", "Five", 190000);
  const auto b1 = add(b, B->root, "aya", "one", 200900), b2 = add(b, B->root, "Aya", "Two (other name)", 180000, "h-two");
  const auto b4 = add(b, B->root, "Cy", "Four", 211000), b5 = add(b, B->root, "Di", "Five", 190000);
  const auto b6 = add(b, B->root, "Ed", "Six", 100000);
  (void)a5;
  (void)b1;
  (void)b2;
  (void)b4;
  (void)b5;

  // 1. Alice shares [One, Two, Three, Four]; Bob joins.
  const auto pa = a.create_playlist(0, "Friday set", false);
  for (const auto id : {a1, a2, a3, a4}) a.add_to_playlist(pa, id);
  const SharedResult sh = share_playlist(a, pa, A->cfg);
  CHECK(sh.ok && sh.added == 4 && !sh.shared_id.empty());
  CHECK(!share_playlist(a, pa, A->cfg).ok);  // already shared
  const auto listed = list_shared_playlists(B->cfg);
  CHECK(listed.size() == 1 && listed[0].shared_id == sh.shared_id && listed[0].name == "Friday set" && listed[0].participants.size() == 1);
  const SharedResult jn = join_shared_playlist(b, sh.shared_id, B->cfg);
  CHECK(jn.ok && jn.missing == 1 && jn.received == 4);
  const auto pb = jn.playlist_id;
  CHECK(titles(b, pb) == (std::vector<std::string>{"one", "Two (other name)", "Four"}));  // Three is not in Bob's library
  auto entries = shared_playlist_entries(b, pb, B->cfg);
  CHECK(entries.size() == 4 && entries[2].missing && entries[2].track.title == "Three" && !entries[0].missing && !entries[3].missing);
  bool name_ok = false;
  for (const PlaylistNode& n : b.playlists()) name_ok = name_ok || (n.id == pb && n.name == "Friday set");
  CHECK(name_ok);

  // 2. A repeated sync changes nothing.
  CHECK(sync_shared_playlist(b, pb, B->cfg).ok);
  const SharedResult again = sync_shared_playlist(a, pa, A->cfg);
  CHECK(again.ok && again.added == 0 && again.removed == 0 && again.moved == 0);
  CHECK(titles(b, pb).size() == 3 && titles(a, pa).size() == 4);

  // 3. Bob adds Five at the end and removes One; Alice moves Four to the front. Both sync, in any order.
  b.add_to_playlist(pb, b5);  // (b5 = Five)
  b.remove_from_playlist(pb, 0);
  const SharedResult rb = sync_shared_playlist(b, pb, B->cfg);
  CHECK(rb.ok && rb.added == 1 && rb.removed == 1);
  a.move_in_playlist(pa, 3, 0);
  const SharedResult ra = sync_shared_playlist(a, pa, A->cfg);
  CHECK(ra.ok && ra.moved == 1 && ra.received == 2);  // Five arrived, One left
  CHECK(sync_shared_playlist(b, pb, B->cfg).ok);
  CHECK(titles(a, pa) == (std::vector<std::string>{"Four", "Two", "Three", "Five"}));
  CHECK(titles(b, pb) == (std::vector<std::string>{"Four", "Two (other name)", "Five"}));  // Three: missing for Bob
  entries = shared_playlist_entries(b, pb, B->cfg);
  CHECK(entries.size() == 4 && entries[2].missing && entries[2].track.title == "Three");

  // 4. Both add something at once: the two playlists end up the same.
  const auto a6 = add(a, A->root, "Ed", "Six", 100000);
  a.add_to_playlist(pa, a6);
  b.add_to_playlist(pb, b6, 0);  // Bob puts Six first
  CHECK(sync_shared_playlist(a, pa, A->cfg).ok && sync_shared_playlist(b, pb, B->cfg).ok && sync_shared_playlist(a, pa, A->cfg).ok &&
        sync_shared_playlist(b, pb, B->cfg).ok);
  const auto ea = shared_playlist_entries(a, pa, A->cfg), eb = shared_playlist_entries(b, pb, B->cfg);
  CHECK(ea.size() == eb.size() && ea.size() == 6);
  for (std::size_t i = 0; i < ea.size(); ++i) CHECK(ea[i].entry_id == eb[i].entry_id && ea[i].track.title == eb[i].track.title);
  CHECK(titles(a, pa).size() == 6 && titles(b, pb).size() == 5);

  // 5. The missing track arrives in Bob's library: it shows up in his playlist at its place, and is not added twice.
  const auto b3 = add(b, B->root, "Ben", "Three", 239500);
  (void)b3;
  b.add_to_playlist(pb, b3);  // he also adds it by hand to his playlist before syncing
  const SharedResult r5 = sync_shared_playlist(b, pb, B->cfg);
  CHECK(r5.ok && r5.added == 0 && r5.missing == 0);
  CHECK(shared_playlist_entries(b, pb, B->cfg).size() == 6 && titles(b, pb).size() == 6);
  CHECK(sync_shared_playlist(a, pa, A->cfg).ok && shared_playlist_entries(a, pa, A->cfg).size() == 6);

  // 6. A removal by Alice wins over nothing, and a rename travels.
  a.remove_from_playlist(pa, 0);
  a.rename_playlist(pa, "Friday set v2");
  CHECK(sync_shared_playlist(a, pa, A->cfg).ok && sync_shared_playlist(b, pb, B->cfg).ok);
  CHECK(titles(b, pb).size() == 5 && titles(a, pa).size() == 5);
  bool renamed = false;
  for (const PlaylistNode& n : b.playlists()) renamed = renamed || (n.id == pb && n.name == "Friday set v2");
  CHECK(renamed);

  // 7. A participant file that was cut off is ignored until it is whole, and nothing is lost.
  {
    fs::path file;
    for (const auto& e : fs::recursive_directory_iterator(dir / "shared"))
      if (e.path().extension() == ".dhsp" && e.path().filename().string().rfind("bob", 0) == 0) file = e.path();
    CHECK(!file.empty());
    std::ifstream in(file, std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    {
      std::ofstream o(file, std::ios::binary | std::ios::trunc);
      o << all.substr(0, all.size() / 2);
    }
    CHECK(sync_shared_playlist(a, pa, A->cfg).ok);  // Alice sees only her own file; her playlist is not emptied of Bob's entries by this
    {
      std::ofstream o(file, std::ios::binary | std::ios::trunc);
      o << all;
    }
    CHECK(sync_shared_playlist(a, pa, A->cfg).ok);
    CHECK(shared_playlist_entries(a, pa, A->cfg).size() == 5);
  }

  // 8. Unlinking leaves the local playlist alone; an unshared playlist cannot be synced.
  unlink_shared_playlist(b, pb);
  CHECK(!sync_shared_playlist(b, pb, B->cfg).ok && titles(b, pb).size() == 5);
  CHECK(!join_shared_playlist(b, "nonexistent", B->cfg).ok);

  A.reset();  // close both libraries first: Windows refuses to delete files that are still open
  B.reset();
  std::error_code ec;
  fs::remove_all(dir, ec);
  std::puts("test_shared_playlist ok");
  return 0;
}
