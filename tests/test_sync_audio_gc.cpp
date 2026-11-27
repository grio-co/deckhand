// Audio store clean-up: an object that no synced playlist references any more is removed after a grace period, never one
// another device referenced meanwhile, resumably, and the bytes freed are reported.
#include <algorithm>

#include "sync_dev.hpp"

using namespace syncdev;

namespace {

constexpr std::int64_t kDay = 24ll * 3600 * 1000;

Dev& device(Scenario& s, const char* name, const std::string& pass) {
  Dev& d = s.add(name, false);
  d.cfg.trust_path_hints = false;
  d.cfg.passphrase = pass;
  d.cfg.audio_cleanup_grace_ms = 10 * kDay;
  fs::create_directories(d.dir + "/music_in");
  d.cfg.music_folder = d.dir + "/music_in";
  d.start();
  return d;
}
std::vector<fs::path> objects(const std::string& folder) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(fs::path(folder) / "DeckHandSync" / "audio", ec))
    if (e.is_regular_file(ec) && e.path().extension() == ".dhau") out.push_back(e.path());
  std::sort(out.begin(), out.end());
  return out;
}
std::uint64_t size_sum(const std::vector<fs::path>& v) {
  std::uint64_t n = 0;
  std::error_code ec;
  for (const auto& p : v) n += fs::file_size(p, ec);
  return n;
}
int row_count(Dev& d, const char* sql) { return d.count(sql); }

void run(const std::string& pass) {
  Scenario s(pass.empty() ? "gc" : "gcenc");
  Dev& a = device(s, "A", pass);
  Dev& b = device(s, "B", pass);
  Dev& c = device(s, "C", pass);
  const auto t1 = add_track(a, 1, "One"), t2 = add_track(a, 2, "Two");
  const auto pl = a.lib->create_playlist(0, "Gig", false);
  a.lib->add_to_playlist(pl, t1);
  a.lib->add_to_playlist(pl, t2);
  a.cfg.audio_playlists = {pl};
  a.restart();
  s.sync_all();
  CHECK(a.eng->sync_audio().exported == 2);
  s.sync_all();
  CHECK(b.eng->sync_audio().ok && c.eng->sync_audio().ok);
  CHECK(objects(s.folder).size() == 2);

  // Still referenced: nothing happens, however long it takes.
  adv(40 * kDay);
  auto r = a.eng->sync_audio();
  CHECK(r.ok && r.removed == 0 && r.cleanup_pending == 0 && objects(s.folder).size() == 2);

  // The playlist drops track 2 on A. The object is kept through the grace period, on every device's call.
  adv();
  a.lib->remove_from_playlist(pl, 1);
  s.sync_all();
  r = b.eng->sync_audio();  // B is the first to notice
  CHECK(r.ok && r.removed == 0 && r.cleanup_pending == 1 && objects(s.folder).size() == 2);
  adv(5 * kDay);
  s.sync_all();
  r = a.eng->sync_audio();
  CHECK(r.ok && r.removed == 0 && r.cleanup_pending == 1 && objects(s.folder).size() == 2);

  // Another device references it again inside the grace period (synced only a moment before the cleanup runs): kept, and
  // the countdown restarts later.
  adv(4 * kDay);
  const auto bt2 = row_of(b, fp_of(2));
  CHECK(bt2 != 0);
  const auto bpl = b.lib->create_playlist(0, "Warmup", false);
  b.lib->add_to_playlist(bpl, bt2);
  b.sync();  // only B has synced: A's call must read B's new records itself
  adv(3 * kDay);
  r = a.eng->sync_audio();
  CHECK(r.ok && r.removed == 0 && r.freed_bytes == 0 && r.cleanup_pending == 0 && objects(s.folder).size() == 2);

  // B drops it again: a fresh countdown starts, then it is removed once, reporting the bytes.
  adv();
  b.lib->delete_playlist(bpl);
  s.sync_all();
  r = c.eng->sync_audio();
  CHECK(r.ok && r.removed == 0 && r.cleanup_pending == 1);
  adv(9 * kDay);
  r = c.eng->sync_audio();
  CHECK(r.removed == 0 && r.cleanup_pending == 1);
  // A leftover of a crashed removal on A is cleaned up on A's next call.
  const auto before = objects(s.folder);
  CHECK(before.size() == 2);
  const std::uint64_t total_before = size_sum(before);
  const fs::path leftover = before[0].parent_path() / (".00000000000000000000000000000000." + a.eng->status().device_id + ".tmp");
  write_bytes(leftover.string(), "cut off");
  adv(2 * kDay);
  r = a.eng->sync_audio();
  if (r.removed != 1) std::fprintf(stderr, "removed %d pending %d ok %d warnings %zu\n", r.removed, r.cleanup_pending, r.ok, r.warnings.size());
  for (const auto& w : r.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
  CHECK(r.ok && r.removed == 1 && r.freed_bytes > 0 && !fs::exists(leftover));
  const auto after = objects(s.folder);
  CHECK(after.size() == 1 && total_before - size_sum(after) == r.freed_bytes);
  CHECK(r.store_bytes == size_sum(after));
  // Nothing else is touched and the remaining audio still works: another call and a normal sync change nothing.
  r = a.eng->sync_audio();
  CHECK(r.ok && r.removed == 0 && objects(s.folder).size() == 1);
  s.sync_all();
  r = b.eng->sync_audio();
  CHECK(r.ok && r.removed == 0 && r.failed == 0 && objects(s.folder).size() == 1);

  // The device that still has the file republishes it when it is referenced again.
  adv();
  a.lib->add_to_playlist(pl, t2);
  s.sync_all();
  r = a.eng->sync_audio();
  CHECK(r.ok && r.exported == 1 && objects(s.folder).size() == 2);

  // A negative grace period never removes anything.
  adv();
  a.lib->remove_from_playlist(pl, 1);
  a.cfg.audio_cleanup_grace_ms = -1;
  a.restart();
  s.sync_all();
  adv(400 * kDay);
  r = a.eng->sync_audio();
  CHECK(r.ok && r.removed == 0 && objects(s.folder).size() == 2);

  // A zero grace period removes it at the first call that sees it unreferenced.
  a.cfg.audio_cleanup_grace_ms = 0;
  a.restart();
  s.sync_all();
  r = a.eng->sync_audio();
  CHECK(r.ok && r.removed == 1 && objects(s.folder).size() == 1);
  (void)row_count;
}

}  // namespace

int main() {
  run("");
  if (deckhand::sync::SyncEngine::encryption_available()) run("audio gc passphrase");
  else std::puts("encryption unavailable in this build: the encrypted run was skipped");
  std::puts("test_sync_audio_gc: all checks passed");
  return 0;
}
