#pragma once
// Shared by tests/test_sync_*.cpp: devices are separate Library files sharing one folder (like test_sync.cpp).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/sync/sync.hpp"

namespace syncdev {
namespace fs = std::filesystem;
using namespace deckhand;
using namespace deckhand::sync;

inline std::int64_t g_now = 1'700'000'000'000ll;
inline void adv(std::int64_t ms = 1000) { g_now += ms; }

struct Dev {
  std::string name, dir, music, folder;
  SyncConfig cfg;  // adjust before start() / restart()
  std::unique_ptr<Library> lib;
  std::unique_ptr<SyncEngine> eng;
  std::int64_t skew = 0;

  void start() {
    lib = std::make_unique<Library>(dir + "/library.db");
    SyncConfig c = cfg;
    c.folder = folder;
    c.device_name = name;
    c.clock = [this] { return g_now + skew; };
    eng = std::make_unique<SyncEngine>(*lib, c);
  }
  void stop() {
    eng.reset();
    lib.reset();
  }
  void restart() {
    stop();
    start();
  }
  SyncResult sync() {
    adv();
    const auto r = eng->sync_once();
    if (!r.ok) std::fprintf(stderr, "sync on %s failed: %s\n", name.c_str(), r.error.c_str());
    CHECK(r.ok);
    return r;
  }
  int count(const char* sql) {
    Stmt s(lib->db(), sql);
    return s.step() ? static_cast<int>(s.i64(0)) : -1;
  }
};

struct Scenario {
  std::string root, folder;
  std::vector<std::unique_ptr<Dev>> devs;
  explicit Scenario(const std::string& name) {
    const auto id = std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007;
    root = (fs::temp_directory_path() / ("dh_sync_" + name + "_" + std::to_string(id))).string();
    std::error_code ec;
    fs::remove_all(root, ec);
    folder = root + "/cloud";
    fs::create_directories(folder);
  }
  ~Scenario() {
    for (auto& d : devs) d->stop();
    devs.clear();
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  Dev& add(const std::string& name, bool start = true) {
    auto d = std::make_unique<Dev>();
    d->name = name;
    d->dir = root + "/" + name;
    d->music = d->dir + "/music";
    d->folder = folder;
    std::error_code ec;
    fs::create_directories(d->music, ec);
    if (start) d->start();
    devs.push_back(std::move(d));
    return *devs.back();
  }
  void sync_all(int rounds = 3) {
    for (int i = 0; i < rounds; ++i)
      for (auto& d : devs) d->sync();
  }
};

// Deterministic audio-like bytes: `size` bytes derived from the seed (a tag at the front makes seeds distinct).
inline std::string audio_bytes(int seed, std::size_t size = 3000) {
  std::string b(size, '\0');
  for (std::size_t i = 0; i < b.size(); ++i)
    b[i] = static_cast<char>((seed * 131 + static_cast<int>(i) * 7 + (static_cast<int>(i) >> 5) * seed) & 0xFF);
  const std::string tag = "seed:" + std::to_string(seed) + ";";
  b.replace(0, std::min(tag.size(), b.size()), tag.substr(0, b.size()));
  return b;
}
inline void write_bytes(const std::string& path, const std::string& bytes) {
  std::ofstream(fs::path(path), std::ios::binary) << bytes;
}
inline std::string read_bytes(const std::string& path) {
  std::ifstream in(fs::path(path), std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
inline std::string file_for(Dev& d, int seed) { return d.music + "/trk" + std::to_string(seed) + ".mp3"; }

inline std::int64_t add_track(Dev& d, int seed, const std::string& title, std::size_t size = 3000) {
  write_bytes(file_for(d, seed), audio_bytes(seed, size));
  NewTrack t;
  t.path = file_for(d, seed);
  t.title = title;
  t.artist = "Artist";
  t.bpm = 120;
  t.duration_ms = 200000 + seed;
  return d.lib->upsert_track(t);
}
// Fingerprint of the audio made from this seed (computed from a scratch file).
inline std::string fp_of(int seed, std::size_t size = 3000) {
  const fs::path dir = fs::temp_directory_path() / "dh_sync_fp_scratch";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path p = dir / ("s" + std::to_string(seed) + "_" + std::to_string(size));
  write_bytes(p.string(), audio_bytes(seed, size));
  return SyncEngine::fingerprint_file(p.string());
}
// Local row of the track with this fingerprint on this device (0 when none).
inline std::int64_t row_of(Dev& d, const std::string& fp) {
  Stmt s(d.lib->db(), "SELECT s.track_id FROM sync_track s JOIN tracks t ON t.id = s.track_id WHERE s.fp = ?1 ORDER BY s.missing, s.track_id");
  s.bind(1, fp);
  return s.step() ? s.i64(0) : 0;
}

}  // namespace syncdev
