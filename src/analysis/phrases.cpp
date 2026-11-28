// Phrase segmentation and cue suggestions (see analysis.hpp). Works on the bar grid from the beat tracker; falls back
// to fixed time blocks when there is no usable grid. Cheap by design: three biquad band splits over the (already
// decimated) mono signal, one 4096-point FFT per ~370 ms for a coarse chroma, and O(bars) arithmetic.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>

#include "deckhand/analysis/analysis.hpp"
#include "phrases_internal.hpp"

namespace deckhand {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinPhraseSeconds = 30.0;

double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// ---------------------------------------------------------------- small FFT (forward, complex, in place)
class Fft {
 public:
  explicit Fft(std::size_t n) : n_(n), rev_(n), cs_(n / 2), sn_(n / 2) {
    std::size_t bits = 0;
    while ((std::size_t(1) << bits) < n) ++bits;
    for (std::size_t i = 0; i < n; ++i) {
      std::size_t r = 0;
      for (std::size_t b = 0; b < bits; ++b)
        if (i & (std::size_t(1) << b)) r |= std::size_t(1) << (bits - 1 - b);
      rev_[i] = static_cast<std::uint32_t>(r);
    }
    for (std::size_t k = 0; k < n / 2; ++k) {
      cs_[k] = static_cast<float>(std::cos(2.0 * kPi * double(k) / double(n)));
      sn_[k] = static_cast<float>(std::sin(2.0 * kPi * double(k) / double(n)));
    }
  }
  void forward(float* re, float* im) const {
    for (std::size_t i = 0; i < n_; ++i) {
      const std::size_t j = rev_[i];
      if (j > i) {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
    for (std::size_t len = 2; len <= n_; len <<= 1) {
      const std::size_t half = len / 2, step = n_ / len;
      for (std::size_t i = 0; i < n_; i += len)
        for (std::size_t j = 0; j < half; ++j) {
          const float wr = cs_[j * step], wi = -sn_[j * step];
          const std::size_t a = i + j, b = a + half;
          const float tr = re[b] * wr - im[b] * wi;
          const float ti = re[b] * wi + im[b] * wr;
          re[b] = re[a] - tr;
          im[b] = im[a] - ti;
          re[a] += tr;
          im[a] += ti;
        }
    }
  }
  std::size_t size() const { return n_; }

 private:
  std::size_t n_;
  std::vector<std::uint32_t> rev_;
  std::vector<float> cs_, sn_;
};

// ---------------------------------------------------------------- band envelopes
struct Biq {
  double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  double run(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};
Biq lowpass(double fc, double fs) {
  const double w = 2 * kPi * fc / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * 0.70710678);
  const double a0 = 1 + al;
  Biq q;
  q.b0 = (1 - c) / 2 / a0;
  q.b1 = (1 - c) / a0;
  q.b2 = q.b0;
  q.a1 = -2 * c / a0;
  q.a2 = (1 - al) / a0;
  return q;
}

// Prefix sums (index k = sum of the first k hops) of band power and of "activity" (positive log-power steps of
// mid+high, a cheap onset density).
struct Env {
  double hop_s = 0.006;
  std::vector<double> low, mid, high, act;
};

Env band_env(const std::vector<float>& x, double rate) {
  Env e;
  const std::size_t hop = std::max<std::size_t>(16, static_cast<std::size_t>(std::lround(rate * 0.006)));
  e.hop_s = double(hop) / rate;
  const std::size_t nf = x.size() / hop;
  Biq l1 = lowpass(150.0, rate), l2 = l1, m1 = lowpass(2000.0, rate), m2 = m1;
  std::vector<double> pl(nf), pm(nf), ph(nf);
  for (std::size_t f = 0; f < nf; ++f) {
    double sl = 0, sm = 0, sh = 0;
    for (std::size_t i = 0; i < hop; ++i) {
      const double v = x[f * hop + i];
      const double lo = l2.run(l1.run(v));
      const double lm = m2.run(m1.run(v));
      const double md = lm - lo, hi = v - lm;
      sl += lo * lo;
      sm += md * md;
      sh += hi * hi;
    }
    pl[f] = sl / double(hop);
    pm[f] = sm / double(hop);
    ph[f] = sh / double(hop);
  }
  double mean_mh = 0;
  for (std::size_t f = 0; f < nf; ++f) mean_mh += pm[f] + ph[f];
  mean_mh = nf ? mean_mh / double(nf) : 0;
  const double eps = 1e-4 * mean_mh + 1e-12;
  e.low.assign(nf + 1, 0);
  e.mid.assign(nf + 1, 0);
  e.high.assign(nf + 1, 0);
  e.act.assign(nf + 1, 0);
  std::vector<double> s(nf);
  for (std::size_t f = 0; f < nf; ++f) s[f] = 0.5 * (pm[f] + ph[f] + (f ? pm[f - 1] + ph[f - 1] : 0.0));
  for (std::size_t f = 0; f < nf; ++f) {
    e.low[f + 1] = e.low[f] + pl[f];
    e.mid[f + 1] = e.mid[f] + pm[f];
    e.high[f + 1] = e.high[f] + ph[f];
    const double a = f >= 2 ? std::max(0.0, std::log(s[f] + eps) - std::log(s[f - 2] + eps)) : 0.0;
    e.act[f + 1] = e.act[f] + a;
  }
  return e;
}

// Sum of prefix p over [t0, t1) seconds and the hop count.
double range_sum(const std::vector<double>& p, double hop_s, double t0, double t1, double* n = nullptr) {
  const std::ptrdiff_t last = static_cast<std::ptrdiff_t>(p.size()) - 1;
  auto idx = [&](double t) { return std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(std::floor(t / hop_s)), 0, last); };
  const std::ptrdiff_t a = idx(t0), b = idx(t1);
  if (n) *n = double(b - a);
  return p[b] - p[a];
}
double range_mean(const std::vector<double>& p, double hop_s, double t0, double t1) {
  double n = 0;
  const double s = range_sum(p, hop_s, t0, t1, &n);
  return n > 0 ? s / n : 0.0;
}

double to_db(double power) { return std::max(-90.0, 10.0 * std::log10(power + 1e-10)); }

// ---------------------------------------------------------------- chroma per bar
using Chroma = std::array<float, 12>;

std::vector<Chroma> chroma_per_bar(const std::vector<float>& x, double rate, const std::vector<double>& starts_s) {
  const std::size_t nb = starts_s.size() - 1;
  std::vector<Chroma> out(nb);
  for (auto& c : out) c.fill(0.f);
  constexpr std::size_t N = 4096, HOP = 8192;
  if (x.size() < N || nb == 0) return out;
  const Fft fft(N);
  std::vector<float> win(N);
  for (std::size_t i = 0; i < N; ++i) win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(N)));
  // bin -> pitch class (-1 = unused)
  std::vector<int> pc(N / 2 + 1, -1);
  for (std::size_t k = 1; k <= N / 2; ++k) {
    const double f = double(k) * rate / double(N);
    if (f < 55.0 || f > 1760.0) continue;
    const int m = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(f / 440.0)));
    pc[k] = ((m % 12) + 12) % 12;
  }
  const double t_begin = starts_s.front() - 0.1, t_end = starts_s.back();
  std::size_t f0 = 0;
  while (f0 * HOP + N / 2 < static_cast<std::size_t>(std::max(0.0, t_begin) * rate)) ++f0;
  std::vector<std::size_t> frames;
  for (std::size_t f = f0; f * HOP + N <= x.size(); ++f) {
    if (double(f * HOP + N / 2) / rate > t_end) break;
    frames.push_back(f);
  }
  std::vector<float> re(N), im(N);
  std::size_t bar = 0;
  auto accumulate = [&](std::size_t f, auto&& mag_at) {
    const double t = double(f * HOP + N / 2) / rate;
    while (bar + 1 < nb && t >= starts_s[bar + 1]) ++bar;
    if (t < starts_s[0] || t >= starts_s[nb]) return;
    Chroma& c = out[bar];
    for (std::size_t k = 1; k <= N / 2; ++k)
      if (pc[k] >= 0) c[pc[k]] += std::sqrt(mag_at(k));
  };
  for (std::size_t j = 0; j < frames.size(); j += 2) {
    const std::size_t fa = frames[j];
    const bool two = j + 1 < frames.size();
    const std::size_t fb = two ? frames[j + 1] : fa;
    for (std::size_t i = 0; i < N; ++i) {
      re[i] = x[fa * HOP + i] * win[i];
      im[i] = two ? x[fb * HOP + i] * win[i] : 0.f;
    }
    fft.forward(re.data(), im.data());
    auto magA = [&](std::size_t k) {
      const std::size_t k2 = (N - k) & (N - 1);
      const float a = re[k] + re[k2], b = im[k] - im[k2];
      return 0.25f * (a * a + b * b);
    };
    auto magB = [&](std::size_t k) {
      const std::size_t k2 = (N - k) & (N - 1);
      const float a = im[k] + im[k2], b = re[k] - re[k2];
      return 0.25f * (a * a + b * b);
    };
    // frames are visited in time order, bar pointer only moves forward
    accumulate(fa, magA);
    if (two) accumulate(fb, magB);
  }
  for (auto& c : out) {
    double n = 0;
    for (float v : c) n += double(v) * v;
    n = std::sqrt(n);
    if (n > 1e-9)
      for (float& v : c) v = static_cast<float>(v / n);
  }
  return out;
}

