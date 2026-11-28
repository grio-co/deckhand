#pragma once
// Test-only FLAC decoder and MD5, written separately from the encoder (bitwise CRCs, no tables, no shared code) so a round
// trip through it is an independent check of the stream the encoder writes.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace flacref {

struct Info {
  std::uint32_t min_bs = 0, max_bs = 0, min_fs = 0, max_fs = 0, rate = 0, channels = 0, bps = 0;
  std::uint64_t total = 0;
  std::uint8_t md5[16] = {};
};
struct Seek {
  std::uint64_t sample = 0, offset = 0;
  std::uint32_t count = 0;
};
struct Decoded {
  bool ok = false;
  std::string err;
  Info info;
  std::vector<Seek> seek;
  std::vector<std::string> comments;
  std::string vendor;
  std::uint64_t padding = 0, first_frame = 0;
  std::vector<std::int32_t> pcm;                    // interleaved
  std::vector<std::uint64_t> frame_offsets;         // relative to the first frame
  std::vector<std::uint64_t> frame_first_sample;
  int modes[4] = {0, 0, 0, 0};                      // frames per stereo mode: independent, L/S, S/R, M/S
  int kinds[4] = {0, 0, 0, 0};                      // subframes: constant, verbatim, fixed, lpc
  int wasted_subframes = 0;
};

// ---- MD5, compact reference ----
inline void md5(const std::uint8_t* d, std::size_t n, std::uint8_t out[16]) {
  static const std::uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
      0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
      0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
      0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
      0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
      0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  std::vector<std::uint8_t> m(d, d + n);
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  const std::uint64_t bits = static_cast<std::uint64_t>(n) * 8;
  for (int i = 0; i < 8; ++i) m.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
  std::uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
  for (std::size_t o = 0; o < m.size(); o += 64) {
    std::uint32_t w[16];
    for (int i = 0; i < 16; ++i)
      w[i] = m[o + i * 4] | (m[o + i * 4 + 1] << 8) | (m[o + i * 4 + 2] << 16) | (static_cast<std::uint32_t>(m[o + i * 4 + 3]) << 24);
    std::uint32_t a = h0, b = h1, c = h2, dd = h3;
    for (int i = 0; i < 64; ++i) {
      std::uint32_t f;
      int g;
      if (i < 16) { f = (b & c) | (~b & dd); g = i; }
      else if (i < 32) { f = (dd & b) | (~dd & c); g = (5 * i + 1) % 16; }
      else if (i < 48) { f = b ^ c ^ dd; g = (3 * i + 5) % 16; }
      else { f = c ^ (b | ~dd); g = (7 * i) % 16; }
      const std::uint32_t tmp = dd;
      dd = c;
      c = b;
      const std::uint32_t x = a + f + K[i] + w[g];
      b = b + ((x << R[i]) | (x >> (32 - R[i])));
      a = tmp;
    }
    h0 += a; h1 += b; h2 += c; h3 += dd;
  }
  const std::uint32_t hs[4] = {h0, h1, h2, h3};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(hs[i] >> (8 * j));
}

inline std::uint8_t crc8_bitwise(const std::uint8_t* p, std::size_t n) {
  std::uint8_t c = 0;
  for (std::size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c & 0x80) ? static_cast<std::uint8_t>((c << 1) ^ 0x07) : static_cast<std::uint8_t>(c << 1);
  }
  return c;
}
inline std::uint16_t crc16_bitwise(const std::uint8_t* p, std::size_t n) {
  std::uint16_t c = 0;
  for (std::size_t i = 0; i < n; ++i) {
    c = static_cast<std::uint16_t>(c ^ (p[i] << 8));
    for (int k = 0; k < 8; ++k) c = (c & 0x8000) ? static_cast<std::uint16_t>((c << 1) ^ 0x8005) : static_cast<std::uint16_t>(c << 1);
  }
  return c;
}

