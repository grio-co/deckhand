#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "internal.hpp"

namespace deckhand::stems {
namespace {
std::uint32_t rd32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint16_t rd16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
void wr32(std::vector<std::uint8_t>& v, std::uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(std::uint8_t(x >> (8 * i))); }
void wr16(std::vector<std::uint8_t>& v, std::uint16_t x) { v.push_back(std::uint8_t(x)); v.push_back(std::uint8_t(x >> 8)); }
bool fail(std::string* e, const char* m) { if (e) *e = m; return false; }
}  // namespace

bool read_wav(const std::string& path, Audio& out, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return fail(error, "cannot open file");
  std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return fail(error, "not a RIFF/WAVE file");
  std::size_t pos = 12;
  int fmt = 0, ch = 0, bits = 0, rate = 0;
  const std::uint8_t* data = nullptr;
  std::size_t dlen = 0;
  while (pos + 8 <= d.size()) {
    std::size_t len = rd32(&d[pos + 4]);
    const std::uint8_t* body = &d[pos + 8];
    if (!std::memcmp(&d[pos], "fmt ", 4) && len >= 16 && pos + 8 + len <= d.size()) {
      fmt = rd16(body); ch = rd16(body + 2); rate = int(rd32(body + 4)); bits = rd16(body + 14);
      if (fmt == 0xFFFE && len >= 26) fmt = rd16(body + 24);
    } else if (!std::memcmp(&d[pos], "data", 4)) {
      data = body;
      dlen = std::min(len, d.size() - (pos + 8));
      break;
    }
    pos += 8 + len + (len & 1);
  }
  if (!data || ch < 1 || ch > 2 || rate <= 0) return fail(error, "unsupported WAV (need 1-2 channels with a data chunk)");
  const bool isfloat = fmt == 3;
  if (!(fmt == 1 || fmt == 3) || !(bits == 8 || bits == 16 || bits == 24 || bits == 32) || (isfloat && bits != 32))
    return fail(error, "unsupported WAV sample format");
  const std::size_t bps = std::size_t(bits / 8), frame = bps * std::size_t(ch), n = dlen / frame;
  out.sample_rate = rate;
  out.l.assign(n, 0.f);
  out.r.assign(n, 0.f);
  for (std::size_t i = 0; i < n; ++i)
    for (int c = 0; c < ch; ++c) {
      const std::uint8_t* p = data + i * frame + std::size_t(c) * bps;
      float v;
      if (isfloat) { std::uint32_t u = rd32(p); std::memcpy(&v, &u, 4); }
      else if (bits == 8) v = (int(p[0]) - 128) / 128.f;
      else if (bits == 16) v = std::int16_t(rd16(p)) / 32768.f;
      else if (bits == 24) v = (std::int32_t((std::uint32_t(p[0]) << 8) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 24)) >> 8) / 8388608.f;
      else v = std::int32_t(rd32(p)) / 2147483648.f;
      (c == 0 ? out.l : out.r)[i] = v;
    }
  if (ch == 1) out.r = out.l;
  return true;
}

bool write_wav(const std::string& path, const Audio& a, int bits, std::string* error) {
  if (bits != 16 && bits != 24 && bits != 32) return fail(error, "bits must be 16, 24 or 32 (float)");
  const std::size_t n = a.frames(), bps = std::size_t(bits / 8), dlen = n * 2 * bps;
  if (dlen > 0xFFFFFFF0u - 64) return fail(error, "too long for a WAV file");
  std::vector<std::uint8_t> v;
  v.reserve(44 + dlen);
  v.insert(v.end(), {'R', 'I', 'F', 'F'}); wr32(v, std::uint32_t(36 + dlen));
  v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); wr32(v, 16);
  wr16(v, bits == 32 ? 3 : 1); wr16(v, 2); wr32(v, std::uint32_t(a.sample_rate));
  wr32(v, std::uint32_t(a.sample_rate) * 2 * std::uint32_t(bps)); wr16(v, std::uint16_t(2 * bps)); wr16(v, std::uint16_t(bits));
  v.insert(v.end(), {'d', 'a', 't', 'a'}); wr32(v, std::uint32_t(dlen));
  for (std::size_t i = 0; i < n; ++i)
    for (int c = 0; c < 2; ++c) {
      float x = (c == 0 ? a.l : a.r)[i];
      if (bits == 32) { std::uint32_t u; std::memcpy(&u, &x, 4); wr32(v, u); continue; }
      double sc = bits == 16 ? 32767.0 : 8388607.0;
      long q = std::lround(std::max(-1.0, std::min(1.0, double(x))) * sc);
      if (bits == 16) wr16(v, std::uint16_t(std::int16_t(q)));
      else { v.push_back(std::uint8_t(q)); v.push_back(std::uint8_t(q >> 8)); v.push_back(std::uint8_t(q >> 16)); }
    }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return fail(error, "cannot create file");
  f.write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size()));
  return f.good() ? true : fail(error, "write failed");
}

}  // namespace deckhand::stems
