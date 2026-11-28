// FLAC encoder, written from the format specification (RFC 9639). See flac.hpp.
#include "deckhand/capture/flac.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

namespace deckhand::capture {

// ---- MD5 (RFC 1321) ----------------------------------------------------------------------------------------------------

namespace {
inline std::uint32_t rol(std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
}  // namespace

Md5::Md5() : s_{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u} { std::memset(buf_, 0, sizeof(buf_)); }

void Md5::block(const std::uint8_t* p) {
  static const int kShift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  static std::uint32_t k[64];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 64; ++i) k[i] = static_cast<std::uint32_t>(std::floor(std::fabs(std::sin(static_cast<double>(i + 1))) * 4294967296.0));
    init = true;
  }
  std::uint32_t m[16];
  for (int i = 0; i < 16; ++i)
    m[i] = static_cast<std::uint32_t>(p[i * 4]) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 8) | (static_cast<std::uint32_t>(p[i * 4 + 2]) << 16) |
           (static_cast<std::uint32_t>(p[i * 4 + 3]) << 24);
  std::uint32_t a = s_[0], b = s_[1], c = s_[2], d = s_[3];
  for (int i = 0; i < 64; ++i) {
    std::uint32_t f;
    int g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
    else { f = c ^ (b | ~d); g = (7 * i) & 15; }
    const std::uint32_t t = d;
    d = c;
    c = b;
    b = b + rol(a + f + k[i] + m[g], kShift[i]);
    a = t;
  }
  s_[0] += a; s_[1] += b; s_[2] += c; s_[3] += d;
}

void Md5::update(const void* data, std::size_t n) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  len_ += n;
  while (n > 0) {
    if (fill_ == 0 && n >= 64) { block(p); p += 64; n -= 64; continue; }
    const std::size_t take = std::min<std::size_t>(64 - fill_, n);
    std::memcpy(buf_ + fill_, p, take);
    fill_ += take; p += take; n -= take;
    if (fill_ == 64) { block(buf_); fill_ = 0; }
  }
}

