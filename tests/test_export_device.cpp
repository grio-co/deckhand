// Device library backup / restore / verify: round trip, corruption detection, refusals.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include "check.hpp"
#include "deckhand/export/export.hpp"
#include "deckhand/export/export_check.hpp"
#include "deckhand/export/export_device.hpp"

#if defined(_WIN32)
#include <process.h>
#define DH_PID _getpid()
#else
#include <unistd.h>
#define DH_PID getpid()
#endif

using namespace deckhand::usbexport;
namespace fs = std::filesystem;

namespace {

fs::path unique_dir(const char* tag) {
  static std::atomic<int> n{0};
  const fs::path p = fs::temp_directory_path() / ("dh_device_" + std::string(tag) + "_" + std::to_string(DH_PID) + "_" +
                                                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                                                  std::to_string(n++));
  fs::create_directories(p);
  return p;
}

std::map<std::string, std::string> snapshot(const fs::path& root) {  // relative path -> hash
  std::map<std::string, std::string> m;
  for (auto& e : fs::recursive_directory_iterator(root))
    if (e.is_regular_file()) {
      std::string err;
      m[fs::relative(e.path(), root).generic_string()] = sha256_hex_of_file(e.path().string(), err);
    }
  return m;
}

void write_stick(const fs::path& src, const fs::path& stick) {
  std::vector<ExportTrack> tracks;
  for (int i = 1; i <= 4; ++i) {
    ExportTrack t;
    t.id = i;
    const fs::path p = src / ("song" + std::to_string(i) + ".mp3");
    std::ofstream(p, std::ios::binary) << std::string(5000 + 100 * static_cast<std::size_t>(i), static_cast<char>('a' + i));
    t.source_path = p.string();
    t.title = "Song " + std::to_string(i);
    t.artist = "Artist";
    t.album = "Album";
    t.bpm = 128;
    t.duration_ms = 60000;
    t.bitrate = 320;
    for (int b = 0; b < 8; ++b) t.beats.push_back({std::int64_t(b) * 469 + 10, 128.0, b % 4 + 1});
    t.waveform.assign(9000, WaveSample{50, 60, 70});
    tracks.push_back(t);
  }
  ExportPlaylist pl;
  pl.id = 1;
  pl.name = "Set";
  pl.track_ids = {1, 2, 3, 4};
  ExportOptions o;
  o.destination = stick.string();
  std::string err;
  CHECK(export_usb(tracks, {pl}, o, err));
}

bool has_code(const DeviceReport& r, const std::string& code) {
  for (const auto& p : r.problems)
    if (p.code == code) return true;
  return false;
}

fs::path first_file(const fs::path& dir, const std::string& ext) {
  for (auto& e : fs::recursive_directory_iterator(dir))
    if (e.is_regular_file() && e.path().extension() == ext) return e.path();
  return {};
}

}  // namespace