double chroma_cos_dist(const std::array<double, 12>& a, const std::array<double, 12>& b) {
  double na = 0, nb = 0, d = 0;
  for (int i = 0; i < 12; ++i) {
    na += a[i] * a[i];
    nb += b[i] * b[i];
    d += a[i] * b[i];
  }
  if (na < 1e-12 || nb < 1e-12) return (na < 1e-12 && nb < 1e-12) ? 0.0 : 0.5;
  return 1.0 - d / std::sqrt(na * nb);
}

// ---------------------------------------------------------------- features and novelty
constexpr int kScalars = 5;  // low, mid, high, kick, density
constexpr double kWeights[kScalars] = {1.0, 0.8, 0.8, 1.5, 0.6};
constexpr double kChromaWeight = 2.0;

struct BarFeatures {
  std::vector<std::array<double, kScalars>> f;  // normalised scalars
  std::vector<Chroma> chroma;
  std::vector<double> kick;       // 0..1
  std::vector<double> energy;     // 0..1 composite
  std::vector<std::array<double, kScalars + 12>> prefix;  // prefix sums for window means
  bool has_kick = true;
};

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  const std::size_t k = std::min(v.size() - 1, static_cast<std::size_t>(q * double(v.size() - 1) + 0.5));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

double norm_range(double v, double lo, double hi, double min_span) {
  if (hi - lo < min_span) return 0.5;
  return clamp01((v - lo) / (hi - lo));
}