std::array<std::uint8_t, 16> Md5::finish() {
  const std::uint64_t bits = len_ * 8;
  const std::uint8_t pad = 0x80;
  update(&pad, 1);
  const std::uint8_t zero = 0;
  while (fill_ != 56) update(&zero, 1);
  std::uint8_t l[8];
  for (int i = 0; i < 8; ++i) l[i] = static_cast<std::uint8_t>(bits >> (8 * i));
  update(l, 8);
  std::array<std::uint8_t, 16> out{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[static_cast<std::size_t>(i * 4 + j)] = static_cast<std::uint8_t>(s_[i] >> (8 * j));
  return out;
}

// ---- CRCs --------------------------------------------------------------------------------------------------------------

namespace {

struct CrcTables {
  std::uint8_t c8[256];
  std::uint16_t c16[256];
  CrcTables() {
    for (int i = 0; i < 256; ++i) {
      std::uint32_t a = static_cast<std::uint32_t>(i);
      for (int k = 0; k < 8; ++k) a = (a & 0x80) ? ((a << 1) ^ 0x07) : (a << 1);
      c8[i] = static_cast<std::uint8_t>(a);
      std::uint32_t b = static_cast<std::uint32_t>(i) << 8;
      for (int k = 0; k < 8; ++k) b = (b & 0x8000) ? ((b << 1) ^ 0x8005) : (b << 1);
      c16[i] = static_cast<std::uint16_t>(b);
    }
  }
};
const CrcTables& tables() {
  static const CrcTables t;
  return t;
}
std::uint8_t crc8(const std::uint8_t* p, std::size_t n) {
  std::uint8_t c = 0;
  for (std::size_t i = 0; i < n; ++i) c = tables().c8[c ^ p[i]];
  return c;
}
std::uint16_t crc16(const std::uint8_t* p, std::size_t n) {
  std::uint16_t c = 0;
  for (std::size_t i = 0; i < n; ++i) c = static_cast<std::uint16_t>((c << 8) ^ tables().c16[(c >> 8) ^ p[i]]);
  return c;
}

// ---- bit writer --------------------------------------------------------------------------------------------------------

struct BitWriter {
  std::vector<std::uint8_t> buf;
  std::uint64_t acc = 0;
  int n = 0;  // bits held in acc (0..7 between calls)
  void clear() { buf.clear(); acc = 0; n = 0; }
  std::uint64_t bit_count() const { return static_cast<std::uint64_t>(buf.size()) * 8 + static_cast<std::uint64_t>(n); }
  void put(std::uint32_t v, int bits) {  // low `bits` bits of v, 0..32
    if (bits <= 0) return;
    const std::uint64_t mask = bits >= 32 ? 0xFFFFFFFFull : ((1ull << bits) - 1);
    acc = (acc << bits) | (v & mask);
    n += bits;
    while (n >= 8) {
      n -= 8;
      buf.push_back(static_cast<std::uint8_t>(acc >> n));
    }
    acc &= (1ull << n) - 1;
  }
  void put_signed(std::int32_t v, int bits) { put(static_cast<std::uint32_t>(v), bits); }
  void put_zeros_then_one(std::uint32_t zeros) {
    while (zeros >= 32) { put(0, 32); zeros -= 32; }
    put(1u, static_cast<int>(zeros) + 1);
  }
  void align() { if (n) put(0, 8 - n); }
  void append(const BitWriter& o) {
    for (std::uint8_t b : o.buf) put(b, 8);
    if (o.n) put(static_cast<std::uint32_t>(o.acc), o.n);
  }
};

// ---- residual coding ----------------------------------------------------------------------------------------------------

inline std::uint32_t zigzag(std::int32_t r) { return (static_cast<std::uint32_t>(r) << 1) ^ static_cast<std::uint32_t>(r >> 31); }

struct PartCost {
  int k;
  std::int64_t bits;
};
PartCost part_cost(std::uint64_t sum, std::int64_t cnt) {
  if (cnt <= 0) return {0, 0};
  const std::uint64_t mean = sum / static_cast<std::uint64_t>(cnt);
  int k0 = 0;
  while (k0 < 30 && (mean >> (k0 + 1)) != 0) ++k0;
  PartCost best{0, INT64_MAX};
  for (int k = std::max(0, k0 - 1); k <= std::min(30, k0 + 1); ++k) {
    const std::int64_t b = cnt * (k + 1) + static_cast<std::int64_t>(sum >> k);
    if (b < best.bits) best = {k, b};
  }
  return best;
}

int max_partition_order(int n, int order, int cap) {
  int p = 0;
  while (p < cap && p < 15 && (n % (1 << (p + 1))) == 0 && (n >> (p + 1)) > order) ++p;
  return p;
}

void partition_sums(const std::uint32_t* u, int n, int order, int po, std::vector<std::uint64_t>& sums, std::vector<std::int64_t>& cnt) {
  const int parts = 1 << po, per = n >> po;
  sums.assign(static_cast<std::size_t>(parts), 0);
  cnt.assign(static_cast<std::size_t>(parts), 0);
  int pos = 0;
  for (int i = 0; i < parts; ++i) {
    const int len = per - (i == 0 ? order : 0);
    std::uint64_t s = 0;
    for (int j = 0; j < len; ++j) s += u[pos + j];
    sums[static_cast<std::size_t>(i)] = s;
    cnt[static_cast<std::size_t>(i)] = len;
    pos += len;
  }
}

// Best partition order for the zigzagged residual u (n - order values); returns estimated bits including the 6-bit header.
std::int64_t rice_search(const std::uint32_t* u, int n, int order, int cap, int& best_po) {
  const int maxp = max_partition_order(n, order, cap);
  std::vector<std::uint64_t> sums;
  std::vector<std::int64_t> cnt;
  partition_sums(u, n, order, maxp, sums, cnt);
  std::int64_t best = INT64_MAX;
  best_po = 0;
  for (int p = maxp; p >= 0; --p) {
    std::int64_t bits = 6 + 4ll * (1 << p);
    for (std::size_t i = 0; i < sums.size(); ++i) bits += part_cost(sums[i], cnt[i]).bits;
    if (bits < best) { best = bits; best_po = p; }
    if (p > 0) {
      for (std::size_t i = 0; i < sums.size() / 2; ++i) {
        sums[i] = sums[2 * i] + sums[2 * i + 1];
        cnt[i] = cnt[2 * i] + cnt[2 * i + 1];
      }
      sums.resize(sums.size() / 2);
      cnt.resize(cnt.size() / 2);
    }
  }
  return best;
}

void write_residual(BitWriter& bw, const std::int32_t* r, int n, int order, int po) {
  std::vector<std::uint32_t> u(static_cast<std::size_t>(n - order));
  for (int i = 0; i < n - order; ++i) u[static_cast<std::size_t>(i)] = zigzag(r[i]);
  std::vector<std::uint64_t> sums;
  std::vector<std::int64_t> cnt;
  partition_sums(u.data(), n, order, po, sums, cnt);
  std::vector<int> ks(sums.size());
  int kmax = 0;
  for (std::size_t i = 0; i < sums.size(); ++i) { ks[i] = part_cost(sums[i], cnt[i]).k; kmax = std::max(kmax, ks[i]); }
  const int method = kmax > 14 ? 1 : 0;
  const int pbits = method ? 5 : 4;
  bw.put(static_cast<std::uint32_t>(method), 2);
  bw.put(static_cast<std::uint32_t>(po), 4);
  std::size_t pos = 0;
  for (std::size_t i = 0; i < sums.size(); ++i) {
    const int k = ks[i];
    bw.put(static_cast<std::uint32_t>(k), pbits);
    for (std::int64_t j = 0; j < cnt[i]; ++j) {
      const std::uint32_t v = u[pos++];
      bw.put_zeros_then_one(v >> k);
      bw.put(v, k);  // the low k bits
    }
  }
}

// ---- subframes ---------------------------------------------------------------------------------------------------------

struct Plan {
  enum Kind { Verbatim, Fixed, Lpc } kind = Verbatim;
  int order = 0, po = 0, shift = 0, prec = 0;
  std::int64_t bits = INT64_MAX;
  std::vector<int> qlp;
  std::vector<std::int32_t> res;
};

class SubframeEncoder {
 public:
  explicit SubframeEncoder(int level) : level_(level) {}

  // Writes one subframe for the n samples x (each `bps` bits signed) into out.
  void encode(const std::int32_t* x, int n, int bps, BitWriter& out) {
    std::uint32_t orv = 0;
    for (int i = 0; i < n; ++i) orv |= static_cast<std::uint32_t>(x[i]);
    int wasted = 0;
    if (orv != 0) while (((orv >> wasted) & 1u) == 0) ++wasted;
    xs_.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) xs_[static_cast<std::size_t>(i)] = x[i] >> wasted;  // exact: the low bits are zero
    const int b = bps - wasted;
    auto header = [&](int type) {
      out.put(0, 1);
      out.put(static_cast<std::uint32_t>(type), 6);
      out.put(wasted ? 1u : 0u, 1);
      if (wasted) out.put_zeros_then_one(static_cast<std::uint32_t>(wasted - 1));
    };
    bool constant = true;
    for (int i = 1; i < n && constant; ++i) constant = xs_[static_cast<std::size_t>(i)] == xs_[0];
    if (constant) {
      header(0);
      out.put_signed(xs_[0], b);
      return;
    }
    const std::int64_t verbatim_bits = 8 + static_cast<std::int64_t>(n) * b;
    Plan best;
    best.bits = verbatim_bits;
    if (n >= 16) search(n, b, best);
    if (best.kind != Plan::Verbatim) {
      const std::uint64_t mark = out.bit_count();
      write_plan(out, best, n, b, header);
      if (static_cast<std::int64_t>(out.bit_count() - mark) <= verbatim_bits) return;
      // The estimate was optimistic: fall back to verbatim (rewind by rebuilding the writer's tail).
      rewind(out, mark);
    }
    header(1);
    for (int i = 0; i < n; ++i) out.put_signed(xs_[static_cast<std::size_t>(i)], b);
  }

 private:
  int level_;
  std::vector<std::int32_t> xs_;
  std::vector<std::uint32_t> u_;
  std::vector<double> window_, dw_;
  int window_n_ = 0;

  static void rewind(BitWriter& out, std::uint64_t mark) {
    // Called on a scratch writer that only holds this subframe (mark == 0), so clearing is correct.
    (void)mark;
    out.clear();
  }

  void consider(Plan& best, Plan::Kind kind, int order, std::vector<std::int32_t>& res, int n, int b, int shift, int prec, const std::vector<int>* qlp) {
    u_.resize(res.size());
    for (std::size_t i = 0; i < res.size(); ++i) u_[i] = zigzag(res[i]);
    int po = 0;
    const std::int64_t rb = rice_search(u_.data(), n, order, 8, po);
    std::int64_t bits = 8 + static_cast<std::int64_t>(order) * b + rb;
    if (kind == Plan::Lpc) bits += 9 + static_cast<std::int64_t>(order) * prec;
    if (bits < best.bits) {
      best.kind = kind;
      best.order = order;
      best.po = po;
      best.shift = shift;
      best.prec = prec;
      best.bits = bits;
      if (qlp) best.qlp = *qlp;
      best.res = res;
    }
  }

  void search(int n, int b, Plan& best) {
    const std::int32_t* x = xs_.data();
    std::vector<std::int32_t> res;
    // Fixed predictors 0..4.
    for (int order = 0; order <= 4 && order < n; ++order) {
      res.resize(static_cast<std::size_t>(n - order));
      bool ok = true;
      for (int i = order; i < n && ok; ++i) {
        std::int64_t r;
        switch (order) {
          case 0: r = x[i]; break;
          case 1: r = static_cast<std::int64_t>(x[i]) - x[i - 1]; break;
          case 2: r = static_cast<std::int64_t>(x[i]) - 2ll * x[i - 1] + x[i - 2]; break;
          case 3: r = static_cast<std::int64_t>(x[i]) - 3ll * x[i - 1] + 3ll * x[i - 2] - x[i - 3]; break;
          default: r = static_cast<std::int64_t>(x[i]) - 4ll * x[i - 1] + 6ll * x[i - 2] - 4ll * x[i - 3] + x[i - 4]; break;
        }
        if (r > INT32_MAX || r < -INT32_MAX) ok = false; else res[static_cast<std::size_t>(i - order)] = static_cast<std::int32_t>(r);
      }
      if (ok) consider(best, Plan::Fixed, order, res, n, b, 0, 0, nullptr);
    }
    if (level_ <= 0) return;
    // LPC: windowed autocorrelation, Levinson-Durbin for every order up to the limit.
    int maxo = level_ >= 5 ? 12 : 8;
    maxo = std::min(maxo, n / 4);
    if (maxo < 1) return;
    if (window_n_ != n) {  // Tukey(0.5)
      window_.assign(static_cast<std::size_t>(n), 1.0);
      const int ramp = static_cast<int>(0.5 * n / 2);
      for (int i = 0; i < ramp; ++i) {
        const double w = 0.5 - 0.5 * std::cos(3.14159265358979323846 * i / std::max(1, ramp));
        window_[static_cast<std::size_t>(i)] = w;
        window_[static_cast<std::size_t>(n - 1 - i)] = w;
      }
      window_n_ = n;
    }
    dw_.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) dw_[static_cast<std::size_t>(i)] = x[i] * window_[static_cast<std::size_t>(i)];
    std::vector<double> ac(static_cast<std::size_t>(maxo + 1), 0.0);
    for (int lag = 0; lag <= maxo; ++lag) {
      double s = 0;
      for (int i = lag; i < n; ++i) s += dw_[static_cast<std::size_t>(i)] * dw_[static_cast<std::size_t>(i - lag)];
      ac[static_cast<std::size_t>(lag)] = s;
    }
    if (!(ac[0] > 0)) return;
    ac[0] *= 1.0 + 1e-9;
    std::vector<std::vector<double>> coef(static_cast<std::size_t>(maxo + 1));
    std::vector<double> err(static_cast<std::size_t>(maxo + 1), 0.0);
    std::vector<double> a(static_cast<std::size_t>(maxo + 1), 0.0), t(static_cast<std::size_t>(maxo + 1), 0.0);
    double e = ac[0];
    int have = 0;
    for (int m = 1; m <= maxo; ++m) {
      double acc = ac[static_cast<std::size_t>(m)];
      for (int j = 1; j < m; ++j) acc -= a[static_cast<std::size_t>(j)] * ac[static_cast<std::size_t>(m - j)];
      const double k = acc / e;
      if (!std::isfinite(k) || std::fabs(k) >= 1.0) break;
      for (int j = 1; j < m; ++j) t[static_cast<std::size_t>(j)] = a[static_cast<std::size_t>(j)] - k * a[static_cast<std::size_t>(m - j)];
      for (int j = 1; j < m; ++j) a[static_cast<std::size_t>(j)] = t[static_cast<std::size_t>(j)];
      a[static_cast<std::size_t>(m)] = k;
      e *= 1.0 - k * k;
      if (!(e > 0)) break;
      coef[static_cast<std::size_t>(m)].assign(a.begin() + 1, a.begin() + m + 1);
      err[static_cast<std::size_t>(m)] = e;
      have = m;
    }
    if (have < 1) return;
    const int prec = n <= 192 ? 9 : n <= 384 ? 10 : n <= 576 ? 11 : n <= 1152 ? 12 : n <= 2304 ? 13 : 14;
    // Order whose predicted residual size is smallest, plus a few fixed orders around it.
    int est = 1;
    double est_bits = 1e300;
    for (int m = 1; m <= have; ++m) {
      const double bpr = std::max(0.0, 0.5 * std::log2(std::max(err[static_cast<std::size_t>(m)], 1e-9) / std::max(1, n - m)));
      const double total = bpr * (n - m) + m * (b + prec);
      if (total < est_bits) { est_bits = total; est = m; }
    }
    std::vector<int> orders{est};
    if (level_ >= 5) for (int o : {4, 8, 12}) if (o <= have && std::find(orders.begin(), orders.end(), o) == orders.end()) orders.push_back(o);
    for (int order : orders) {
      std::vector<int> q;
      int shift;
      if (!quantise(coef[static_cast<std::size_t>(order)], prec, q, shift)) continue;
      res.resize(static_cast<std::size_t>(n - order));
      bool ok = true;
      for (int i = order; i < n && ok; ++i) {
        std::int64_t sum = 0;
        for (int j = 0; j < order; ++j) sum += static_cast<std::int64_t>(q[static_cast<std::size_t>(j)]) * x[i - 1 - j];
        const std::int64_t r = static_cast<std::int64_t>(x[i]) - (sum >> shift);
        if (r > INT32_MAX || r < -INT32_MAX) ok = false; else res[static_cast<std::size_t>(i - order)] = static_cast<std::int32_t>(r);
      }
      if (ok) consider(best, Plan::Lpc, order, res, n, b, shift, prec, &q);
    }
  }

  static bool quantise(const std::vector<double>& c, int prec, std::vector<int>& q, int& shift) {
    double cmax = 0;
    for (double v : c) cmax = std::max(cmax, std::fabs(v));
    if (!(cmax > 0) || !std::isfinite(cmax)) return false;
    int e;
    std::frexp(cmax, &e);  // cmax < 2^e
    shift = std::min(15, prec - 1 - e);
    if (shift < 0) shift = 0;
    const int hi = (1 << (prec - 1)) - 1, lo = -(1 << (prec - 1));
    q.resize(c.size());
    double carry = 0;
    for (std::size_t i = 0; i < c.size(); ++i) {
      const double v = c[i] * std::ldexp(1.0, shift) + carry;
      long r = std::lround(v);
      r = std::clamp(r, static_cast<long>(lo), static_cast<long>(hi));
      carry = v - static_cast<double>(r);
      q[i] = static_cast<int>(r);
    }
    return true;
  }

  template <class H>
  void write_plan(BitWriter& out, const Plan& p, int n, int b, H& header) {
    if (p.kind == Plan::Fixed) header(8 + p.order);
    else header(32 + (p.order - 1));
    for (int i = 0; i < p.order; ++i) out.put_signed(xs_[static_cast<std::size_t>(i)], b);
    if (p.kind == Plan::Lpc) {
      out.put(static_cast<std::uint32_t>(p.prec - 1), 4);
      out.put(static_cast<std::uint32_t>(p.shift), 5);
      for (int c : p.qlp) out.put_signed(c, p.prec);
    }
    write_residual(out, p.res.data(), n, p.order, p.po);
  }
};

