// Audio files in library sync: chosen playlists' audio goes into a content-addressed store in the sync folder
// (de-duplicated, temp file + rename, verified by hash), and is imported on another device into its music folder and
// relinked, never overwriting a different file. Budgets, damaged objects, encryption and resuming are checked too.
#include <algorithm>
#include <tuple>

#include "sync_dev.hpp"

using namespace syncdev;

namespace {

Dev& device(Scenario& s, const char* name) {
  Dev& d = s.add(name, false);
  d.cfg.trust_path_hints = false;
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
int count_files(const std::string& dir) {
  int n = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_regular_file(ec)) ++n;
  return n;
}
bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::int64_t playlist_with(Dev& d, const std::vector<std::int64_t>& tracks) {
  const auto pl = d.lib->create_playlist(0, "Gig", false);
  for (const auto t : tracks) d.lib->add_to_playlist(pl, t);
  return pl;
}

// What the other device's row for `fp` points at, or "" when it is still a missing-file placeholder.
std::string path_of(Dev& d, const std::string& fp) {
  Stmt s(d.lib->db(), "SELECT s.path FROM sync_track s WHERE s.fp = ?1 AND s.missing = 0");
  s.bind(1, fp);
  return s.step() ? s.text(0) : std::string();
}

constexpr std::size_t kBig = 1200 * 1024;  // two chunks when encrypted

}  // namespace

