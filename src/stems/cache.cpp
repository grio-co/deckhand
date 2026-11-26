#include "deckhand/stems/cache.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#define DH_FSEEK _fseeki64
#define DH_GETPID _getpid
#else
#include <unistd.h>
#define DH_FSEEK fseeko
#define DH_GETPID getpid
#endif

#include "internal.hpp"

namespace deckhand::stems {
namespace fs = std::filesystem;

// ---- key -------------------------------------------------------------------------------------------------------
struct KeyHasher::Impl {
  detail::Sha256 h;
  std::uint64_t frames = 0;
};
KeyHasher::KeyHasher(const std::string& model_id) : p_(new Impl) {
  const char tag[] = "deckhand-stems-key/1";
  p_->h.update(tag, sizeof tag);
  std::uint64_t n = model_id.size();
  p_->h.update(&n, sizeof n);
  p_->h.update(model_id.data(), model_id.size());
}
KeyHasher::~KeyHasher() = default;
void KeyHasher::add(const float* l, const float* r, std::size_t frames) {
  std::vector<std::int16_t> q(2048 * 2);
  for (std::size_t i = 0; i < frames;) {
    std::size_t k = std::min<std::size_t>(2048, frames - i);
    for (std::size_t j = 0; j < k; ++j) {
      for (int c = 0; c < 2; ++c) {
        float x = (c == 0 ? l : r)[i + j];
        long v = std::lround(std::max(-1.0f, std::min(1.0f, x)) * 32767.0f);
        std::int16_t s = std::int16_t(v);
        // fixed little-endian byte order so keys match across platforms
        q[2 * j + std::size_t(c)] = s;
      }
    }
    std::vector<std::uint8_t> bytes(k * 4);
    for (std::size_t j = 0; j < k * 2; ++j) {
      std::uint16_t u = std::uint16_t(q[j]);
      bytes[2 * j] = std::uint8_t(u);
      bytes[2 * j + 1] = std::uint8_t(u >> 8);
    }
    p_->h.update(bytes.data(), bytes.size());
    i += k;
  }
  p_->frames += frames;
}
std::string KeyHasher::finish(int sample_rate) {
  std::uint64_t meta[2] = {std::uint64_t(sample_rate), p_->frames};
  p_->h.update(meta, sizeof meta);
  return p_->h.finish_hex();
}
std::string content_key(const Audio& a, const std::string& model_id) {
  KeyHasher h(model_id);
  h.add(a.l.data(), a.r.data(), a.frames());
  return h.finish(a.sample_rate);
}

// ---- file format -----------------------------------------------------------------------------------------------
namespace {
constexpr std::size_t kHeader = 128, kTrailer = 16;
const char kMagic[8] = {'D', 'H', 'S', 'T', 'E', 'M', '0', '1'};
const char kEnd[8] = {'D', 'H', 'E', 'N', 'D', 0, 0, 1};

void put32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = std::uint8_t(v >> (8 * i)); }
void put64(std::uint8_t* p, std::uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = std::uint8_t(v >> (8 * i)); }
std::uint32_t get32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint64_t get64(const std::uint8_t* p) { return std::uint64_t(get32(p)) | (std::uint64_t(get32(p + 4)) << 32); }

bool key_ok(const std::string& k) {
  if (k.size() != 64) return false;
  for (char c : k) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
std::int64_t ticks(const fs::file_time_type& t) { return std::int64_t(t.time_since_epoch().count()); }

struct Header {
  int bits = 16, rate = 0;
  float scale = 1;
  std::uint64_t frames = 0;
  std::string key;
};
bool parse_header(const std::uint8_t* h, Header& o) {
  if (std::memcmp(h, kMagic, 8)) return false;
  if (get32(h + 8) != 1) return false;
  o.bits = int(get32(h + 12));
  o.rate = int(get32(h + 16));
  if (get32(h + 20) != kStemCount || get32(h + 24) != 2) return false;
  std::uint32_t sc = get32(h + 28);
  std::memcpy(&o.scale, &sc, 4);
  o.frames = get64(h + 32);
  o.key.assign(reinterpret_cast<const char*>(h + 40), 64);
  return (o.bits == 16 || o.bits == 24) && o.rate > 0 && std::isfinite(o.scale) && o.scale > 0;
}
}  // namespace

std::uint64_t StemCache::expected_size(std::uint64_t frames, int bits) {
  return kHeader + std::uint64_t(kStemCount) * 2 * frames * std::uint64_t(bits / 8) + kTrailer;
}

// ---- reader ----------------------------------------------------------------------------------------------------
StemReader::~StemReader() { if (f_) std::fclose(f_); }

bool StemReader::read(int stem, int channel, std::uint64_t start, std::size_t n, float* out) {
  if (stem < 0 || stem >= kStemCount || channel < 0 || channel > 1) return false;
  std::fill(out, out + n, 0.f);
  if (start >= frames_ || n == 0) return true;
  const std::size_t bps = std::size_t(bits_ / 8);
  const std::size_t take = std::size_t(std::min<std::uint64_t>(n, frames_ - start));
  std::vector<std::uint8_t> raw(take * bps);
  {
    std::lock_guard<std::mutex> g(mu_);
    std::uint64_t off = kHeader + (std::uint64_t(stem) * 2 + std::uint64_t(channel)) * frames_ * bps + start * bps;
    if (DH_FSEEK(f_, static_cast<long long>(off), SEEK_SET) != 0) return false;
    if (std::fread(raw.data(), 1, raw.size(), f_) != raw.size()) return false;
  }
  for (std::size_t i = 0; i < take; ++i) {
    const std::uint8_t* p = &raw[i * bps];
    float v;
    if (bps == 2) v = std::int16_t(std::uint16_t(p[0] | (p[1] << 8))) / 32768.f;
    else v = (std::int32_t((std::uint32_t(p[0]) << 8) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 24)) >> 8) / 8388608.f;
    out[i] = v * scale_;
  }
  return true;
}