class Bits {
 public:
  Bits(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
  bool bad() const { return bad_; }
  std::size_t bitpos() const { return pos_; }
  std::size_t bytepos() const { return pos_ / 8; }
  std::uint64_t u(int bits) {
    std::uint64_t v = 0;
    for (int i = 0; i < bits; ++i) {
      if (pos_ / 8 >= n_) { bad_ = true; return 0; }
      v = (v << 1) | ((p_[pos_ / 8] >> (7 - pos_ % 8)) & 1u);
      ++pos_;
    }
    return v;
  }
  std::int64_t s(int bits) {
    if (bits == 0) return 0;
    std::uint64_t v = u(bits);
    if (v & (1ull << (bits - 1))) return static_cast<std::int64_t>(v) - (1ll << bits);
    return static_cast<std::int64_t>(v);
  }
  std::uint64_t unary() {
    std::uint64_t q = 0;
    while (!bad_ && u(1) == 0) ++q;
    return q;
  }
  void align() { pos_ = (pos_ + 7) & ~static_cast<std::size_t>(7); }

 private:
  const std::uint8_t* p_;
  std::size_t n_, pos_ = 0;
  bool bad_ = false;
};

inline bool fail(Decoded& d, const std::string& e) {
  d.ok = false;
  d.err = e;
  return false;
}

inline bool decode_residual(Bits& b, int n, int order, std::vector<std::int64_t>& out, Decoded& d) {
  const int method = static_cast<int>(b.u(2));
  if (method > 1) return fail(d, "bad residual method");
  const int po = static_cast<int>(b.u(4));
  const int parts = 1 << po;
  if (n % parts != 0 || (n >> po) <= order) return fail(d, "bad partition order");
  const int pb = method ? 5 : 4, esc = method ? 31 : 15;
  for (int i = 0; i < parts; ++i) {
    const int cnt = (n >> po) - (i == 0 ? order : 0);
    const int k = static_cast<int>(b.u(pb));
    if (k == esc) {
      const int raw = static_cast<int>(b.u(5));
      for (int j = 0; j < cnt; ++j) out.push_back(b.s(raw));
    } else {
      for (int j = 0; j < cnt; ++j) {
        const std::uint64_t q = b.unary();
        const std::uint64_t low = b.u(k);
        const std::uint64_t z = (q << k) | low;
        out.push_back((z & 1) ? -static_cast<std::int64_t>((z + 1) >> 1) : static_cast<std::int64_t>(z >> 1));
      }
    }
    if (b.bad()) return fail(d, "residual ran off the end");
  }
  return true;
}

inline bool decode_subframe(Bits& b, int n, int bps, std::vector<std::int64_t>& x, Decoded& d) {
  if (b.u(1) != 0) return fail(d, "subframe pad bit");
  const int type = static_cast<int>(b.u(6));
  int wasted = 0;
  if (b.u(1)) wasted = static_cast<int>(b.unary()) + 1;
  if (wasted) ++d.wasted_subframes;
  const int w = bps - wasted;
  if (w < 1) return fail(d, "wasted bits exceed the sample size");
  x.clear();
  if (type == 0) {
    const std::int64_t v = b.s(w);
    x.assign(static_cast<std::size_t>(n), v);
    ++d.kinds[0];
  } else if (type == 1) {
    for (int i = 0; i < n; ++i) x.push_back(b.s(w));
    ++d.kinds[1];
  } else if (type >= 8 && type <= 12) {
    const int order = type - 8;
    if (order > 4) return fail(d, "bad fixed order");
    for (int i = 0; i < order; ++i) x.push_back(b.s(w));
    std::vector<std::int64_t> r;
    if (!decode_residual(b, n, order, r, d)) return false;
    for (int i = order; i < n; ++i) {
      const std::int64_t e = r[static_cast<std::size_t>(i - order)];
      std::int64_t p = 0;
      auto at = [&](int k) { return x[static_cast<std::size_t>(i - k)]; };
      switch (order) {
        case 0: p = 0; break;
        case 1: p = at(1); break;
        case 2: p = 2 * at(1) - at(2); break;
        case 3: p = 3 * at(1) - 3 * at(2) + at(3); break;
        default: p = 4 * at(1) - 6 * at(2) + 4 * at(3) - at(4); break;
      }
      x.push_back(p + e);
    }
    ++d.kinds[2];
  } else if (type >= 32) {
    const int order = type - 32 + 1;
    for (int i = 0; i < order; ++i) x.push_back(b.s(w));
    const int prec = static_cast<int>(b.u(4)) + 1;
    if (prec == 16) return fail(d, "invalid coefficient precision");
    const int shift = static_cast<int>(b.s(5));
    if (shift < 0) return fail(d, "negative LPC shift");
    std::vector<std::int64_t> c;
    for (int i = 0; i < order; ++i) c.push_back(b.s(prec));
    std::vector<std::int64_t> r;
    if (!decode_residual(b, n, order, r, d)) return false;
    for (int i = order; i < n; ++i) {
      std::int64_t sum = 0;
      for (int j = 0; j < order; ++j) sum += c[static_cast<std::size_t>(j)] * x[static_cast<std::size_t>(i - 1 - j)];
      x.push_back((sum >> shift) + r[static_cast<std::size_t>(i - order)]);
    }
    ++d.kinds[3];
  } else {
    return fail(d, "reserved subframe type");
  }
  if (b.bad()) return fail(d, "subframe ran off the end");
  if (wasted) for (auto& v : x) v *= (1ll << wasted);
  return true;
}

inline bool decode(const std::vector<std::uint8_t>& f, Decoded& d) {
  d = Decoded{};
  if (f.size() < 8 || std::memcmp(f.data(), "fLaC", 4) != 0) return fail(d, "no fLaC marker");
  std::size_t pos = 4;
  bool last = false, have_si = false;
  while (!last) {
    if (pos + 4 > f.size()) return fail(d, "metadata truncated");
    last = (f[pos] & 0x80) != 0;
    const int type = f[pos] & 0x7F;
    const std::size_t len = (static_cast<std::size_t>(f[pos + 1]) << 16) | (f[pos + 2] << 8) | f[pos + 3];
    pos += 4;
    if (pos + len > f.size()) return fail(d, "metadata block overruns the file");
    const std::uint8_t* q = f.data() + pos;
    if (type == 0) {
      if (len != 34 || have_si) return fail(d, "bad STREAMINFO");
      have_si = true;
      Bits b(q, len);
      d.info.min_bs = static_cast<std::uint32_t>(b.u(16));
      d.info.max_bs = static_cast<std::uint32_t>(b.u(16));
      d.info.min_fs = static_cast<std::uint32_t>(b.u(24));
      d.info.max_fs = static_cast<std::uint32_t>(b.u(24));
      d.info.rate = static_cast<std::uint32_t>(b.u(20));
      d.info.channels = static_cast<std::uint32_t>(b.u(3)) + 1;
      d.info.bps = static_cast<std::uint32_t>(b.u(5)) + 1;
      d.info.total = b.u(36);
      for (int i = 0; i < 16; ++i) d.info.md5[i] = static_cast<std::uint8_t>(b.u(8));
    } else if (type == 1) {
      d.padding += len;
      for (std::size_t i = 0; i < len; ++i) if (q[i]) return fail(d, "padding not zero");
    } else if (type == 3) {
      if (len % 18) return fail(d, "bad SEEKTABLE size");
      std::uint64_t prev = 0;
      bool placeholder = false;
      for (std::size_t i = 0; i < len; i += 18) {
        Bits b(q + i, 18);
        Seek s;
        s.sample = b.u(64);
        s.offset = b.u(64);
        s.count = static_cast<std::uint32_t>(b.u(16));
        if (s.sample == ~0ull) { placeholder = true; continue; }
        if (placeholder) return fail(d, "seek point after a placeholder");
        if (!d.seek.empty() && s.sample <= prev) return fail(d, "seek points not ascending");
        prev = s.sample;
        d.seek.push_back(s);
      }
    } else if (type == 4) {
      auto le32 = [&](std::size_t o) { return static_cast<std::uint32_t>(q[o] | (q[o + 1] << 8) | (q[o + 2] << 16) | (static_cast<std::uint32_t>(q[o + 3]) << 24)); };
      std::size_t o = 0;
      if (len < 8) return fail(d, "short VORBIS_COMMENT");
      const std::uint32_t vl = le32(0);
      o = 4;
      if (o + vl + 4 > len) return fail(d, "bad VORBIS_COMMENT");
      d.vendor.assign(reinterpret_cast<const char*>(q + o), vl);
      o += vl;
      const std::uint32_t cnt = le32(o);
      o += 4;
      for (std::uint32_t i = 0; i < cnt; ++i) {
        if (o + 4 > len) return fail(d, "bad VORBIS_COMMENT");
        const std::uint32_t l = le32(o);
        o += 4;
        if (o + l > len) return fail(d, "bad VORBIS_COMMENT");
        d.comments.emplace_back(reinterpret_cast<const char*>(q + o), l);
        o += l;
      }
    }
    pos += len;
  }
  if (!have_si) return fail(d, "no STREAMINFO");
  d.first_frame = pos;
  const Info& in = d.info;
  std::uint64_t decoded = 0, idx = 0;
  while (pos < f.size()) {
    const std::size_t start = pos;
    Bits b(f.data() + pos, f.size() - pos);
    if (b.u(14) != 0x3FFE) return fail(d, "bad frame sync");
    if (b.u(1) != 0) return fail(d, "reserved bit set");
    if (b.u(1) != 0) return fail(d, "variable blocking in a fixed stream");
    const int bsc = static_cast<int>(b.u(4)), rc = static_cast<int>(b.u(4)), ca = static_cast<int>(b.u(4)), sc = static_cast<int>(b.u(3));
    if (b.u(1) != 0) return fail(d, "reserved bit set");
    // UTF-8 coded number
    std::uint64_t num = b.u(8);
    int extra = 0;
    if (num >= 0xFC) extra = num == 0xFE ? 6 : 5;
    else if (num >= 0xF8) extra = 4;
    else if (num >= 0xF0) extra = 3;
    else if (num >= 0xE0) extra = 2;
    else if (num >= 0xC0) extra = 1;
    if (extra) {
      num = extra == 6 ? 0 : (num & (0x3Fu >> extra));
      for (int i = 0; i < extra; ++i) {
        const std::uint64_t c = b.u(8);
        if ((c & 0xC0) != 0x80) return fail(d, "bad UTF-8 frame number");
        num = (num << 6) | (c & 0x3F);
      }
    }
    if (num != idx) return fail(d, "frame number out of sequence");
    std::uint32_t bs;
    if (bsc == 0) return fail(d, "reserved block size");
    else if (bsc == 1) bs = 192;
    else if (bsc <= 5) bs = 576u << (bsc - 2);
    else if (bsc == 6) bs = static_cast<std::uint32_t>(b.u(8)) + 1;
    else if (bsc == 7) bs = static_cast<std::uint32_t>(b.u(16)) + 1;
    else bs = 256u << (bsc - 8);
    std::uint32_t rate = in.rate;
    static const std::uint32_t kRates[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    if (rc >= 1 && rc <= 11) rate = kRates[rc];
    else if (rc == 12) rate = static_cast<std::uint32_t>(b.u(8)) * 1000;
    else if (rc == 13) rate = static_cast<std::uint32_t>(b.u(16));
    else if (rc == 14) rate = static_cast<std::uint32_t>(b.u(16)) * 10;
    else if (rc == 15) return fail(d, "invalid sample rate code");
    if (rate != in.rate) return fail(d, "frame rate differs from STREAMINFO");
    const std::size_t hdr_len = b.bytepos() + (b.bitpos() % 8 ? 1 : 0);
    const std::uint8_t hc = static_cast<std::uint8_t>(b.u(8));
    if (hc != crc8_bitwise(f.data() + pos, hdr_len)) return fail(d, "frame header CRC-8 mismatch");
    static const int kBps[8] = {0, 8, 12, -1, 16, 20, 24, 32};
    const int fb = sc == 0 ? static_cast<int>(in.bps) : kBps[sc];
    if (fb <= 0 || fb != static_cast<int>(in.bps)) return fail(d, "bad sample size code");
    int chans;
    if (ca < 8) chans = ca + 1;
    else if (ca <= 10) chans = 2;
    else return fail(d, "reserved channel assignment");
    if (static_cast<std::uint32_t>(chans) != in.channels) return fail(d, "channel count differs from STREAMINFO");
    if (bs < 1 || bs > in.max_bs) return fail(d, "block size outside STREAMINFO range");
    d.frame_offsets.push_back(start - d.first_frame);
    d.frame_first_sample.push_back(decoded);
    std::vector<std::vector<std::int64_t>> ch(static_cast<std::size_t>(chans));
    for (int c = 0; c < chans; ++c) {
      int bp = fb;
      if ((ca == 8 && c == 1) || (ca == 9 && c == 0) || (ca == 10 && c == 1)) ++bp;
      if (!decode_subframe(b, static_cast<int>(bs), bp, ch[static_cast<std::size_t>(c)], d)) return false;
    }
    if (ca >= 8) {
      ++d.modes[ca - 7];
      auto& A = ch[0];
      auto& B = ch[1];
      for (std::uint32_t i = 0; i < bs; ++i) {
        std::int64_t l, r;
        if (ca == 8) { l = A[i]; r = A[i] - B[i]; }
        else if (ca == 9) { r = B[i]; l = A[i] + B[i]; }
        else {
          const std::int64_t side = B[i];
          const std::int64_t mid = (A[i] * 2) | (side & 1);
          l = (mid + side) >> 1;
          r = (mid - side) >> 1;
        }
        A[i] = l;
        B[i] = r;
      }
    } else if (chans == 2) {
      ++d.modes[0];
    }
    b.align();
    const std::size_t body = b.bytepos();
    const std::uint16_t cc = static_cast<std::uint16_t>(b.u(16));
    if (b.bad()) return fail(d, "frame truncated");
    if (cc != crc16_bitwise(f.data() + pos, body)) return fail(d, "frame CRC-16 mismatch");
    for (std::uint32_t i = 0; i < bs; ++i)
      for (int c = 0; c < chans; ++c) d.pcm.push_back(static_cast<std::int32_t>(ch[static_cast<std::size_t>(c)][i]));
    pos += b.bytepos();
    const std::size_t fsize = pos - start;
    if (fsize < in.min_fs || fsize > in.max_fs) return fail(d, "frame size outside STREAMINFO range");
    decoded += bs;
    ++idx;
  }
  if (in.total != decoded) return fail(d, "total samples differs from STREAMINFO");
  // MD5 of the decoded samples
  const std::uint32_t nb = (in.bps + 7) / 8;
  std::vector<std::uint8_t> raw;
  raw.reserve(d.pcm.size() * nb);
  for (std::int32_t v : d.pcm)
    for (std::uint32_t k = 0; k < nb; ++k) raw.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> (8 * k)));
  std::uint8_t dig[16];
  md5(raw.data(), raw.size(), dig);
  if (std::memcmp(dig, in.md5, 16) != 0) return fail(d, "MD5 mismatch");
  d.ok = true;
  return true;
}

}  // namespace flacref