// Window mean of the 17-d prefix over bars [a, b).
struct WinMean {
  std::array<double, kScalars> s{};
  std::array<double, 12> c{};
};
WinMean win_mean(const BarFeatures& bf, std::size_t a, std::size_t b) {
  WinMean m;
  const double n = double(b - a);
  if (n <= 0) return m;
  for (int i = 0; i < kScalars; ++i) m.s[i] = (bf.prefix[b][i] - bf.prefix[a][i]) / n;
  for (int i = 0; i < 12; ++i) m.c[i] = (bf.prefix[b][kScalars + i] - bf.prefix[a][kScalars + i]) / n;
  return m;
}
double win_dist(const WinMean& a, const WinMean& b, bool has_kick, bool use_chroma = true) {
  double d = 0;
  for (int i = 0; i < kScalars; ++i) {
    if (i == 3 && !has_kick) continue;
    d += kWeights[i] * std::fabs(a.s[i] - b.s[i]);
  }
  return use_chroma ? d + kChromaWeight * chroma_cos_dist(a.c, b.c) : d;
}

struct Bar {
  double t0 = 0, t1 = 0;  // seconds
  int b0 = 0;             // first beat index (gridded)
};

BarFeatures bar_features(const std::vector<float>& mono, double rate, const Env& env, const std::vector<Bar>& bars,
                         const std::vector<double>* beat_kick /* per beat, may be null */) {
  const std::size_t nb = bars.size();
  BarFeatures bf;
  bf.has_kick = beat_kick != nullptr;
  std::vector<double> dl(nb), dm(nb), dh(nb), dt(nb), dd(nb);
  bf.kick.assign(nb, 0.0);
  std::vector<double> starts;
  for (const Bar& b : bars) starts.push_back(b.t0);
  starts.push_back(bars.back().t1);
  for (std::size_t i = 0; i < nb; ++i) {
    const Bar& b = bars[i];
    const double pl = range_mean(env.low, env.hop_s, b.t0, b.t1), pm = range_mean(env.mid, env.hop_s, b.t0, b.t1),
                 ph = range_mean(env.high, env.hop_s, b.t0, b.t1);
    dl[i] = to_db(pl);
    dm[i] = to_db(pm);
    dh[i] = to_db(ph);
    dt[i] = to_db(pl + pm + ph);
    dd[i] = std::log1p(range_sum(env.act, env.hop_s, b.t0, b.t1) / std::max(0.2, b.t1 - b.t0));
    if (beat_kick) {
      int n = 0;
      for (int k = 0; k < 4; ++k) n += (*beat_kick)[static_cast<std::size_t>(b.b0 + k)] > 0.5 ? 1 : 0;
      bf.kick[i] = n / 4.0;
    }
  }
  auto norm = [&](const std::vector<double>& v, double floor_span) {
    const double lo = percentile(v, 0.1), hi = percentile(v, 0.9);
    const double span = std::max(hi - lo, floor_span);
    std::vector<double> o(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) o[i] = (v[i] - lo) / span;
    return o;
  };
  const auto nl = norm(dl, 4.0), nm = norm(dm, 4.0), nh = norm(dh, 4.0);
  double dmax = 0;
  for (double v : dd) dmax = std::max(dmax, v);
  const auto nd = norm(dd, std::max(0.2 * dmax, 1e-6));
  bf.f.resize(nb);
  for (std::size_t i = 0; i < nb; ++i) bf.f[i] = {nl[i], nm[i], nh[i], bf.kick[i], nd[i]};
  bf.chroma = chroma_per_bar(mono, rate, starts);
  // composite energy 0..1
  const double t5 = percentile(dt, 0.05), t95 = percentile(dt, 0.95);
  const double l5 = percentile(dl, 0.05), l95 = percentile(dl, 0.95);
  const double h5 = percentile(dh, 0.05), h95 = percentile(dh, 0.95);
  bf.energy.resize(nb);
  for (std::size_t i = 0; i < nb; ++i) {
    const double nt = norm_range(dt[i], t5, t95, 2.0), nlo = norm_range(dl[i], l5, l95, 2.0),
                 nhi = norm_range(dh[i], h5, h95, 2.0);
    bf.energy[i] = beat_kick ? 0.45 * nt + 0.25 * nlo + 0.15 * bf.kick[i] + 0.15 * nhi
                             : 0.6 * nt + 0.2 * nlo + 0.2 * nhi;
  }
  bf.prefix.assign(nb + 1, {});
  for (std::size_t i = 0; i < nb; ++i) {
    bf.prefix[i + 1] = bf.prefix[i];
    for (int k = 0; k < kScalars; ++k) bf.prefix[i + 1][k] += bf.f[i][k];
    for (int k = 0; k < 12; ++k) bf.prefix[i + 1][kScalars + k] += bf.chroma[i][k];
  }
  return bf;
}

