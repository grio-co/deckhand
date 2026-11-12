#include "deckhand/gfx/png.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace deckhand::gfx {
namespace {

// ---- inflate (RFC 1951) ------------------------------------------------------------------------------
struct BitReader {
  const std::uint8_t* p;
  std::size_t n, pos = 0;
  std::uint32_t buf = 0;
  int cnt = 0;
  bool overrun = false;
  int bits(int need) {  // need <= 16
    while (cnt < need) {
      if (pos >= n) {
        overrun = true;
        buf |= 0;  // reads zeros past the end; callers check `overrun`
        cnt += 8;
        continue;
      }
      buf |= static_cast<std::uint32_t>(p[pos++]) << cnt;
      cnt += 8;
    }
    const int v = static_cast<int>(buf & ((1u << need) - 1));
    buf >>= need;
    cnt -= need;
    return v;
  }
  void align() {
    buf = 0;
    cnt = 0;
  }
};

// Canonical Huffman code: counts per length and symbols sorted by code.
struct Huffman {
  std::uint16_t count[16] = {};
  std::uint16_t symbol[320] = {};
  bool build(const std::uint8_t* lengths, int n) {
    std::memset(count, 0, sizeof count);
    for (int i = 0; i < n; ++i) ++count[lengths[i]];
    count[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {  // reject over-subscribed sets (incomplete ones are legal)
      left <<= 1;
      left -= count[len];
      if (left < 0) return false;
    }
    std::uint16_t offs[16] = {};
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<std::uint16_t>(offs[len] + count[len]);
    for (int i = 0; i < n; ++i)
      if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
    return true;
  }
  int decode(BitReader& br) const {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= br.bits(1);
      const int c = count[len];
      if (code - c < first) return symbol[index + (code - first)];
      index += c;
      first += c;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }
};

constexpr std::uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_codes(BitReader& br, const Huffman& lit, const Huffman& dist, std::vector<std::uint8_t>& out, std::size_t limit, std::string& err) {
  for (;;) {
    const int sym = lit.decode(br);
    if (br.overrun) { err = "truncated deflate data"; return false; }
    if (sym < 0) { err = "bad Huffman code"; return false; }
    if (sym < 256) {
      if (out.size() >= limit) { err = "decoded data too large"; return false; }
      out.push_back(static_cast<std::uint8_t>(sym));
    } else if (sym == 256) {
      return true;
    } else {
      const int li = sym - 257;
      if (li >= 29) { err = "bad length symbol"; return false; }
      const int len = kLenBase[li] + br.bits(kLenExtra[li]);
      const int ds = dist.decode(br);
      if (ds < 0 || ds >= 30) { err = "bad distance symbol"; return false; }
      const std::size_t d = static_cast<std::size_t>(kDistBase[ds] + br.bits(kDistExtra[ds]));
      if (br.overrun) { err = "truncated deflate data"; return false; }
      if (d > out.size()) { err = "distance beyond the start of the data"; return false; }
      if (out.size() + static_cast<std::size_t>(len) > limit) { err = "decoded data too large"; return false; }
      for (int i = 0; i < len; ++i) out.push_back(out[out.size() - d]);
    }
  }
}

bool inflate_raw(BitReader& br, std::vector<std::uint8_t>& out, std::size_t limit, std::string& err) {
  for (;;) {
    const int final_block = br.bits(1);
    const int type = br.bits(2);
    if (type == 0) {
      br.align();
      if (br.pos + 4 > br.n) { err = "truncated stored block"; return false; }
      const std::size_t len = br.p[br.pos] | (br.p[br.pos + 1] << 8), nlen = br.p[br.pos + 2] | (br.p[br.pos + 3] << 8);
      br.pos += 4;
      if ((len ^ 0xFFFF) != nlen) { err = "bad stored block"; return false; }
      if (br.pos + len > br.n) { err = "truncated stored block"; return false; }
      if (out.size() + len > limit) { err = "decoded data too large"; return false; }
      out.insert(out.end(), br.p + br.pos, br.p + br.pos + len);
      br.pos += len;
    } else if (type == 1) {
      std::uint8_t l[288];
      for (int i = 0; i < 144; ++i) l[i] = 8;
      for (int i = 144; i < 256; ++i) l[i] = 9;
      for (int i = 256; i < 280; ++i) l[i] = 7;
      for (int i = 280; i < 288; ++i) l[i] = 8;
      Huffman lit, dist;
      lit.build(l, 288);
      std::uint8_t dl[30];
      std::fill(dl, dl + 30, std::uint8_t{5});
      dist.build(dl, 30);
      if (!inflate_codes(br, lit, dist, out, limit, err)) return false;
    } else if (type == 2) {
      const int nlen = br.bits(5) + 257, ndist = br.bits(5) + 1, ncode = br.bits(4) + 4;
      if (nlen > 286 || ndist > 30) { err = "bad dynamic block header"; return false; }
      static const std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      std::uint8_t lengths[320] = {};
      for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<std::uint8_t>(br.bits(3));
      Huffman cl;
      if (!cl.build(lengths, 19)) { err = "bad code-length code"; return false; }
      std::uint8_t all[320] = {};
      int idx = 0;
      while (idx < nlen + ndist) {
        const int sym = cl.decode(br);
        if (br.overrun || sym < 0) { err = "bad code lengths"; return false; }
        if (sym < 16) {
          all[idx++] = static_cast<std::uint8_t>(sym);
        } else {
          int rep, val = 0;
          if (sym == 16) {
            if (idx == 0) { err = "bad code lengths"; return false; }
            val = all[idx - 1];
            rep = 3 + br.bits(2);
          } else if (sym == 17) {
            rep = 3 + br.bits(3);
          } else {
            rep = 11 + br.bits(7);
          }
          if (idx + rep > nlen + ndist) { err = "bad code lengths"; return false; }
          while (rep--) all[idx++] = static_cast<std::uint8_t>(val);
        }
      }
      if (all[256] == 0) { err = "missing end-of-block code"; return false; }
      Huffman lit, dist;
      if (!lit.build(all, nlen) || !dist.build(all + nlen, ndist)) { err = "bad Huffman tables"; return false; }
      if (!inflate_codes(br, lit, dist, out, limit, err)) return false;
    } else {
      err = "bad deflate block type";
      return false;
    }
    if (br.overrun) { err = "truncated deflate data"; return false; }
    if (final_block) return true;
  }
}

std::uint32_t be32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

}  // namespace

bool zlib_inflate(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out, std::size_t limit, std::string& error) {
  out.clear();
  if (size < 6) { error = "zlib stream too short"; return false; }
  if ((data[0] & 0x0F) != 8 || ((data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20)) { error = "unsupported zlib header"; return false; }
  BitReader br{data + 2, size - 2};
  return inflate_raw(br, out, limit, error);  // the Adler-32 trailer is not verified; the PNG chunk CRCs are
}

// ---- PNG -----------------------------------------------------------------------------------------------
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

int paeth(int a, int b, int c) {
  const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
}  // namespace

bool decode_png(const std::uint8_t* data, std::size_t size, Image& out, std::string& error) {
  out = Image{};
  static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (size < 8 || std::memcmp(data, sig, 8) != 0) { error = "not a PNG file"; return false; }
  std::size_t pos = 8;
  std::uint32_t w = 0, h = 0;
  int depth = 0, ctype = -1, interlace = 0;
  std::vector<std::uint8_t> palette, trns, idat;
  bool have_header = false, seen_end = false;
  while (pos + 12 <= size && !seen_end) {
    const std::uint32_t len = be32(data + pos);
    const char* type = reinterpret_cast<const char*>(data + pos + 4);
    if (len > size - pos - 12) { error = "truncated chunk"; return false; }
    const std::uint8_t* body = data + pos + 8;
    if (crc32(data + pos + 4, len + 4) != be32(body + len)) { error = "chunk CRC mismatch"; return false; }
    if (!std::memcmp(type, "IHDR", 4)) {
      if (len != 13) { error = "bad IHDR"; return false; }
      w = be32(body);
      h = be32(body + 4);
      depth = body[8];
      ctype = body[9];
      interlace = body[12];
      if (body[10] != 0 || body[11] != 0) { error = "unsupported compression or filter method"; return false; }
      have_header = true;
    } else if (!std::memcmp(type, "PLTE", 4)) {
      if (len % 3 != 0 || len > 768) { error = "bad PLTE"; return false; }
      palette.assign(body, body + len);
    } else if (!std::memcmp(type, "tRNS", 4)) {
      trns.assign(body, body + len);
    } else if (!std::memcmp(type, "IDAT", 4)) {
      if (idat.size() + len > (64u << 20)) { error = "image data too large"; return false; }
      idat.insert(idat.end(), body, body + len);
    } else if (!std::memcmp(type, "IEND", 4)) {
      seen_end = true;
    }
    pos += 12 + len;
  }
  if (!have_header) { error = "missing IHDR"; return false; }
  if (interlace != 0) { error = "interlaced PNGs are not supported"; return false; }
  if (depth != 8) { error = "only 8-bit PNGs are supported"; return false; }
  int channels = 0;
  switch (ctype) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: error = "unsupported colour type"; return false;
  }
  if (w == 0 || h == 0 || static_cast<std::uint64_t>(w) * h > kMaxPixels) { error = "bad or oversized image dimensions"; return false; }
  if (ctype == 3 && palette.empty()) { error = "missing palette"; return false; }
  const std::size_t stride = static_cast<std::size_t>(w) * static_cast<std::size_t>(channels);
  const std::size_t expect = (stride + 1) * h;
  std::vector<std::uint8_t> raw;
  if (!zlib_inflate(idat.data(), idat.size(), raw, expect, error)) return false;
  if (raw.size() != expect) { error = "image data has the wrong length"; return false; }

  // Undo the scanline filters in place.
  const std::size_t bpp = static_cast<std::size_t>(channels);
  for (std::uint32_t y = 0; y < h; ++y) {
    std::uint8_t* row = raw.data() + y * (stride + 1);
    const std::uint8_t ft = row[0];
    std::uint8_t* cur = row + 1;
    const std::uint8_t* up = y ? row - stride : nullptr;  // previous row's data (its filter byte is just before it)
    if (ft > 4) { error = "bad filter type"; return false; }
    for (std::size_t i = 0; i < stride; ++i) {
      const int a = i >= bpp ? cur[i - bpp] : 0, b = up ? up[i] : 0, c = (up && i >= bpp) ? up[i - bpp] : 0;
      int v = cur[i];
      switch (ft) {
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: v += paeth(a, b, c); break;
        default: break;
      }
      cur[i] = static_cast<std::uint8_t>(v);
    }
  }

  out.width = static_cast<int>(w);
  out.height = static_cast<int>(h);
  out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint8_t* s = raw.data() + y * (stride + 1) + 1;
    std::uint8_t* d = out.rgba.data() + static_cast<std::size_t>(y) * w * 4;
    for (std::uint32_t x = 0; x < w; ++x, d += 4) {
      switch (ctype) {
        case 6: std::memcpy(d, s + x * 4, 4); break;
        case 2:
          d[0] = s[x * 3]; d[1] = s[x * 3 + 1]; d[2] = s[x * 3 + 2]; d[3] = 255;
          if (trns.size() == 6 && d[0] == trns[1] && d[1] == trns[3] && d[2] == trns[5]) d[3] = 0;
          break;
        case 0:
          d[0] = d[1] = d[2] = s[x]; d[3] = 255;
          if (trns.size() == 2 && s[x] == trns[1]) d[3] = 0;
          break;
        case 4: d[0] = d[1] = d[2] = s[x * 2]; d[3] = s[x * 2 + 1]; break;
        case 3: {
          const std::size_t i = s[x];
          if (i * 3 + 2 >= palette.size()) { error = "palette index out of range"; out = Image{}; return false; }
          d[0] = palette[i * 3]; d[1] = palette[i * 3 + 1]; d[2] = palette[i * 3 + 2];
          d[3] = i < trns.size() ? trns[i] : 255;
          break;
        }
      }
    }
  }
  return true;
}

bool load_png_file(const std::string& path, Image& out, std::string& error, std::size_t max_bytes) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) { error = "cannot open " + path; return false; }
  const std::streamoff n = f.tellg();
  if (n <= 0 || static_cast<std::size_t>(n) > max_bytes) { error = "bad file size: " + path; return false; }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(bytes.data()), n);
  if (!f) { error = "cannot read " + path; return false; }
  return decode_png(bytes.data(), bytes.size(), out, error);
}

}  // namespace deckhand::gfx