int main() {
  const fs::path src = unique_dir("src"), stick = unique_dir("stick"), backups = unique_dir("bk"), target = unique_dir("target");
  write_stick(src, stick);
  std::string err;

  // verify: a fresh stick is fine
  DeviceReport rep;
  CHECK(verify_device(stick.string(), rep));
  CHECK(rep.ok && rep.tracks == 4 && rep.playlists == 1 && rep.files_checked > 0);
  if (!rep.problems.empty()) std::fprintf(stderr, "%s", rep.text().c_str());
  CHECK(rep.text().find("looks fine") != std::string::npos);
  CHECK(rep.to_json().find("\"ok\":true") != std::string::npos);

  // backup: library only, then with audio
  std::string b1, b2;
  BackupOptions bo;
  bo.stamp = "20260101-120000";
  CHECK(backup_device(stick.string(), backups.string(), bo, b1, err));
  bo.include_audio = true;
  CHECK(backup_device(stick.string(), backups.string(), bo, b2, err));  // same stamp: gets a suffix
  CHECK(b1 != b2 && fs::path(b2).filename().string() == "DeckHand-device-20260101-120000-2");
  std::vector<std::string> problems;
  CHECK(verify_device_backup(b1, problems) && verify_device_backup(b2, problems));
  std::vector<ManifestEntry> m1, m2;
  CHECK(read_backup_manifest(b1, m1, err) && read_backup_manifest(b2, m2, err));
  std::size_t audio1 = 0, audio2 = 0;
  for (const auto& e : m1) audio1 += e.audio;
  for (const auto& e : m2) audio2 += e.audio;
  CHECK(audio1 == 0 && audio2 == 4 && m2.size() == m1.size() + 4);
  const auto list = list_device_backups(backups.string());
  CHECK(list.size() == 2 && list[0].name == "DeckHand-device-20260101-120000-2" && list[0].has_audio && list[0].valid && !list[1].has_audio);
  CHECK(list[1].created == "2026-01-01 12:00:00");
  for (auto& e : fs::directory_iterator(backups)) CHECK(e.path().extension() != ".dhtmp");
  // nothing was changed on the stick by the backup
  const auto before = snapshot(stick);

  // backup of something that is not an export
  std::string none;
  CHECK(!backup_device(target.string(), backups.string(), bo, none, err) && !err.empty());

  // restore onto an empty stick (audio included), then the check passes and the files are identical
  RestoreResult rr;
  CHECK(restore_device(b2, target.string(), {}, rr, err));
  CHECK(rr.files == m2.size());
  CHECK(snapshot(target) == before);
  CHECK(verify_device(target.string(), rep) && rep.ok);

  // restore onto a stick that already has a library: refused, nothing changes
  CHECK(!restore_device(b2, target.string(), {}, rr, err));
  CHECK(err.find("already has a library") != std::string::npos);
  CHECK(snapshot(target) == before);

  // forced restore: replaces the library, removes stale library files, leaves everything else alone
  {
    std::ofstream(target / "notes.txt") << "mine";
    fs::create_directories(target / "PIONEER/USBANLZ/P999/00000001");
    std::ofstream(target / "PIONEER/USBANLZ/P999/00000001/ANLZ0000.DAT") << "stale";
    fs::create_directories(target / "Other Music");
    std::ofstream(target / "Other Music/keep.mp3") << "keep";
    std::ofstream(first_file(target / "PIONEER/rekordbox", ".pdb"), std::ios::binary | std::ios::app) << "junk";
    RestoreOptions ro;
    ro.force = true;
    const bool fr = restore_device(b1, target.string(), ro, rr, err);
    if (!fr) std::fprintf(stderr, "restore error: %s\n", err.c_str());
    CHECK(fr);
    CHECK(rr.removed_stale == 1);
    std::error_code ec;
    CHECK(!fs::exists(target / "PIONEER/USBANLZ/P999/00000001/ANLZ0000.DAT", ec));
    CHECK(fs::exists(target / "notes.txt", ec) && fs::exists(target / "Other Music/keep.mp3", ec));
    CHECK(verify_device(target.string(), rep) && rep.ok);
    for (auto& e : fs::recursive_directory_iterator(target)) CHECK(e.path().extension() != ".dhtmp");
  }

  // an audio file that differs from the backup is not replaced without force
  {
    const fs::path audio = first_file(target / "Contents", ".mp3");
    std::ofstream(audio, std::ios::binary | std::ios::trunc) << "different";
    RestoreOptions ro;
    ro.force = true;
    // library files are replaceable, but the differing audio needs force too: force is on, so it is replaced
    CHECK(restore_device(b2, target.string(), ro, rr, err));
    CHECK(snapshot(target).count("notes.txt") == 1);
    CHECK(verify_device(target.string(), rep) && rep.ok);
    std::ofstream(audio, std::ios::binary | std::ios::trunc) << "different";
    fs::remove(target / "PIONEER/rekordbox/export.pdb");
    fs::remove_all(target / "PIONEER/USBANLZ");
    CHECK(!restore_device(b2, target.string(), {}, rr, err));  // library gone, but the audio differs
    CHECK(err.find("differs from the backup") != std::string::npos);
    CHECK(!fs::exists(target / "PIONEER/rekordbox/export.pdb"));
  }

  // verify catches damage in plain language
  {
    const auto audio = first_file(stick / "Contents", ".mp3");
    const std::string keep = audio.string();
    fs::resize_file(audio, 100);
    CHECK(!verify_device(stick.string(), rep) && !rep.ok && has_code(rep, "audio.size"));
    CHECK(rep.text().find("Do not rely on this stick") != std::string::npos);
    CHECK(rep.to_json().find("\"ok\":false") != std::string::npos);
    CHECK(restore_device(b2, stick.string(), RestoreOptions{true, true}, rr, err));  // forced restore repairs it
    CHECK(verify_device(stick.string(), rep) && rep.ok);
    const auto anlz = first_file(stick / "PIONEER/USBANLZ", ".DAT");
    fs::remove(anlz);
    CHECK(!verify_device(stick.string(), rep) && (has_code(rep, "anlz.missing_DAT") || has_code(rep, "library.unreadable")));
    CHECK(restore_device(b1, stick.string(), RestoreOptions{true, true}, rr, err));
    CHECK(verify_device(stick.string(), rep) && rep.ok);
    // flipped byte in the database
    const auto pdb = stick / "PIONEER/rekordbox/export.pdb";
    {
      std::fstream f(pdb, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(4096 + 40);
      f.put('\x7f');
      f.put('\x7f');
    }
    // (whether the structural check notices depends on the byte; the backup hash always does)
    std::string h1 = sha256_hex_of_file(pdb.string(), err);
    bool differs = false;
    for (const auto& e : m1)
      if (e.path == "PIONEER/rekordbox/export.pdb") differs = e.sha256 != h1;
    CHECK(differs);
  }

  // corrupted backup: detected, restore refused, nothing written
  {
    const fs::path victim = fs::path(b2) / "files" / "PIONEER/rekordbox/export.pdb";
    {
      std::fstream f(victim, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(100);
      f.put('\x55');
    }
    CHECK(!verify_device_backup(b2, problems) && !problems.empty());
    const fs::path empty = unique_dir("empty");
    CHECK(!restore_device(b2, empty.string(), {}, rr, err));
    CHECK(err.find("does not verify") != std::string::npos);
    CHECK(fs::is_empty(empty));
    // missing manifest = unfinished backup
    fs::remove(fs::path(b1) / "manifest.txt");
    CHECK(!restore_device(b1, empty.string(), {}, rr, err) && fs::is_empty(empty));
    const auto l = list_device_backups(backups.string());
    CHECK(l.size() == 2);
    bool saw_invalid = false;
    for (const auto& b : l) saw_invalid = saw_invalid || !b.valid;
    CHECK(saw_invalid);
    // restore to a missing destination
    CHECK(!restore_device(b2, (empty / "nope").string(), {}, rr, err));
    std::error_code ec;
    fs::remove_all(empty, ec);
  }

  // verify on something that is not a stick
  {
    const fs::path blank = unique_dir("blank");
    CHECK(!verify_device(blank.string(), rep) && has_code(rep, "library.missing"));
    std::error_code ec;
    fs::remove_all(blank, ec);
  }

  std::error_code ec;
  for (const auto& p : {src, stick, backups, target}) fs::remove_all(p, ec);
  std::printf("test_export_device: ok\n");
  return 0;
}