// ---------------------------------------------------------------- segmentation
struct Boundary {
  std::size_t bar;
  double strength;  // 0..1
};

// Candidate boundaries as bar indices (1..nb-1), strongest-first resolution within +-3 bars.
std::vector<Boundary> find_boundaries(const BarFeatures& bf, std::size_t nb, bool phrase_prior, std::size_t min_gap) {
  std::vector<double> S(nb, 0.0), Sp(nb, 0.0);
  const std::size_t Ls[3] = {2, 4, 8};
  const double Lw[3] = {0.45, 0.35, 0.2};
  const std::size_t edge = std::min<std::size_t>(2, nb / 4);
  for (std::size_t b = std::max<std::size_t>(1, edge); b < nb; ++b) {
    if (nb - b < edge) break;
    double s = 0;
    for (int k = 0; k < 3; ++k) {
      const std::size_t a0 = b > Ls[k] ? b - Ls[k] : 0, b1 = std::min(nb, b + Ls[k]);
      s += Lw[k] * win_dist(win_mean(bf, a0, b), win_mean(bf, b, b1), bf.has_kick, Ls[k] >= 4);  // chords change every bar or two: compare harmony over >= 4 bars only
    }
    S[b] = s;
    double mult = 1.0;
    if (phrase_prior) mult = (b % 16 == 0) ? 1.3 : (b % 8 == 0) ? 1.15 : (b % 4 == 0) ? 0.9 : 0.65;
    Sp[b] = s * mult;
  }
  double mx = 0;
  for (double v : Sp) mx = std::max(mx, v);
  std::vector<double> nz(Sp.begin() + 1, Sp.end());
  const double med = percentile(nz, 0.5);
  for (double& v : nz) v = std::fabs(v - med);
  const double mad = percentile(nz, 0.5);
  const double thr = std::max({0.4, 0.08 * mx, med + 2.0 * mad});
  std::vector<Boundary> out;
  for (std::size_t b = 1; b < nb; ++b) {
    if (Sp[b] < thr) continue;
    bool peak = true;
    for (std::size_t j = (b > min_gap - 1 ? b - (min_gap - 1) : 1); j < b && peak; ++j)
      if (Sp[j] >= Sp[b]) peak = false;
    for (std::size_t j = b + 1; j <= std::min(nb - 1, b + (min_gap - 1)) && peak; ++j)
      if (Sp[j] > Sp[b]) peak = false;
    if (!peak) continue;
    std::size_t pos = b;
    if (phrase_prior && b % 4 != 0) {  // snap to a neighbouring multiple of four when it is nearly as strong
      for (std::size_t m : {b - 1, b + 1})
        if (m >= 1 && m < nb && m % 4 == 0 && S[m] >= 0.7 * S[b]) pos = m;
    }
    out.push_back({pos, clamp01(Sp[b] / (thr * 3.0))});
  }
  // enforce minimum gap after snapping
  std::vector<Boundary> res;
  for (const Boundary& bd : out) {
    if (!res.empty() && bd.bar - res.back().bar < min_gap) {
      if (bd.strength > res.back().strength) res.back() = bd;
      continue;
    }
    res.push_back(bd);
  }
  return res;
}

double dist_secs(const BarFeatures& bf, std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1) {
  return win_dist(win_mean(bf, a0, a1), win_mean(bf, b0, b1), bf.has_kick);
}

// ---------------------------------------------------------------- labelling
struct SecStat {
  std::size_t a = 0, b = 0;  // bars [a, b)
  double e = 0, kick = 0, slope = 0;
  bool recurs = false;
};