bool StemReader::load_all(Stems& out) {
  for (int s = 0; s < kStemCount; ++s) {
    Audio& a = out[std::size_t(s)];
    a.sample_rate = rate_;
    a.l.assign(std::size_t(frames_), 0.f);
    a.r.assign(std::size_t(frames_), 0.f);
    if (!read(s, 0, 0, a.l.size(), a.l.data()) || !read(s, 1, 0, a.r.size(), a.r.data())) return false;
  }
  return true;
}

// ---- cache -----------------------------------------------------------------------------------------------------
StemCache::StemCache(CacheConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.bits != 24) cfg_.bits = 16;
  std::error_code ec;
  fs::create_directories(cfg_.root, ec);
  rescan();
}

fs::path StemCache::path_for(const std::string& key) const { return cfg_.root / key.substr(0, 2) / (key + ".dhstems"); }

bool StemCache::valid_file(const fs::path& p, std::uint64_t* bytes) const {
  std::FILE* f = std::fopen(p.string().c_str(), "rb");
  if (!f) return false;
  std::uint8_t h[kHeader];
  bool ok = std::fread(h, 1, kHeader, f) == kHeader;
  Header hd;
  ok = ok && parse_header(h, hd);
  std::uint64_t want = ok ? expected_size(hd.frames, hd.bits) : 0;
  if (ok) {
    ok = DH_FSEEK(f, static_cast<long long>(want - kTrailer), SEEK_SET) == 0;
    std::uint8_t t[kTrailer];
    ok = ok && std::fread(t, 1, kTrailer, f) == kTrailer && !std::memcmp(t, kEnd, 8);
    // nothing may follow the trailer (a truncated or over-long file is rejected)
    ok = ok && std::fgetc(f) == EOF;
    // the key in the name must match the key in the header
    ok = ok && p.stem().string() == hd.key;
  }
  std::fclose(f);
  if (ok && bytes) *bytes = want;
  return ok;
}

void StemCache::rescan(int tmp_age_seconds) {
  std::lock_guard<std::mutex> g(mu_);
  idx_.clear();
  std::error_code ec;
  auto now = fs::file_time_type::clock::now();
  for (auto& d : fs::directory_iterator(cfg_.root, ec)) {
    if (!d.is_directory(ec)) continue;
    for (auto& e : fs::directory_iterator(d.path(), ec)) {
      const fs::path p = e.path();
      std::error_code e2;
      if (p.extension() == ".tmp") {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - fs::last_write_time(p, e2)).count();
        if (!e2 && age >= tmp_age_seconds) fs::remove(p, e2);
        continue;
      }
      if (p.extension() != ".dhstems" || !key_ok(p.stem().string())) continue;
      std::uint64_t bytes = 0;
      if (!valid_file(p, &bytes)) { fs::remove(p, e2); continue; }  // truncated/corrupt: never served
      auto mt = fs::last_write_time(p, e2);
      idx_[p.stem().string()] = Meta{bytes, e2 ? 0 : ticks(mt)};
    }
  }
  for (auto& kv : idx_) clock_ = std::max(clock_, kv.second.last_used);
}