// ---- frame header pieces -----------------------------------------------------------------------------------------------

void put_utf8(std::vector<std::uint8_t>& v, std::uint64_t x) {
  if (x < 0x80) { v.push_back(static_cast<std::uint8_t>(x)); return; }
  int extra = 1;
  while (extra < 6 && x >= (1ull << (5 * extra + 6))) ++extra;  // 2 bytes hold 11 bits, 3 hold 16, ... 7 hold 36
  static const std::uint8_t kLead[7] = {0, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE};
  v.push_back(static_cast<std::uint8_t>(kLead[extra] | (extra == 6 ? 0 : (x >> (6 * extra)))));
  for (int i = extra - 1; i >= 0; --i) v.push_back(static_cast<std::uint8_t>(0x80 | ((x >> (6 * i)) & 0x3F)));
}

int sample_rate_code(std::uint32_t r) {
  switch (r) {
    case 88200: return 1;
    case 176400: return 2;
    case 192000: return 3;
    case 8000: return 4;
    case 16000: return 5;
    case 22050: return 6;
    case 24000: return 7;
    case 32000: return 8;
    case 44100: return 9;
    case 48000: return 10;
    case 96000: return 11;
    default: return 0;  // from STREAMINFO
  }
}

int bits_code(std::uint32_t b) {
  switch (b) {
    case 8: return 1;
    case 12: return 2;
    case 16: return 4;
    case 20: return 5;
    case 24: return 6;
    default: return 0;
  }
}