void label_sections(PhraseAnalysis& pa, const std::vector<SecStat>& st, const BarFeatures& bf) {
  const std::size_t n = pa.sections.size();
  double emax = 0, emin = 1;
  for (const SecStat& s : st) {
    emax = std::max(emax, s.e);
    emin = std::min(emin, s.e);
  }
  const double spread = emax - emin;
  // A section that climbs and is then followed by a clearly higher one is the build into it, however close to the top it
  // already sits: at fast tempi the kick tails overlap and every part of a track reads louder, which pushed the build
  // over the absolute line (the builds of 150 BPM and faster tracks read as drops).
  auto is_high = [&](std::size_t i) {
    if (!(spread > 0.12 && st[i].e >= std::max(0.5, emin + 0.7 * spread))) return false;
    const bool climbs = st[i].slope > 0.06 && i + 1 < n && st[i + 1].e > st[i].e + 0.1;
    return !climbs;
  };
  auto kickless = [&](std::size_t i) { return bf.has_kick && st[i].kick < 0.35; };
  // "club" style: there is a kick-less middle section or something rising into a high section
  bool clubby = false;
  for (std::size_t i = 1; i + 1 < n; ++i) clubby = clubby || kickless(i);
  for (std::size_t i = 0; i < n; ++i) {
    PhraseSection& s = pa.sections[i];
    s.energy = std::clamp(1 + static_cast<int>(std::lround(9.0 * st[i].e)), 1, 10);
    s.kick = bf.has_kick ? st[i].kick : -1.0;
    const bool high = is_high(i);
    const bool next_high = i + 1 < n && is_high(i + 1);
    const double prev_e = i > 0 ? st[i - 1].e : st[i].e;
    std::string label = "verse";
    double conf = 0.35;
    if (i == 0 && !high) {
      label = "intro";
      conf = st[i].e < emin + 0.5 * spread ? 0.85 : 0.5;
    } else if (i + 1 == n && n > 1 && (!high || st[i].e < prev_e - 0.1)) {
      label = "outro";
      conf = (st[i].e < prev_e - 0.1) ? 0.8 : 0.45;
    } else if (high) {
      const bool after_lift = i > 0 && (kickless(i - 1) || st[i - 1].slope > 0.06 || st[i - 1].e < st[i].e - 0.2);
      if (clubby) {
        label = "drop";
        conf = after_lift ? 0.8 : 0.55;
      } else {
        label = "chorus";
        conf = st[i].recurs ? 0.55 : 0.4;
      }
      if (i == 0) conf = std::min(conf, 0.35);
      if (st[i].recurs) conf = std::min(0.9, conf + 0.1);
    } else if (kickless(i) && i > 0 && i + 1 < n && st[i].e < std::min(prev_e, i + 1 < n ? st[i + 1].e : 1.0) - 0.05) {
      label = "breakdown";
      conf = 0.8;
    } else if (next_high && (st[i].slope > 0.06 || st[i].e < st[i + 1].e - 0.15)) {
      label = "build";
      conf = st[i].slope > 0.06 ? 0.75 : 0.5;
    } else if (i > 0 && double(i) / double(n) > 0.6 && !st[i].recurs) {
      label = "bridge";
      conf = 0.3;
    } else {
      label = "verse";
      conf = st[i].recurs ? 0.45 : 0.35;
    }
    s.label = label;
    s.label_confidence = conf * (0.6 + 0.4 * std::min(1.0, s.boundary_confidence + 0.3));
    if (i == 0) s.label_confidence = conf;
    // energy-style kind: safe wording only
    std::string kind;
    if (i == 0 && !high) kind = "intro";
    else if (i + 1 == n && n > 1 && (!high || st[i].e < prev_e - 0.1)) kind = "outro";
    else if (high) kind = "chorus";
    else if (i > 0 && st[i].e > prev_e + 0.1) kind = "up";
    else if (i > 0 && st[i].e < prev_e - 0.1) kind = "down";
    else kind = st[i].e >= emin + 0.5 * spread ? "up" : "down";
    s.kind = kind;
  }
}

// ---------------------------------------------------------------- kick presence per beat
std::vector<double> beat_kicks(const Env& env, const std::vector<double>& beats_ms) {
  const std::size_t n = beats_ms.size();
  std::vector<double> a(n, 0.0), b(n, 0.0), out(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    const double tb = beats_ms[i] / 1000.0;
    double T = i + 1 < n ? (beats_ms[i + 1] - beats_ms[i]) / 1000.0 : (i ? (beats_ms[i] - beats_ms[i - 1]) / 1000.0 : 0.5);
    T = std::clamp(T, 0.2, 1.5);
    a[i] = range_mean(env.low, env.hop_s, tb - 0.012, tb + 0.07);
    b[i] = range_mean(env.low, env.hop_s, tb + 0.55 * T, tb + 0.92 * T);
  }
  const double p90 = percentile(a, 0.9);
  for (std::size_t i = 0; i < n; ++i) out[i] = (a[i] > 1.8 * b[i] && a[i] >= 0.2 * p90 && a[i] > 1e-7) ? 1.0 : 0.0;
  return out;
}

}  // namespace