bool StemCache::has_stems(const std::string& key) const {
  if (!key_ok(key)) return false;
  std::lock_guard<std::mutex> g(mu_);
  if (!idx_.count(key)) return false;
  std::error_code ec;
  return fs::exists(path_for(key), ec);
}

std::unique_ptr<StemReader> StemCache::open(const std::string& key, std::string* error) {
  auto fail = [&](const char* m) -> std::unique_ptr<StemReader> { if (error) *error = m; return nullptr; };
  if (!key_ok(key)) return fail("bad key");
  fs::path p = path_for(key);
  std::uint64_t bytes = 0;
  if (!valid_file(p, &bytes)) {
    std::lock_guard<std::mutex> g(mu_);
    idx_.erase(key);
    return fail("not cached");
  }
  std::unique_ptr<StemReader> r(new StemReader);
  r->f_ = std::fopen(p.string().c_str(), "rb");
  if (!r->f_) return fail("cannot open");
  std::uint8_t h[kHeader];
  Header hd;
  if (std::fread(h, 1, kHeader, r->f_) != kHeader || !parse_header(h, hd)) return fail("bad header");
  r->rate_ = hd.rate; r->bits_ = hd.bits; r->scale_ = hd.scale; r->frames_ = hd.frames;
  // LRU touch
  std::error_code ec;
  fs::file_time_type now = fs::file_time_type::clock::now();
  fs::last_write_time(p, now, ec);
  std::lock_guard<std::mutex> g(mu_);
  std::int64_t t = ec ? clock_ + 1 : ticks(now);
  clock_ = std::max(clock_ + 1, t);
  idx_[key] = Meta{bytes, clock_};
  return r;
}

bool StemCache::load_stems(const std::string& key, Stems& out, std::string* error) {
  auto r = open(key, error);
  if (!r) return false;
  if (!r->load_all(out)) { if (error) *error = "read error"; return false; }
  return true;
}

bool StemCache::store(const std::string& key, const Stems& s, std::string* error) {
  auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
  if (!key_ok(key)) return fail("bad key");
  const std::size_t n = s[0].frames();
  for (auto& a : s)
    if (a.frames() != n || a.r.size() != n || a.sample_rate != s[0].sample_rate) return fail("stems differ in length or rate");
  if (s[0].sample_rate <= 0) return fail("bad sample rate");
  const int bits = cfg_.bits;
  const std::size_t bps = std::size_t(bits / 8);
  float peak = 0;
  for (auto& a : s) for (auto* v : {&a.l, &a.r}) for (float x : *v) peak = std::max(peak, std::fabs(x));
  const float scale = std::max(1.0f, peak);  // never clip: louder-than-full-scale stems are stored scaled

  fs::path dest = path_for(key);
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  static std::atomic<unsigned> counter{0};
  fs::path tmp = dest.parent_path() / (key + "." + std::to_string(DH_GETPID()) + "." + std::to_string(counter++) + ".tmp");
  std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
  if (!f) return fail("cannot create " + tmp.string());
  bool ok = true;
  std::uint8_t h[kHeader] = {0};
  std::memcpy(h, kMagic, 8);
  put32(h + 8, 1); put32(h + 12, std::uint32_t(bits)); put32(h + 16, std::uint32_t(s[0].sample_rate));
  put32(h + 20, kStemCount); put32(h + 24, 2);
  std::uint32_t sc; std::memcpy(&sc, &scale, 4); put32(h + 28, sc);
  put64(h + 32, n);
  std::memcpy(h + 40, key.data(), 64);
  ok = std::fwrite(h, 1, kHeader, f) == kHeader;
  std::uint32_t crc = 0;
  std::vector<std::uint8_t> buf;
  for (int st = 0; st < kStemCount && ok; ++st)
    for (int c = 0; c < 2 && ok; ++c) {
      const std::vector<float>& v = c == 0 ? s[std::size_t(st)].l : s[std::size_t(st)].r;
      for (std::size_t i = 0; i < n && ok;) {
        std::size_t k = std::min<std::size_t>(1 << 16, n - i);
        buf.resize(k * bps);
        for (std::size_t j = 0; j < k; ++j) {
          double x = double(v[i + j]) / scale;
          if (bps == 2) {
            long q = std::lround(std::max(-1.0, std::min(1.0, x)) * 32767.0);
            buf[2 * j] = std::uint8_t(q); buf[2 * j + 1] = std::uint8_t(q >> 8);
          } else {
            long q = std::lround(std::max(-1.0, std::min(1.0, x)) * 8388607.0);
            buf[3 * j] = std::uint8_t(q); buf[3 * j + 1] = std::uint8_t(q >> 8); buf[3 * j + 2] = std::uint8_t(q >> 16);
          }
        }
        crc = detail::crc32_update(crc, buf.data(), buf.size());
        ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
        i += k;
      }
    }
  if (ok) {
    std::uint8_t t[kTrailer] = {0};
    std::memcpy(t, kEnd, 8);
    put32(t + 8, crc);
    ok = std::fwrite(t, 1, kTrailer, f) == kTrailer;
  }
  ok = ok && std::fflush(f) == 0;
#if defined(_WIN32)
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && fsync(fileno(f)) == 0;
#endif
  ok = (std::fclose(f) == 0) && ok;
  if (!ok) { fs::remove(tmp, ec); return fail("write failed (disk full?)"); }
  fs::rename(tmp, dest, ec);
  if (ec) { fs::remove(tmp, ec); return fail("rename failed"); }
  {
    std::lock_guard<std::mutex> g(mu_);
    clock_ = std::max(clock_ + 1, ticks(fs::file_time_type::clock::now()));
    idx_[key] = Meta{expected_size(n, bits), clock_};
  }
  evict_to(cfg_.cap_bytes, key);
  return true;
}