void put_be(std::vector<std::uint8_t>& v, std::uint64_t x, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

void put_le32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>(v >> (8 * i))); }

}  // namespace

std::int32_t flac_quantise(float x, std::uint32_t bits) {
  const float scale = static_cast<float>((1u << (bits - 1)) - 1u);
  return static_cast<std::int32_t>(std::lrintf(std::clamp(x, -1.0f, 1.0f) * scale));
}

// ---- writer ------------------------------------------------------------------------------------------------------------

struct FlacWriter::Impl {
  struct SeekPoint {
    std::uint64_t sample, offset;
    std::uint16_t count;
  };
  FlacParams p;
  std::FILE* f = nullptr;
  std::vector<std::vector<std::int32_t>> planar;
  std::uint32_t fill = 0;
  Md5 md5;
  std::uint64_t total = 0, frame_idx = 0, frame_bytes = 0, file_bytes = 0;
  std::uint32_t min_frame = UINT32_MAX, max_frame = 0, last_n = 0;
  std::vector<SeekPoint> seek;
  std::size_t seek_cap = 0;
  std::uint64_t seek_every = 1;
  long seektable_off = 0;
  bool failed = false;
  SubframeEncoder enc;
  std::vector<std::uint8_t> mdbuf;
  BitWriter fw, sub[4];