// ---------------------------------------------------------------- core
namespace detail {

PhraseAnalysis phrases_from_mono(const std::vector<float>& mono, double rate, const BeatGrid& g, double first_ms,
                                 double last_ms, double duration_ms, const AnalysisOptions&) {
  PhraseAnalysis pa;
  if (!(last_ms > first_ms) || (last_ms - first_ms) / 1000.0 < kMinPhraseSeconds) {
    pa.error = "track too short for phrase analysis";
    return pa;
  }
  if (mono.size() < 4096 || rate < 4000) {
    pa.error = "invalid input";
    return pa;
  }
  try {
    const Env env = band_env(mono, rate);
    std::vector<Bar> bars;
    std::vector<double> bk;
    const bool gridded = g.ok && g.bpm > 0 && g.confidence >= 0.35 && g.beats_ms.size() >= 32;
    double bar_ms = 0;
    if (gridded) {
      const auto& bm = g.beats_ms;
      const std::size_t ph = static_cast<std::size_t>(((g.downbeat_phase % 4) + 4) % 4);
      std::size_t i0 = ph;
      for (; i0 + 4 < bm.size(); i0 += 4) {
        const double beat = bm[std::min(i0 + 1, bm.size() - 1)] - bm[i0];
        if (bm[i0] >= first_ms - 0.5 * beat) break;
      }
      for (std::size_t i = i0; i + 4 < bm.size(); i += 4) {
        const double t0 = bm[i], t1 = bm[i + 4];
        if (t0 >= last_ms - 0.5 * (t1 - t0)) break;
        Bar b;
        b.t0 = t0 / 1000.0;
        b.t1 = t1 / 1000.0;
        b.b0 = static_cast<int>(i);
        bars.push_back(b);
      }
      if (bars.size() >= 8) {
        bk = beat_kicks(env, bm);
        pa.gridded = true;
        pa.origin_ms = bars.front().t0 * 1000.0;
        pa.pickup_ms = std::max(0.0, pa.origin_ms - first_ms);
        bar_ms = (bars.back().t1 - bars.front().t0) * 1000.0 / double(bars.size());
      }
    }
    if (!pa.gridded) {
      bars.clear();
      pa.time_based = true;
      bar_ms = 2000.0;
      for (double t = first_ms; t + 1000.0 <= last_ms; t += bar_ms) {
        Bar b;
        b.t0 = t / 1000.0;
        b.t1 = std::min(t + bar_ms, last_ms) / 1000.0;
        bars.push_back(b);
      }
      pa.origin_ms = first_ms;
      if (bars.size() < 8) {
        pa.error = "track too short for phrase analysis";
        return pa;
      }
    }
    pa.bar_ms = bar_ms;
    const std::size_t nb = bars.size();
    pa.total_bars = pa.gridded ? static_cast<int>(nb) : 0;
    for (const Bar& b : bars) pa.bar_starts_ms.push_back(b.t0 * 1000.0);
    pa.bar_starts_ms.push_back(bars.back().t1 * 1000.0);

    const BarFeatures bf = bar_features(mono, rate, env, bars, pa.gridded ? &bk : nullptr);
    pa.bar_energy.resize(nb);
    for (std::size_t i = 0; i < nb; ++i) pa.bar_energy[i] = static_cast<float>(bf.energy[i]);

    const std::size_t min_gap = pa.gridded ? 4 : 5;
    std::vector<Boundary> bd = find_boundaries(bf, nb, pa.gridded, min_gap);
    // drop a final section shorter than the minimum gap
    while (!bd.empty() && nb - bd.back().bar < min_gap) bd.pop_back();

    std::vector<std::size_t> cuts{0};
    std::vector<double> bconf{1.0};
    for (const Boundary& b : bd) {
      cuts.push_back(b.bar);
      bconf.push_back(b.strength);
    }
    cuts.push_back(nb);
    const std::size_t ns = cuts.size() - 1;

    std::vector<SecStat> st(ns);
    for (std::size_t i = 0; i < ns; ++i) {
      SecStat& s = st[i];
      s.a = cuts[i];
      s.b = cuts[i + 1];
      double e = 0, k = 0;
      for (std::size_t j = s.a; j < s.b; ++j) {
        e += bf.energy[j];
        k += bf.kick[j];
      }
      const double len = double(s.b - s.a);
      s.e = e / len;
      s.kick = k / len;
      if (s.b - s.a >= 6) {
        const std::size_t third = (s.b - s.a) / 3;
        double f = 0, l = 0;
        for (std::size_t j = 0; j < third; ++j) {
          f += bf.energy[s.a + j];
          l += bf.energy[s.b - 1 - j];
        }
        s.slope = (l - f) / double(third);
      }
    }
    for (std::size_t i = 0; i < ns; ++i)
      for (std::size_t j = 0; j < ns; ++j) {
        if (i == j) continue;
        const double li = double(st[i].b - st[i].a), lj = double(st[j].b - st[j].a);
        if (li > 2.0 * lj || lj > 2.0 * li) continue;
        const std::size_t L = std::min(st[i].b - st[i].a, st[j].b - st[j].a);
        if (dist_secs(bf, st[i].a, st[i].a + L, st[j].a, st[j].a + L) < 0.6) st[i].recurs = true;
      }

    pa.sections.resize(ns);
    for (std::size_t i = 0; i < ns; ++i) {
      PhraseSection& s = pa.sections[i];
      s.start_ms = pa.bar_starts_ms[st[i].a];
      s.end_ms = pa.bar_starts_ms[st[i].b];
      if (i + 1 == ns) s.end_ms = std::min(duration_ms, std::max(s.end_ms, last_ms));
      if (pa.gridded) {
        s.start_bar = static_cast<int>(st[i].a);
        s.end_bar = static_cast<int>(st[i].b);
      }
      s.bars = static_cast<int>(st[i].b - st[i].a);
      s.boundary_confidence = bconf[i];
      s.phrase_aligned = pa.gridded ? (st[i].a % 4 == 0) : false;
    }
    label_sections(pa, st, bf);
    pa.ok = true;
  } catch (...) {
    pa = PhraseAnalysis{};
    pa.error = "internal error";
  }
  return pa;
}

}  // namespace detail

PhraseAnalysis analyse_phrases(const float* pcm, std::uint64_t frames, std::uint32_t rate, const BeatGrid& grid,
                               double first_audible_ms, double last_audible_ms, const AnalysisOptions& options) {
  PhraseAnalysis pa;
  if (pcm == nullptr || frames == 0 || rate < 8000) {
    pa.error = "invalid input";
    return pa;
  }
  try {
    std::vector<float> x(static_cast<std::size_t>(frames));
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * (pcm[2 * i] + pcm[2 * i + 1]);
    double r = rate;
    while (r > 24500.0 && x.size() > 64) {  // crude 2:1 box decimation is enough for band energies
      std::vector<float> y(x.size() / 2);
      for (std::size_t i = 0; i < y.size(); ++i) y[i] = 0.5f * (x[2 * i] + x[2 * i + 1]);
      x.swap(y);
      r *= 0.5;
    }
    return detail::phrases_from_mono(x, r, grid, first_audible_ms, last_audible_ms, double(frames) * 1000.0 / rate, options);
  } catch (...) {
    pa = PhraseAnalysis{};
    pa.error = "internal error";
    return pa;
  }
}