bool StemCache::verify(const std::string& key) {
  if (!key_ok(key)) return false;
  fs::path p = path_for(key);
  std::uint64_t bytes = 0;
  if (!valid_file(p, &bytes)) return false;
  std::FILE* f = std::fopen(p.string().c_str(), "rb");
  if (!f) return false;
  std::uint32_t crc = 0;
  std::vector<std::uint8_t> buf(1 << 16);
  bool ok = DH_FSEEK(f, kHeader, SEEK_SET) == 0;
  std::uint64_t left = bytes - kHeader - kTrailer;
  while (ok && left) {
    std::size_t k = std::size_t(std::min<std::uint64_t>(left, buf.size()));
    ok = std::fread(buf.data(), 1, k, f) == k;
    crc = detail::crc32_update(crc, buf.data(), k);
    left -= k;
  }
  std::uint8_t t[kTrailer];
  ok = ok && std::fread(t, 1, kTrailer, f) == kTrailer && get32(t + 8) == crc;
  std::fclose(f);
  return ok;
}

bool StemCache::evict(const std::string& key) {
  if (!key_ok(key)) return false;
  std::error_code ec;
  bool removed = fs::remove(path_for(key), ec);
  std::lock_guard<std::mutex> g(mu_);
  idx_.erase(key);
  return removed;
}

std::size_t StemCache::evict_to(std::uint64_t cap, const std::string& keep) {
  if (cap == 0) return 0;
  std::vector<std::pair<std::int64_t, std::string>> order;
  std::uint64_t total = 0;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& kv : idx_) { total += kv.second.bytes; order.emplace_back(kv.second.last_used, kv.first); }
  }
  std::sort(order.begin(), order.end());
  std::size_t removed = 0;
  for (auto& o : order) {
    if (total <= cap) break;
    if (o.second == keep) continue;
    std::uint64_t b = 0;
    { std::lock_guard<std::mutex> g(mu_); auto it = idx_.find(o.second); if (it != idx_.end()) b = it->second.bytes; }
    if (evict(o.second)) { total -= std::min(total, b); ++removed; }
  }
  return removed;
}

void StemCache::set_cap_bytes(std::uint64_t cap) {
  { std::lock_guard<std::mutex> g(mu_); cfg_.cap_bytes = cap; }
  evict_to(cap);
}
std::uint64_t StemCache::total_bytes() const {
  std::lock_guard<std::mutex> g(mu_);
  std::uint64_t t = 0;
  for (auto& kv : idx_) t += kv.second.bytes;
  return t;
}
std::size_t StemCache::count() const { std::lock_guard<std::mutex> g(mu_); return idx_.size(); }
std::vector<CacheEntry> StemCache::entries() const {
  std::vector<CacheEntry> v;
  { std::lock_guard<std::mutex> g(mu_); for (auto& kv : idx_) v.push_back({kv.first, kv.second.bytes, kv.second.last_used}); }
  std::sort(v.begin(), v.end(), [](const CacheEntry& a, const CacheEntry& b) { return a.last_used < b.last_used; });
  return v;
}

}  // namespace deckhand::stems