  explicit Impl(const FlacParams& params) : p(params), enc(params.level) {}

  bool put(const void* data, std::size_t n) {
    if (std::fwrite(data, 1, n, f) != n) failed = true;
    file_bytes += n;
    return !failed;
  }

  void encode_block(std::uint32_t n) {
    const int ni = static_cast<int>(n);
    const int bps = static_cast<int>(p.bits);
    int assign = static_cast<int>(p.channels) - 1;
    int count = static_cast<int>(p.channels);
    const std::int32_t* chan[8];
    std::vector<std::int32_t> mid, side;
    for (std::uint32_t c = 0; c < p.channels; ++c) chan[c] = planar[c].data();
    if (p.channels == 2) {
      mid.resize(n);
      side.resize(n);
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::int32_t l = planar[0][i], r = planar[1][i];
        side[i] = l - r;
        mid[i] = (l + r) >> 1;
      }
      for (auto& s : sub) s.clear();
      enc.encode(planar[0].data(), ni, bps, sub[0]);
      enc.encode(planar[1].data(), ni, bps, sub[1]);
      enc.encode(mid.data(), ni, bps, sub[2]);
      enc.encode(side.data(), ni, bps + 1, sub[3]);
      const std::uint64_t l = sub[0].bit_count(), r = sub[1].bit_count(), m = sub[2].bit_count(), s = sub[3].bit_count();
      std::uint64_t best = l + r;
      if (l + s < best) { best = l + s; assign = 8; }
      if (r + s < best) { best = r + s; assign = 9; }
      if (m + s < best) { best = m + s; assign = 10; }
      const BitWriter* pick[2];
      if (assign == 8) { pick[0] = &sub[0]; pick[1] = &sub[3]; }
      else if (assign == 9) { pick[0] = &sub[3]; pick[1] = &sub[1]; }
      else if (assign == 10) { pick[0] = &sub[2]; pick[1] = &sub[3]; }
      else { pick[0] = &sub[0]; pick[1] = &sub[1]; }
      write_frame(n, assign, pick, 2);
      return;
    }
    const BitWriter* pick[8];
    std::vector<BitWriter> subs(static_cast<std::size_t>(count));
    for (int c = 0; c < count; ++c) {
      enc.encode(chan[c], ni, bps, subs[static_cast<std::size_t>(c)]);
      pick[c] = &subs[static_cast<std::size_t>(c)];
    }
    write_frame(n, assign, pick, count);
  }

  void write_frame(std::uint32_t n, int assign, const BitWriter* const* subs, int count) {
    std::vector<std::uint8_t> h;
    h.push_back(0xFF);
    h.push_back(0xF8);  // sync, reserved bit 0, fixed-size blocking
    int bs_code;
    switch (n) {
      case 192: bs_code = 1; break;
      case 576: bs_code = 2; break;
      case 1152: bs_code = 3; break;
      case 2304: bs_code = 4; break;
      case 4608: bs_code = 5; break;
      case 256: bs_code = 8; break;
      case 512: bs_code = 9; break;
      case 1024: bs_code = 10; break;
      case 2048: bs_code = 11; break;
      case 4096: bs_code = 12; break;
      case 8192: bs_code = 13; break;
      case 16384: bs_code = 14; break;
      case 32768: bs_code = 15; break;
      default: bs_code = n <= 256 ? 6 : 7; break;
    }
    const int rate_code = sample_rate_code(p.sample_rate);
    h.push_back(static_cast<std::uint8_t>((bs_code << 4) | rate_code));
    h.push_back(static_cast<std::uint8_t>((assign << 4) | (bits_code(p.bits) << 1)));
    put_utf8(h, frame_idx);
    if (bs_code == 6) h.push_back(static_cast<std::uint8_t>(n - 1));
    else if (bs_code == 7) put_be(h, n - 1, 2);
    h.push_back(crc8(h.data(), h.size()));
    fw.clear();
    for (std::uint8_t b : h) fw.put(b, 8);
    for (int c = 0; c < count; ++c) fw.append(*subs[c]);
    fw.align();
    const std::uint16_t c16 = crc16(fw.buf.data(), fw.buf.size());
    fw.put(c16, 16);
    if (frame_idx % seek_every == 0 && seek.size() < seek_cap) seek.push_back({frame_idx * p.block_size, frame_bytes, static_cast<std::uint16_t>(n)});
    put(fw.buf.data(), fw.buf.size());
    frame_bytes += fw.buf.size();
    min_frame = std::min<std::uint32_t>(min_frame, static_cast<std::uint32_t>(fw.buf.size()));
    max_frame = std::max<std::uint32_t>(max_frame, static_cast<std::uint32_t>(fw.buf.size()));
    ++frame_idx;
    last_n = n;
  }

  void add_samples(const std::int32_t* in, std::uint64_t frames) {
    const std::uint32_t nb = (p.bits + 7) / 8;
    mdbuf.resize(static_cast<std::size_t>(std::min<std::uint64_t>(frames, 4096) * p.channels * nb));
    for (std::uint64_t done = 0; done < frames;) {
      const std::uint64_t chunk = std::min<std::uint64_t>(frames - done, 4096);
      std::size_t o = 0;
      for (std::uint64_t i = 0; i < chunk; ++i)
        for (std::uint32_t c = 0; c < p.channels; ++c) {
          const std::int32_t v = in[(done + i) * p.channels + c];
          for (std::uint32_t k = 0; k < nb; ++k) mdbuf[o++] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> (8 * k));
        }
      md5.update(mdbuf.data(), o);
      // distribute into the block buffers, encoding whenever one fills
      for (std::uint64_t i = 0; i < chunk; ++i) {
        for (std::uint32_t c = 0; c < p.channels; ++c) planar[c][fill] = in[(done + i) * p.channels + c];
        if (++fill == p.block_size) { encode_block(fill); fill = 0; }
      }
      done += chunk;
    }
    total += frames;
  }
};