int main() {
  const std::string fp1 = fp_of(1), fp2 = fp_of(2), fp3 = fp_of(3), fp4 = fp_of(4, kBig);
  {
    Scenario s("audio");
    Dev& a = device(s, "A");
    Dev& b = device(s, "B");
    const auto t1 = add_track(a, 1, "One"), t2 = add_track(a, 2, "Two"), t4 = add_track(a, 4, "Four", kBig);
    add_track(a, 3, "Three");  // not in the playlist
    const auto pl = playlist_with(a, {t1, t2, t4});
    a.cfg.audio_playlists = {pl};
    a.restart();
    a.sync();

    // Publish: three objects, named by hash, nothing for the track outside the playlist.
    adv();
    const auto ra = a.eng->sync_audio();
    CHECK(ra.ok && ra.exported == 3 && ra.skipped == 0 && ra.waiting_budget == 0 && ra.already_stored == 0);
    CHECK(ra.exported_bytes == 3000 + 3000 + kBig && ra.store_bytes >= ra.exported_bytes);
    CHECK(objects(s.folder).size() == 3);
    for (const auto& o : objects(s.folder)) {
      CHECK(o.filename().string().size() == 32 + 5 && o.parent_path().filename().string() == o.filename().string().substr(0, 2));
    }
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(s.folder) / "DeckHandSync" / "audio", ec))
      CHECK(e.path().filename().string()[0] != '.');  // no temporary files are left
    // Running it again does nothing (resumable and idempotent).
    const auto ra2 = a.eng->sync_audio();
    CHECK(ra2.ok && ra2.exported == 0 && ra2.already_stored == 3 && objects(s.folder).size() == 3);

    // The same audio under another track (another file name) is stored once.
    write_bytes(a.music + "/copy_of_one.mp3", audio_bytes(1));
    NewTrack nt;
    nt.path = a.music + "/copy_of_one.mp3";
    nt.title = "One again";
    const auto t5 = a.lib->upsert_track(nt);
    a.lib->add_to_playlist(pl, t5);
    const auto ra3 = a.eng->sync_audio();
    CHECK(ra3.ok && ra3.exported == 0 && objects(s.folder).size() == 3);

    // Import on the other device.
    b.sync();
    CHECK(b.count("SELECT count(*) FROM tracks") == 4 && path_of(b, fp1).empty());
    write_bytes(b.dir + "/music_in/.dhimport-stale.part", "left over");
    const auto rb = b.eng->sync_audio();
    CHECK(rb.ok && rb.imported == 3 && rb.imported_bytes == 3000 + 3000 + kBig && rb.failed == 0 && rb.waiting_files == 0);
    CHECK(!fs::exists(b.dir + "/music_in/.dhimport-stale.part"));
    CHECK(count_files(b.dir + "/music_in") == 3);
    for (const auto& [fp, seed, size] : {std::tuple<std::string, int, std::size_t>{fp1, 1, 3000}, {fp2, 2, 3000}, {fp4, 4, kBig}}) {
      const std::string p = path_of(b, fp);
      CHECK(!p.empty() && read_bytes(p) == audio_bytes(seed, size));
      CHECK(SyncEngine::fingerprint_file(p) == fp);
    }
    CHECK(path_of(b, fp3).empty());  // not shared
    CHECK(b.count("SELECT count(*) FROM tracks") == 4);  // linked, not duplicated
    CHECK(b.count("SELECT count(*) FROM sync_track WHERE missing = 1") == 1);
    const auto rb2 = b.eng->sync_audio();
    CHECK(rb2.ok && rb2.imported == 0 && rb2.relinked == 0 && rb2.waiting_files == 0 && count_files(b.dir + "/music_in") == 3);
  }
  {  // Never overwrite a different file; relink a file that is already there under another name.
    Scenario s("names");
    Dev& a = device(s, "A");
    Dev& c = device(s, "C");
    Dev& d = device(s, "D");
    const auto t1 = add_track(a, 1, "One"), t2 = add_track(a, 2, "Two");
    a.cfg.audio_playlists = {playlist_with(a, {t1, t2})};
    a.restart();
    a.sync();
    CHECK(a.eng->sync_audio().exported == 2);
    write_bytes(c.dir + "/music_in/trk1.mp3", "a different song with the same name");
    write_bytes(d.dir + "/music_in/renamed.flac", audio_bytes(2));
    s.sync_all(2);
    const auto rc = c.eng->sync_audio();
    CHECK(rc.ok && rc.imported == 2);
    CHECK(read_bytes(c.dir + "/music_in/trk1.mp3") == "a different song with the same name");
    CHECK(count_files(c.dir + "/music_in") == 3);
    CHECK(read_bytes(path_of(c, fp1)) == audio_bytes(1) && path_of(c, fp1).find("trk1.mp3") == std::string::npos);
    const auto rd = d.eng->sync_audio();
    CHECK(rd.ok && rd.relinked == 1 && rd.imported == 1);
    CHECK(path_of(d, fp2).find("renamed.flac") != std::string::npos);
    CHECK(count_files(d.dir + "/music_in") == 2 && d.count("SELECT count(*) FROM tracks") == 2);
  }
  {  // Budgets, and objects that are damaged or still arriving.
    Scenario s("budget");
    Dev& a = device(s, "A");
    Dev& b = device(s, "B");
    const auto t1 = add_track(a, 1, "One"), t2 = add_track(a, 2, "Two"), t4 = add_track(a, 4, "Four", kBig);
    a.cfg.audio_playlists = {playlist_with(a, {t1, t2, t4})};
    a.cfg.audio_budget_bytes = 10000;
    a.restart();
    a.sync();
    const auto r1 = a.eng->sync_audio();
    CHECK(r1.ok && r1.exported == 2 && r1.waiting_budget == 1 && r1.store_bytes <= 10000);
    CHECK(objects(s.folder).size() == 2);
    a.cfg.audio_budget_bytes = 0;
    a.restart();
    const auto r2 = a.eng->sync_audio();
    CHECK(r2.ok && r2.exported == 1 && r2.already_stored == 2 && r2.waiting_budget == 0 && objects(s.folder).size() == 3);
    s.sync_all(2);

    // Import budget: the big file waits.
    b.cfg.audio_import_budget_bytes = 7000;
    b.restart();
    const auto rb = b.eng->sync_audio();
    CHECK(rb.ok && rb.imported == 2 && rb.waiting_budget == 1 && count_files(b.dir + "/music_in") == 2);

    // A damaged object is refused (a flipped byte), a cut-off one waits; neither leaves a file.
    fs::path big;
    for (const auto& o : objects(s.folder))
      if (fs::file_size(o) > kBig) big = o;
    CHECK(!big.empty());
    const std::string good = read_bytes(big.string());
    std::string flipped = good;
    flipped[flipped.size() / 2] ^= 0x55;
    write_bytes(big.string(), flipped);
    b.cfg.audio_import_budget_bytes = 0;
    b.restart();
    const auto rf = b.eng->sync_audio();
    CHECK(rf.ok && rf.failed == 1 && rf.imported == 0 && count_files(b.dir + "/music_in") == 2);
    write_bytes(big.string(), good.substr(0, good.size() / 3));
    const auto rt = b.eng->sync_audio();
    CHECK(rt.ok && rt.waiting_files == 1 && rt.failed == 0 && count_files(b.dir + "/music_in") == 2);
    write_bytes(big.string(), good);  // the rest arrived
    const auto rg = b.eng->sync_audio();
    CHECK(rg.ok && rg.imported == 1 && read_bytes(path_of(b, fp4)) == audio_bytes(4, kBig));
  }
  {  // Encryption: the audio in the folder is unreadable without the passphrase; a missing crypto library refuses.
    Scenario s("enc");
    Dev& a = s.add("A", false);
    Dev& b = s.add("B", false);
    for (Dev* d : {&a, &b}) {
      d->cfg.trust_path_hints = false;
      d->cfg.passphrase = "correct horse";
      fs::create_directories(d->dir + "/music_in");
      d->cfg.music_folder = d->dir + "/music_in";
    }
    a.start();
    b.start();
    if (!SyncEngine::encryption_available()) {
      const auto r = a.eng->sync_audio();
      CHECK(!r.ok);  // a passphrase this build cannot use
      std::puts("encryption unavailable in this build: only the refusal was checked");
    } else {
      const auto t1 = add_track(a, 1, "One"), t4 = add_track(a, 4, "Four", kBig);
      a.cfg.audio_playlists = {playlist_with(a, {t1, t4})};
      a.restart();
      CHECK(!a.eng->sync_audio().ok);  // the folder's key does not exist before the first sync
      a.sync();
      const auto ra = a.eng->sync_audio();
      CHECK(ra.ok && ra.exported == 2);
      for (const auto& o : objects(s.folder)) {
        const std::string bytes = read_bytes(o.string());
        CHECK(!contains(bytes, "seed:1;") && !contains(bytes, "seed:4;"));
      }
      s.sync_all(2);
      const auto rb = b.eng->sync_audio();
      CHECK(rb.ok && rb.imported == 2 && read_bytes(path_of(b, fp4)) == audio_bytes(4, kBig));
      // With an explicit allowance the audio is stored as it is.
      Dev& c = s.add("C", false);
      c.cfg.trust_path_hints = false;
      c.cfg.passphrase = "correct horse";
      c.cfg.allow_unencrypted_audio = true;
      c.start();
      const auto t2 = add_track(c, 2, "Two");
      c.cfg.audio_playlists = {playlist_with(c, {t2})};
      c.restart();
      c.sync();
      CHECK(c.eng->sync_audio().exported == 1);
      bool plain = false;
      for (const auto& o : objects(s.folder)) plain = plain || contains(read_bytes(o.string()), "seed:2;");
      CHECK(plain);
      // A device that requires encryption does not take the plain object.
      s.sync_all(2);
      const auto rb2 = b.eng->sync_audio();
      CHECK(rb2.ok && rb2.failed == 1 && path_of(b, fp2).empty());
    }
  }
  std::puts("test_sync_audio: all checks passed");
  return 0;
}
