#include "stemset.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace deckhand {
namespace {
std::uint32_t get32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint64_t get64(const std::uint8_t* p) { return std::uint64_t(get32(p)) | (std::uint64_t(get32(p + 4)) << 32); }
constexpr std::size_t kHeader = 128, kTrailer = 16;
constexpr std::uint64_t kMaxFrames = 1ull << 31;
}  // namespace

void StemSet::assign_from_float(const float* const l[kStems], const float* const r[kStems], std::int64_t n, int sample_rate) {
  rate = sample_rate;
  frames = n;
  bits = 16;
  lsb = 1.0f / 32768.0f;
  s32.clear();
  s16.assign(static_cast<std::size_t>(n) * 8, 0);
  for (std::int64_t k = 0; k < n; ++k)
    for (int s = 0; s < kStems; ++s) {
      s16[static_cast<std::size_t>(k) * 8 + static_cast<std::size_t>(s) * 2] = static_cast<std::int16_t>(std::lround(std::clamp(l[s][k], -1.0f, 32767.0f / 32768.0f) * 32768.0f));
      s16[static_cast<std::size_t>(k) * 8 + static_cast<std::size_t>(s) * 2 + 1] = static_cast<std::int16_t>(std::lround(std::clamp(r[s][k], -1.0f, 32767.0f / 32768.0f) * 32768.0f));
    }
}

bool stemset_load_file(const std::string& path, StemSet& out, std::string& error) {
  namespace fs = std::filesystem;
  std::FILE* f =
#if defined(_WIN32)
      _wfopen(fs::u8path(path).c_str(), L"rb");
#else
      std::fopen(path.c_str(), "rb");
#endif
  if (!f) {
    error = "cannot open the stem file";
    return false;
  }
  struct Closer { std::FILE* f; ~Closer() { std::fclose(f); } } closer{f};
  std::uint8_t h[kHeader];
  if (std::fread(h, 1, kHeader, f) != kHeader || std::memcmp(h, "DHSTEM01", 8) != 0 || get32(h + 8) != 1) {
    error = "not a stem file";
    return false;
  }
  const int bits = static_cast<int>(get32(h + 12)), rate = static_cast<int>(get32(h + 16));
  float scale;
  const std::uint32_t sc = get32(h + 28);
  std::memcpy(&scale, &sc, 4);
  const std::uint64_t frames = get64(h + 32);
  if ((bits != 16 && bits != 24) || rate < 8000 || rate > 384000 || get32(h + 20) != kStems || get32(h + 24) != 2 ||
      !std::isfinite(scale) || !(scale > 0) || scale > 1000.0f || frames == 0 || frames >= kMaxFrames) {
    error = "bad stem file header";
    return false;
  }
  const std::size_t bps = static_cast<std::size_t>(bits / 8);
  const std::uint64_t expect = kHeader + std::uint64_t(kStems) * 2 * frames * bps + kTrailer;
  std::error_code ec;
  if (fs::file_size(fs::u8path(path), ec) != expect || ec) {
    error = "stem file has the wrong size";
    return false;
  }
  StemSet s;
  s.rate = rate;
  s.frames = static_cast<std::int64_t>(frames);
  s.bits = bits;
  if (bits == 16) {
    s.lsb = scale / 32768.0f;
    s.s16.assign(static_cast<std::size_t>(frames) * 8, 0);
  } else {
    s.lsb = scale / 8388608.0f;
    s.s32.assign(static_cast<std::size_t>(frames) * 8, 0);
  }
  constexpr std::size_t kChunk = 1 << 16;
  std::vector<std::uint8_t> raw(kChunk * bps);
  for (int stem = 0; stem < kStems; ++stem)
    for (int ch = 0; ch < 2; ++ch) {
      const std::uint64_t off = kHeader + (std::uint64_t(stem) * 2 + std::uint64_t(ch)) * frames * bps;
#if defined(_WIN32)
      if (_fseeki64(f, static_cast<long long>(off), SEEK_SET) != 0) {
#else
      if (fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0) {
#endif
        error = "seek failed";
        return false;
      }
      for (std::uint64_t at = 0; at < frames; at += kChunk) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, frames - at));
        if (std::fread(raw.data(), 1, n * bps, f) != n * bps) {
          error = "read failed";
          return false;
        }
        const std::size_t base = static_cast<std::size_t>(at) * 8 + static_cast<std::size_t>(stem) * 2 + static_cast<std::size_t>(ch);
        if (bits == 16) {
          for (std::size_t i = 0; i < n; ++i)
            s.s16[base + i * 8] = static_cast<std::int16_t>(static_cast<std::uint16_t>(raw[i * 2] | (raw[i * 2 + 1] << 8)));
        } else {
          for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t* p = &raw[i * 3];
            s.s32[base + i * 8] = static_cast<std::int32_t>((std::uint32_t(p[0]) << 8) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 24)) >> 8;
          }
        }
      }
    }
  std::uint8_t tr[kTrailer];
  if (std::fread(tr, 1, kTrailer, f) != kTrailer || std::memcmp(tr, "DHEND\0\0\1", 8) != 0) {
    error = "stem file is incomplete";
    return false;
  }
  out = std::move(s);
  return true;
}

}  // namespace deckhand