FlacWriter::FlacWriter() = default;
FlacWriter::~FlacWriter() {
  if (d_ && d_->f) std::fclose(d_->f);
}

std::uint64_t FlacWriter::frames_written() const { return d_ ? d_->total : 0; }
std::uint64_t FlacWriter::bytes_written() const { return d_ ? d_->file_bytes : 0; }

bool FlacWriter::create(const std::string& path, const FlacParams& p, const TrackTags* tags, std::string& error) {
  if (p.channels < 1 || p.channels > 8) { error = "FLAC holds 1 to 8 channels"; return false; }
  if (p.bits < 4 || p.bits > 24) { error = "FLAC export supports 4 to 24 bit"; return false; }
  if (p.sample_rate < 1 || p.sample_rate > 1048575) { error = "sample rate out of range for FLAC"; return false; }
  if (p.block_size < 16 || p.block_size > 65535) { error = "FLAC block size must be 16 to 65535"; return false; }
  d_ = std::make_unique<Impl>(p);
  Impl& d = *d_;
  d.planar.assign(p.channels, std::vector<std::int32_t>(p.block_size));
  d.f = std::fopen(path.c_str(), "wb");
  if (!d.f) { error = "could not create " + path; return false; }
  // Seek table geometry: one point about every 10 s of audio.
  d.seek_every = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(10.0 * p.sample_rate / p.block_size)));
  if (p.total_frames_hint > 0) {
    const std::uint64_t blocks = (p.total_frames_hint + p.block_size - 1) / p.block_size;
    while ((blocks + d.seek_every - 1) / d.seek_every > 4096) d.seek_every *= 2;
    d.seek_cap = static_cast<std::size_t>((blocks + d.seek_every - 1) / d.seek_every);
  } else {
    d.seek_cap = 512;
  }
  if (d.seek_cap < 1) d.seek_cap = 1;
  // Metadata.
  auto block_header = [&](std::vector<std::uint8_t>& v, bool last, int type, std::uint32_t len) {
    v.push_back(static_cast<std::uint8_t>((last ? 0x80 : 0) | type));
    put_be(v, len, 3);
  };
  std::vector<std::uint8_t> head{'f', 'L', 'a', 'C'};
  block_header(head, false, 0, 34);
  head.resize(head.size() + 34, 0);  // STREAMINFO, rewritten at close
  block_header(head, false, 3, static_cast<std::uint32_t>(18 * d.seek_cap));
  d.seektable_off = static_cast<long>(head.size());
  for (std::size_t i = 0; i < d.seek_cap; ++i) {
    put_be(head, 0xFFFFFFFFFFFFFFFFull, 8);
    put_be(head, 0, 8);
    put_be(head, 0, 2);
  }
  std::string vc;
  const std::string vendor = "DeckHand";
  put_le32(vc, static_cast<std::uint32_t>(vendor.size()));
  vc += vendor;
  std::vector<std::string> fields;
  if (tags) {
    auto add = [&](const char* key, const std::string& val) { if (!val.empty()) fields.push_back(std::string(key) + "=" + val); };
    add("TITLE", tags->title);
    add("ARTIST", tags->artist);
    add("ALBUM", tags->album);
    add("GENRE", tags->genre);
    add("DATE", tags->year);
    add("COMMENT", tags->comment);
    if (tags->track_number > 0) add("TRACKNUMBER", std::to_string(tags->track_number));
    if (tags->track_total > 0) add("TRACKTOTAL", std::to_string(tags->track_total));
  }
  put_le32(vc, static_cast<std::uint32_t>(fields.size()));
  for (const auto& s : fields) { put_le32(vc, static_cast<std::uint32_t>(s.size())); vc += s; }
  block_header(head, p.padding_bytes == 0, 4, static_cast<std::uint32_t>(vc.size()));
  head.insert(head.end(), vc.begin(), vc.end());
  if (p.padding_bytes > 0) {
    block_header(head, true, 1, p.padding_bytes);
    head.resize(head.size() + p.padding_bytes, 0);
  }
  if (!d.put(head.data(), head.size())) { error = "write failed"; std::fclose(d.f); d.f = nullptr; return false; }
  d.file_bytes = head.size();
  return true;
}

