#include "png.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace deckhand::shell {
namespace {

std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0) {
  static std::uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  v.push_back(static_cast<std::uint8_t>(x >> 24));
  v.push_back(static_cast<std::uint8_t>(x >> 16));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
  v.push_back(static_cast<std::uint8_t>(x));
}

bool chunk(std::FILE* f, const char* type, const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> head;
  put32(head, static_cast<std::uint32_t>(data.size()));
  std::vector<std::uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  std::vector<std::uint8_t> tail;
  put32(tail, crc32(body.data(), body.size()));
  return std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
         std::fwrite(body.data(), 1, body.size(), f) == body.size() &&
         std::fwrite(tail.data(), 1, tail.size(), f) == tail.size();
}

}  // namespace

bool write_png(const wchar_t* path, int w, int h, const std::uint8_t* rgba) {
  if (w <= 0 || h <= 0) return false;
  std::FILE* f = _wfopen(path, L"wb");
  if (!f) return false;
  static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  bool ok = std::fwrite(sig, 1, 8, f) == 8;
  std::vector<std::uint8_t> ihdr;
  put32(ihdr, static_cast<std::uint32_t>(w));
  put32(ihdr, static_cast<std::uint32_t>(h));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
  ok = ok && chunk(f, "IHDR", ihdr);

  std::vector<std::uint8_t> raw;  // scanlines: filter byte 0 + RGB
  raw.reserve(static_cast<std::size_t>(h) * (static_cast<std::size_t>(w) * 3 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      const std::uint8_t* p = rgba + (static_cast<std::size_t>(y) * w + x) * 4;
      raw.insert(raw.end(), {p[0], p[1], p[2]});
    }
  }
  std::vector<std::uint8_t> z = {0x78, 0x01};  // zlib header, then stored blocks
  for (std::size_t at = 0;;) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
    const bool last = at + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<std::uint8_t>(n & 0xFF));
    z.push_back(static_cast<std::uint8_t>(n >> 8));
    z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
    z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
    at += n;
    if (last) break;
  }
  std::uint32_t a = 1, b = 0;
  for (std::uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  put32(z, (b << 16) | a);
  ok = ok && chunk(f, "IDAT", z) && chunk(f, "IEND", {});
  return std::fclose(f) == 0 && ok;
}

}  // namespace deckhand::shell