// ---------------------------------------------------------------- cue habit
namespace {
int nearest_section(const PhraseAnalysis& p, double ms) {
  int best = -1;
  double bd = 1e18;
  for (std::size_t i = 0; i < p.sections.size(); ++i) {
    const double d = std::fabs(p.sections[i].start_ms - ms);
    if (d < bd) {
      bd = d;
      best = static_cast<int>(i);
    }
  }
  return best;
}
bool is_drop_label(const std::string& l) { return l == "drop" || l == "chorus"; }
}  // namespace

CueHabit build_cue_habit(const std::vector<HabitSample>& samples) {
  CueHabit h;
  double sum_before = 0;
  for (const HabitSample& s : samples) {
    const PhraseAnalysis& p = s.phrases;
    if (!p.ok || p.sections.empty() || p.bar_ms <= 0 || p.time_based) continue;
    int drop = -1;
    for (std::size_t i = 0; i < p.sections.size(); ++i)
      if (is_drop_label(p.sections[i].label) && p.sections[i].energy >= 6) {
        drop = static_cast<int>(i);
        break;
      }
    for (double cue : s.cue_ms) {
      const int k = nearest_section(p, cue);
      if (k < 0) continue;
      const long off = std::lround((cue - p.sections[static_cast<std::size_t>(k)].start_ms) / p.bar_ms);
      if (off < -16 || off > 16) continue;
      h.bar_offset_hist[static_cast<std::size_t>(off + 16)]++;
      h.samples++;
    }
    if (drop >= 0) {
      const double ds = p.sections[static_cast<std::size_t>(drop)].start_ms;
      // choose the cue closest before/at the drop
      double chosen = 1e18;
      for (double cue : s.cue_ms) {
        const double before = (ds - cue) / p.bar_ms;
        if (before < -0.5 || before > 16.5) continue;
        if (std::fabs(before) < std::fabs(chosen)) chosen = before;
      }
      if (std::fabs(chosen) < 1e17) {
        sum_before += std::round(chosen);
        h.drop_samples++;
      }
    }
  }
  h.mean_bars_before_drop = h.drop_samples ? sum_before / h.drop_samples : 0.0;
  return h;
}

// ---------------------------------------------------------------- cue suggestions
namespace {

struct Cand {
  std::string type, name;
  int color = 0;
  double prio = 0, conf = 0;
  std::size_t sec = 0;
  double pos = 0;
  int bar = -1;
  int shift = 0;
  double anchor = 0;
};

}  // namespace