bool FlacWriter::write_int(const std::int32_t* in, std::uint64_t frames) {
  if (!d_ || !d_->f) return false;
  Impl& d = *d_;
  const std::int32_t hi = static_cast<std::int32_t>((1u << (d.p.bits - 1)) - 1u), lo = -hi - 1;
  std::vector<std::int32_t> tmp;
  for (std::uint64_t done = 0; done < frames;) {
    const std::uint64_t n = std::min<std::uint64_t>(frames - done, 16384);
    tmp.resize(static_cast<std::size_t>(n * d.p.channels));
    for (std::size_t i = 0; i < tmp.size(); ++i) tmp[i] = std::clamp(in[done * d.p.channels + i], lo, hi);
    d.add_samples(tmp.data(), n);
    done += n;
  }
  if (d.failed) error_ = "write failed";
  return !d.failed;
}

bool FlacWriter::write_float(const float* in, std::uint64_t frames) {
  if (!d_ || !d_->f) return false;
  Impl& d = *d_;
  std::vector<std::int32_t> tmp;
  for (std::uint64_t done = 0; done < frames;) {
    const std::uint64_t n = std::min<std::uint64_t>(frames - done, 16384);
    tmp.resize(static_cast<std::size_t>(n * d.p.channels));
    for (std::size_t i = 0; i < tmp.size(); ++i) tmp[i] = flac_quantise(in[done * d.p.channels + i], d.p.bits);
    d.add_samples(tmp.data(), n);
    done += n;
  }
  if (d.failed) error_ = "write failed";
  return !d.failed;
}

