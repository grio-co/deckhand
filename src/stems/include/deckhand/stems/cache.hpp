// On-disk stem cache: content-hash keyed, size-capped with LRU eviction, atomic writes.
//
// Layout:  <root>/<key[0..1]>/<key>.dhstems     (key = 64 hex chars, SHA-256 over the decoded audio + model id)
// File:    128-byte header | stem-major, channel-major planar PCM (16 or 24 bit LE, one float scale in the header)
//          | 16-byte trailer (marker + CRC-32 of the sample data). Files are written to a temp name, flushed and
//          renamed, so a crash leaves either the old state or a stray *.tmp that recover() deletes.
#pragma once
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "deckhand/stems/stems.hpp"

namespace deckhand::stems {

// Incremental key: feed decoded audio (any chunking), then finish(). The audio is quantised to 16 bit before hashing
// so that float rounding differences between decoders do not change the key. Not path based: moved or relinked
// files keep their stems.
class KeyHasher {
 public:
  explicit KeyHasher(const std::string& model_id);
  ~KeyHasher();
  void add(const float* l, const float* r, std::size_t frames);
  std::string finish(int sample_rate);  // 64 hex chars
 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};
std::string content_key(const Audio& a, const std::string& model_id);

std::string sha256_hex(const void* data, std::size_t n);
bool sha256_file(const std::filesystem::path& p, std::string& hex);

struct CacheConfig {
  std::filesystem::path root;                 // e.g. <data dir>/stems
  std::uint64_t cap_bytes = 20ull << 30;      // LRU eviction keeps the total at or below this (0 = unlimited)
  int bits = 16;                              // stored sample depth: 16 or 24
};

struct CacheEntry {
  std::string key;
  std::uint64_t bytes = 0;
  std::int64_t last_used = 0;  // file mtime in ticks; larger = more recent
};

// Random-access reader over one stored track. Streams with seeks (thread-safe); samples come back as float.
class StemReader {
 public:
  ~StemReader();
  int sample_rate() const { return rate_; }
  std::uint64_t frames() const { return frames_; }
  int bits() const { return bits_; }
  // Reads `n` frames of one channel (0 = left, 1 = right) of one stem starting at `start`; frames past the end are
  // zero-filled. Returns false on I/O error.
  bool read(int stem, int channel, std::uint64_t start, std::size_t n, float* out);
  bool load_all(Stems& out);  // whole track into memory
 private:
  friend class StemCache;
  StemReader() = default;
  std::FILE* f_ = nullptr;
  std::mutex mu_;
  int rate_ = 0, bits_ = 16;
  float scale_ = 1;
  std::uint64_t frames_ = 0;
};

class StemCache {
 public:
  explicit StemCache(CacheConfig cfg);
  const CacheConfig& config() const { return cfg_; }

  bool has_stems(const std::string& key) const;  // present and structurally valid (header + size + trailer marker)
  std::unique_ptr<StemReader> open(const std::string& key, std::string* error = nullptr);  // marks as recently used
  bool load_stems(const std::string& key, Stems& out, std::string* error = nullptr);
  bool store(const std::string& key, const Stems& s, std::string* error = nullptr);  // atomic; then evicts to the cap
  bool verify(const std::string& key);  // full CRC check of the sample data

  bool evict(const std::string& key);
  // Removes least-recently-used entries until the total is <= cap_bytes; `keep` is never removed. Returns the number removed.
  std::size_t evict_to(std::uint64_t cap_bytes, const std::string& keep = {});
  void set_cap_bytes(std::uint64_t cap);
  std::uint64_t total_bytes() const;
  std::size_t count() const;
  std::vector<CacheEntry> entries() const;  // least recently used first

  // Rescans the folder (other processes may have written) and deletes stray temp files older than `tmp_age_seconds`.
  void rescan(int tmp_age_seconds = 3600);

  std::filesystem::path path_for(const std::string& key) const;
  static std::uint64_t expected_size(std::uint64_t frames, int bits);

 private:
  struct Meta {
    std::uint64_t bytes = 0;
    std::int64_t last_used = 0;
  };
  bool valid_file(const std::filesystem::path& p, std::uint64_t* bytes) const;
  CacheConfig cfg_;
  mutable std::mutex mu_;
  std::map<std::string, Meta> idx_;
  std::int64_t clock_ = 0;
};

}  // namespace deckhand::stems