std::vector<CueSuggestion> suggest_cues(const PhraseAnalysis& p, double first_audible_ms, double last_audible_ms,
                                        const CueOptions& opt) {
  std::vector<CueSuggestion> out;
  if (!p.ok || p.sections.empty() || !(last_audible_ms > first_audible_ms)) return out;
  const std::size_t n = p.sections.size();
  const double bar_ms = p.bar_ms > 0 ? p.bar_ms : 2000.0;
  const int mix_out_bars = opt.mix_out_bars <= 8 ? 8 : (opt.mix_out_bars >= 32 ? 32 : 16);
  auto pos_of_bar = [&](int bar) { return p.bar_starts_ms[static_cast<std::size_t>(bar)]; };
  auto section_bars_left = [&](std::size_t i) {
    return p.gridded ? double(p.total_bars - p.sections[i].start_bar) : (last_audible_ms - p.sections[i].start_ms) / bar_ms;
  };
  auto high_label = [&](std::size_t i) { return is_drop_label(p.sections[i].label); };

  std::vector<Cand> cands;
  auto add = [&](const char* type, const char* name, int color, double prio, std::size_t sec, double conf) {
    Cand c;
    c.type = type;
    c.name = name;
    c.color = color;
    c.prio = prio;
    c.sec = sec;
    c.conf = conf;
    c.pos = p.sections[sec].start_ms;
    c.bar = p.sections[sec].start_bar;
    c.anchor = c.pos;
    cands.push_back(c);
  };

  add("load", "Load", 0, 0.80, 0, 0.9);
  if (p.gridded)
    for (std::size_t i = 0; i < n; ++i)
      if (p.sections[i].start_ms >= first_audible_ms - 0.5 * bar_ms && p.sections[i].kick >= 0.7 && p.sections[i].bars >= 4) {
        add("mix_in", "Mix in", 1, 0.97, i, 0.85);
        break;
      }
  std::size_t first_drop = n, last_drop = n, brk = n, outro = n;
  for (std::size_t i = 0; i < n; ++i) {
    if (first_drop == n && high_label(i) && p.sections[i].energy >= 6) first_drop = i;
    if (high_label(i) && p.sections[i].energy >= 6) last_drop = i;
    if (brk == n && p.sections[i].label == "breakdown") brk = i;
    if (p.sections[i].label == "outro") outro = i;
  }
  if (first_drop < n) add("first_drop", "Drop", 2, 0.95, first_drop, p.sections[first_drop].label_confidence);
  if (brk < n) add("breakdown", "Breakdown", 3, 0.85, brk, p.sections[brk].label_confidence);
  if (last_drop < n && last_drop != first_drop) add("last_drop", "Last drop", 4, 0.78, last_drop, p.sections[last_drop].label_confidence);
  // mix-out: the final phrase whose energy falls, at least N bars before the end, with nothing high after it
  {
    std::size_t pick = n;
    for (std::size_t j = n; j-- > 1;) {
      if (section_bars_left(j) < double(mix_out_bars)) continue;
      if (!(p.sections[j].energy < p.sections[j - 1].energy)) continue;
      bool high_after = false;
      for (std::size_t k = j; k < n; ++k) high_after = high_after || (high_label(k) && p.sections[k].energy >= 6);
      if (high_after) continue;
      pick = j;
      break;
    }
    if (pick < n) {
      add("mix_out", "Mix out", 5, 0.90, pick, 0.8);
    } else if (p.gridded) {
      // no suitable phrase start: use the bar N before the end, rounded down to a multiple of four bars
      int bar = std::max(0, p.total_bars - mix_out_bars);
      bar -= bar % 4;
      Cand c;
      c.type = "mix_out";
      c.name = "Mix out";
      c.color = 5;
      c.prio = 0.88;
      c.conf = 0.4;
      c.bar = bar;
      c.pos = pos_of_bar(bar);
      c.anchor = c.pos;
      for (std::size_t i = 0; i < n; ++i)
        if (p.sections[i].start_bar <= bar) c.sec = i;
      cands.push_back(c);
    }
  }
  if (outro < n) add("outro", "Outro", 6, 0.50, outro, p.sections[outro].label_confidence);

  // habit bias
  if (opt.use_habit && opt.habit_strength > 0 && p.gridded) {
    const CueHabit& h = opt.habit;
    int mode = 0;
    double share = 0;
    if (h.samples >= opt.habit_min_samples) {
      int best = 0;
      for (int i = 0; i < 33; ++i)
        if (h.bar_offset_hist[static_cast<std::size_t>(i)] > h.bar_offset_hist[static_cast<std::size_t>(best)]) best = i;
      mode = best - 16;
      share = double(h.bar_offset_hist[static_cast<std::size_t>(best)]) / double(h.samples);
    }
    for (Cand& c : cands) {
      if (c.type == "load" || c.bar < 0) continue;
      int shift = 0;
      if ((c.type == "first_drop" || c.type == "last_drop") && h.drop_samples >= opt.habit_min_samples)
        shift = -static_cast<int>(std::lround(opt.habit_strength * h.mean_bars_before_drop));
      else if (share >= 0.4 && mode != 0)
        shift = static_cast<int>(std::lround(opt.habit_strength * mode));
      if (shift == 0) continue;
      const int nbar = c.bar + shift;
      if (nbar < 0 || nbar >= p.total_bars) continue;
      const double np = pos_of_bar(nbar);
      if (np < first_audible_ms - 1.0 || np >= last_audible_ms - 0.25 * bar_ms) continue;
      c.bar = nbar;
      c.pos = np;
      c.shift = shift;
    }
  }

  // drop anything outside the audible range, then resolve collisions by priority (deterministic: ties by position)
  std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    if (a.prio != b.prio) return a.prio > b.prio;
    return a.pos < b.pos;
  });
  std::vector<Cand> kept;
  for (const Cand& c : cands) {
    if (c.pos < first_audible_ms - 1.0 || c.pos >= last_audible_ms - 0.25 * bar_ms) continue;
    bool dup = false;
    for (Cand& k : kept)
      if (std::fabs(k.pos - c.pos) < bar_ms) {
        dup = true;
        if (k.type == "mix_in" && c.type == "load") k.name = "Load / Mix in";
      }
    if (!dup) kept.push_back(c);
  }
  int rank = 1;
  const int max_hot = std::max(0, opt.max_hot_cues);
  std::vector<double> used;
  for (const Cand& c : kept) {
    if (rank > max_hot) break;
    CueSuggestion s;
    s.position_ms = c.pos;
    s.type = c.type;
    s.name = c.name;
    s.color = c.color;
    s.rank = rank++;
    s.memory = false;
    s.confidence = c.conf;
    s.bar = c.bar;
    s.anchor_ms = c.anchor;
    s.bar_shift = c.shift;
    out.push_back(s);
    used.push_back(c.pos);
  }
  if (opt.memory_cues) {
    for (std::size_t i = 1; i < n; ++i) {
      const double pos = p.sections[i].start_ms;
      if (pos < first_audible_ms - 1.0 || pos >= last_audible_ms - 0.25 * bar_ms) continue;
      bool dup = false;
      for (double u : used) dup = dup || std::fabs(u - pos) < bar_ms;
      if (dup) continue;
      CueSuggestion s;
      s.position_ms = pos;
      s.type = "memory";
      s.name = p.sections[i].label;
      s.color = 7;
      s.rank = rank++;
      s.memory = true;
      s.confidence = p.sections[i].boundary_confidence;
      s.bar = p.sections[i].start_bar;
      s.anchor_ms = pos;
      out.push_back(s);
      used.push_back(pos);
    }
  }
  return out;
}

std::vector<CueSuggestion> suggest_cues(const AnalysisResult& r, const CueOptions& opt) {
  return suggest_cues(r.phrases, r.first_audible_ms, r.last_audible_ms, opt);
}

}  // namespace deckhand