bool FlacWriter::close() {
  if (!d_ || !d_->f) return false;
  Impl& d = *d_;
  if (d.fill > 0) { d.encode_block(d.fill); d.fill = 0; }
  const auto digest = d.md5.finish();
  // STREAMINFO
  BitWriter si;
  const std::uint32_t minbs = d.frame_idx == 1 ? d.last_n : d.p.block_size;
  si.put(minbs, 16);
  si.put(d.frame_idx == 1 ? d.last_n : d.p.block_size, 16);
  si.put(d.min_frame == UINT32_MAX ? 0 : d.min_frame, 24);
  si.put(d.max_frame, 24);
  si.put(d.p.sample_rate, 20);
  si.put(d.p.channels - 1, 3);
  si.put(d.p.bits - 1, 5);
  si.put(static_cast<std::uint32_t>(d.total >> 32) & 0xF, 4);
  si.put(static_cast<std::uint32_t>(d.total), 32);
  for (std::uint8_t b : digest) si.put(b, 8);
  std::vector<std::uint8_t> st;
  for (std::size_t i = 0; i < d.seek_cap; ++i) {
    if (i < d.seek.size()) {
      put_be(st, d.seek[i].sample, 8);
      put_be(st, d.seek[i].offset, 8);
      put_be(st, d.seek[i].count, 2);
    } else {
      put_be(st, 0xFFFFFFFFFFFFFFFFull, 8);
      put_be(st, 0, 8);
      put_be(st, 0, 2);
    }
  }
  bool ok = !d.failed;
  ok = ok && std::fseek(d.f, 8, SEEK_SET) == 0 && std::fwrite(si.buf.data(), 1, si.buf.size(), d.f) == si.buf.size();
  ok = ok && std::fseek(d.f, d.seektable_off, SEEK_SET) == 0 && std::fwrite(st.data(), 1, st.size(), d.f) == st.size();
  ok = std::fclose(d.f) == 0 && ok;
  d.f = nullptr;
  if (!ok) error_ = "write failed";
  return ok;
}

}  // namespace deckhand::capture
