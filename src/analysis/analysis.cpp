#include "deckhand/analysis/analysis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>

#include "deckhand/log/log.hpp"

namespace deckhand {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinBeatSeconds = 6.0;
constexpr double kMinKeySeconds = 3.0;

double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// ---------------------------------------------------------------- FFT

// Iterative radix-2 complex FFT (forward), in place.
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
  std::size_t size() const { return n_; }

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
      for (std::size_t i = 0; i < n_; i += len) {
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
  }

  // Two real frames were packed as re + i*im and transformed: magnitudes of both spectra, bins 0..n/2.
  void split_magnitudes(const float* re, const float* im, float* ma, float* mb) const {
    for (std::size_t k = 0; k <= n_ / 2; ++k) {
      const std::size_t k2 = (n_ - k) & (n_ - 1);
      const float r1 = re[k], i1 = im[k], r2 = re[k2], i2 = im[k2];
      ma[k] = 0.5f * std::hypot(r1 + r2, i1 - i2);
      mb[k] = 0.5f * std::hypot(i1 + i2, r1 - r2);
    }
  }

 private:
  std::size_t n_;
  std::vector<std::uint32_t> rev_;
  std::vector<float> cs_, sn_;
};

// ---------------------------------------------------------------- preparation

std::vector<float> make_mono(const float* pcm, std::uint64_t frames) {
  std::vector<float> m(static_cast<std::size_t>(frames));
  for (std::size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (pcm[2 * i] + pcm[2 * i + 1]);
  return m;
}

// Half-band low-pass (31 taps, Hann-windowed sinc, cutoff = output Nyquist) and decimate by two.
std::vector<float> decimate2(const std::vector<float>& x) {
  constexpr int kHalf = 15;
  double h[kHalf + 1] = {};  // h[t] for odd t; h[0] = centre
  h[0] = 0.5;
  double sum = 0.5;
  for (int t = 1; t <= kHalf; t += 2) {
    const double s = (((t - 1) / 2) % 2 == 0 ? 1.0 : -1.0) / (kPi * t);
    const double w = 0.5 + 0.5 * std::cos(kPi * t / (kHalf + 1));
    h[t] = s * w;
    sum += 2 * h[t];
  }
  for (double& v : h) v /= sum;
  const std::size_t n = x.size(), m = n / 2;
  std::vector<float> out(m);
  for (std::size_t i = 0; i < m; ++i) {
    const std::ptrdiff_t c = static_cast<std::ptrdiff_t>(2 * i);
    double acc = h[0] * x[c];
    if (c >= kHalf && c + kHalf < static_cast<std::ptrdiff_t>(n)) {
      for (int t = 1; t <= kHalf; t += 2) acc += h[t] * (double(x[c - t]) + double(x[c + t]));
    } else {
      for (int t = 1; t <= kHalf; t += 2) {
        if (c - t >= 0) acc += h[t] * x[c - t];
        if (c + t < static_cast<std::ptrdiff_t>(n)) acc += h[t] * x[c + t];
      }
    }
    out[i] = static_cast<float>(acc);
  }
  return out;
}

struct Prepared {
  std::vector<float> onset;   // mono, <= ~24 kHz
  double onset_rate = 0;
  std::vector<float> tonal;   // mono, ~5-6.5 kHz
  double tonal_rate = 0;
  double duration_s = 0;
};

Prepared prepare(const float* pcm, std::uint64_t frames, std::uint32_t rate) {
  Prepared p;
  p.duration_s = double(frames) / double(rate);
  std::vector<float> x = make_mono(pcm, frames);
  double r = rate;
  while (r > 24500.0 && x.size() > 64) {
    x = decimate2(x);
    r *= 0.5;
  }
  p.onset_rate = r;
  p.onset = std::move(x);
  std::vector<float> y;
  const std::vector<float>* cur = &p.onset;
  while (r > 6500.0 && cur->size() > 64) {
    y = decimate2(*cur);
    cur = &y;
    r *= 0.5;
  }
  p.tonal_rate = r;
  p.tonal = (cur == &p.onset) ? p.onset : std::move(y);
  return p;
}

// ---------------------------------------------------------------- onset strength

struct Onset {
  std::vector<float> env;  // normalised onset strength, one value per hop
  std::vector<float> low;  // low-band (<150 Hz) magnitude per hop
  double hop_s = 0.01;
  double fps = 100;
};

Onset compute_onset(const std::vector<float>& x, double rate) {
  Onset od;
  const std::size_t N = rate <= 24500.0 ? 512 : 1024;
  const std::size_t hop = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(rate * 0.010)));
  od.hop_s = double(hop) / rate;
  od.fps = rate / double(hop);
  if (x.size() < N) return od;
  const std::size_t nf = x.size() / hop + 1;
  const double binhz = rate / double(N);
  const std::size_t kmin = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(30.0 / binhz)));
  const std::size_t kmax = std::min<std::size_t>(N / 2, static_cast<std::size_t>(10000.0 / binhz));
  const std::size_t klow = std::max<std::size_t>(1, static_cast<std::size_t>(150.0 / binhz));

  Fft fft(N);
  std::vector<float> win(N);
  double wsum = 0;
  for (std::size_t i = 0; i < N; ++i) {
    win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(N)));
    wsum += win[i];
  }
  const float norm = static_cast<float>(2.0 / wsum);

  std::vector<float> re(N), im(N), ma(N / 2 + 1), mb(N / 2 + 1), prev(N / 2 + 1, 0.f);
  std::vector<float> flux(nf, 0.f);
  od.low.assign(nf, 0.f);

  auto frame_stats = [&](const float* mags, std::size_t f) {
    double fl = 0, lo = 0;
    for (std::size_t k = kmin; k <= kmax; ++k) {
      const float L = std::log1p(1000.f * mags[k] * norm);
      if (f > 0) {
        const float d = L - prev[k];
        if (d > 0) fl += d;
      }
      prev[k] = L;
    }
    for (std::size_t k = 1; k <= klow; ++k) lo += mags[k] * norm;
    flux[f] = static_cast<float>(fl);
    od.low[f] = static_cast<float>(lo);
  };
  auto fill = [&](std::size_t f, float* dst) {
    const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(f * hop) - static_cast<std::ptrdiff_t>(N / 2);
    for (std::size_t i = 0; i < N; ++i) {
      const std::ptrdiff_t idx = start + static_cast<std::ptrdiff_t>(i);
      dst[i] = (idx >= 0 && idx < static_cast<std::ptrdiff_t>(x.size())) ? x[idx] * win[i] : 0.f;
    }
  };
  for (std::size_t f = 0; f < nf; f += 2) {
    fill(f, re.data());
    if (f + 1 < nf) fill(f + 1, im.data());
    else std::fill(im.begin(), im.end(), 0.f);
    fft.forward(re.data(), im.data());
    fft.split_magnitudes(re.data(), im.data(), ma.data(), mb.data());
    frame_stats(ma.data(), f);
    if (f + 1 < nf) frame_stats(mb.data(), f + 1);
  }

  const double mean_flux = std::accumulate(flux.begin(), flux.end(), 0.0) / double(nf);
  if (mean_flux < 0.02) return od;  // nothing happening

  // Remove the slowly varying part (local mean over about +-0.3 s), keep positive excursions.
  const std::size_t w = static_cast<std::size_t>(0.3 * od.fps);
  std::vector<double> pre(nf + 1, 0.0);
  for (std::size_t i = 0; i < nf; ++i) pre[i + 1] = pre[i] + flux[i];
  od.env.assign(nf, 0.f);
  double ss = 0;
  for (std::size_t i = 0; i < nf; ++i) {
    const std::size_t a = i > w ? i - w : 0, b = std::min(nf, i + w + 1);
    const double mean = (pre[b] - pre[a]) / double(b - a);
    const double v = std::max(0.0, double(flux[i]) - mean);
    od.env[i] = static_cast<float>(v);
    ss += v * v;
  }
  const double rms = std::sqrt(ss / double(nf));
  if (rms < 1e-6) {
    od.env.clear();
    return od;
  }
  for (float& v : od.env) v = static_cast<float>(v / rms);
  return od;
}

// ---------------------------------------------------------------- tempo

// Global tempo hint from a comb-filtered autocorrelation of the onset envelope.
double tempo_from_onsets(const std::vector<float>& env, double fps, const AnalysisOptions& o) {
  const std::size_t n = env.size();
  const double minb = std::max(30.0, o.min_bpm), maxb = std::min(300.0, o.max_bpm);
  if (n < 64 || minb >= maxb) return 0;
  std::size_t maxlag = static_cast<std::size_t>(std::ceil(4.0 * fps * 60.0 / minb)) + 3;
  maxlag = std::min(maxlag, n / 2);
  if (maxlag < 8) return 0;
  double mean = 0;
  for (float v : env) mean += v;
  mean /= double(n);
  std::vector<double> e(n);
  for (std::size_t i = 0; i < n; ++i) e[i] = env[i] - mean;
  std::vector<double> ac(maxlag + 2, 0.0);
  for (std::size_t l = 1; l <= maxlag; ++l) {
    double s = 0;
    for (std::size_t t = 0; t + l < n; ++t) s += e[t] * e[t + l];
    ac[l] = s / double(n - l);
  }
  auto acf = [&](double lag) {
    if (lag < 1.0 || lag >= double(maxlag)) return 0.0;
    const std::size_t i = static_cast<std::size_t>(lag);
    const double f = lag - double(i);
    return ac[i] * (1 - f) + ac[i + 1] * f;
  };
  static const double kWeights[4] = {1.0, 0.6, 0.4, 0.3};
  double best = -1e30, bestbpm = 0, sum = 0;
  int count = 0;
  for (double bpm = minb; bpm <= maxb + 1e-9; bpm += 0.2) {
    const double lag = fps * 60.0 / bpm;
    double s = 0;
    for (int m = 1; m <= 4; ++m) s += kWeights[m - 1] * acf(lag * m);
    const double oct = std::log2(bpm / 122.0) / 1.2;
    double prior = std::exp(-0.5 * oct * oct);
    if (bpm < o.preferred_min_bpm || bpm > o.preferred_max_bpm) prior *= 0.8;
    const double score = s * prior;
    sum += s;
    ++count;
    if (score > best) {
      best = score;
      bestbpm = bpm;
    }
  }
  if (best <= 0 || bestbpm <= 0) return 0;
  // Fold into the preferred range.
  while (bestbpm < o.preferred_min_bpm && bestbpm * 2 <= maxb) bestbpm *= 2;
  while (bestbpm > o.preferred_max_bpm && bestbpm / 2 >= minb) bestbpm /= 2;
  (void)sum;
  (void)count;
  // A pulse at twice the rate that is as strong as the beat itself: take the faster octave.
  const double lag = fps * 60.0 / bestbpm;
  auto peak_near = [&](double l) { return std::max({acf(l - 1.0), acf(l - 0.5), acf(l), acf(l + 0.5), acf(l + 1.0)}); };
  if (bestbpm * 2 <= o.preferred_max_bpm && peak_near(lag) > 0 && peak_near(lag * 0.5) >= 0.75 * peak_near(lag))
    bestbpm *= 2;
  return bestbpm;
}

// Dynamic-programming beat tracker (cumulative score with a log-interval tempo penalty).
std::vector<int> dp_beats(const std::vector<float>& env, double P) {
  const int n = static_cast<int>(env.size());
  const int lo = std::max(1, static_cast<int>(std::lround(0.5 * P)));
  const int hi = std::max(lo + 1, static_cast<int>(std::lround(2.0 * P)));
  std::vector<double> pen(static_cast<std::size_t>(hi) + 1, 0.0);
  for (int t = lo; t <= hi; ++t) {
    const double l = std::log(double(t) / P);
    pen[t] = -100.0 * l * l;
  }
  std::vector<double> score(n);
  std::vector<int> back(n, -1);
  for (int t = 0; t < n; ++t) {
    double best = -1e30;
    int bi = -1;
    for (int tau = lo; tau <= hi; ++tau) {
      const int p = t - tau;
      if (p < 0) break;
      const double v = score[p] + pen[tau];
      if (v > best) {
        best = v;
        bi = p;
      }
    }
    score[t] = env[t];
    if (bi >= 0 && best > 0) {
      score[t] += best;
      back[t] = bi;
    }
  }
  int end = std::max(0, n - hi);
  for (int t = end; t < n; ++t)
    if (score[t] > score[end]) end = t;
  std::vector<int> beats;
  for (int t = end; t >= 0; t = back[t]) {
    beats.push_back(t);
    if (back[t] < 0) break;
  }
  std::reverse(beats.begin(), beats.end());
  return beats;
}

// Sub-hop onset time: where short-term energy jumps most steeply near `t_center` (seconds).
double refine_onset(const std::vector<float>& x, double rate, double t_center, double half_window_s) {
  const std::ptrdiff_t W = std::max<std::ptrdiff_t>(8, static_cast<std::ptrdiff_t>(0.0022 * rate));
  const std::ptrdiff_t c = static_cast<std::ptrdiff_t>(std::lround(t_center * rate));
  const std::ptrdiff_t hw = static_cast<std::ptrdiff_t>(half_window_s * rate);
  const std::ptrdiff_t lo = c - hw, hi = c + hw;
  if (lo - W < 0 || hi + W >= static_cast<std::ptrdiff_t>(x.size())) return t_center;
  const std::ptrdiff_t m = hi - lo + 2 * W + 1;
  std::vector<double> P(static_cast<std::size_t>(m) + 1, 0.0);
  for (std::ptrdiff_t i = 0; i < m; ++i) {
    const double v = x[lo - W + i];
    P[i + 1] = P[i] + v * v;
  }
  const double eps = 1e-3 * (P[m] / double(m)) * double(W) + 1e-12;
  std::vector<double> d(static_cast<std::size_t>(hi - lo) + 1);
  double dmax = -1e30;
  for (std::ptrdiff_t j = lo; j <= hi; ++j) {
    const std::ptrdiff_t q = j - lo + W;  // index of j in P
    const double fwd = P[q + W] - P[q], bwd = P[q] - P[q - W];
    d[j - lo] = std::log((fwd + eps) / (bwd + eps));
    dmax = std::max(dmax, d[j - lo]);
  }
  if (dmax < 0.7) return t_center;
  double sj = 0, sw = 0;
  for (std::ptrdiff_t j = lo; j <= hi; ++j) {
    if (d[j - lo] >= 0.9 * dmax) {
      sj += double(j);
      sw += 1;
    }
  }
  return (sj / sw + 0.5 * double(W)) / rate;
}

// ---------------------------------------------------------------- beat grid fitting

struct Pt {
  double k;  // beat number
  double t;  // seconds
};

struct Line {
  double a = 0;      // time at k = k_ref
  double T = 0;      // period
  double rms = 0;    // residual rms of the inliers
  double frac = 0;   // inlier fraction
  std::size_t inliers = 0;
};

// Least squares over the points flagged in `use`, about k_ref.
bool ols(const std::vector<Pt>& p, std::size_t lo, std::size_t hi, const std::vector<char>& use,
         double k_ref, double& a, double& T) {
  double n = 0, sk = 0, st = 0, skk = 0, skt = 0;
  for (std::size_t i = lo; i < hi; ++i) {
    if (!use[i]) continue;
    const double k = p[i].k - k_ref, t = p[i].t;
    n += 1;
    sk += k;
    st += t;
    skk += k * k;
    skt += k * t;
  }
  if (n < 3) return false;
  const double sxx = skk - sk * sk / n;
  if (sxx < 1e-9) return false;
  T = (skt - sk * st / n) / sxx;
  a = (st - T * sk) / n;
  return T > 0;
}

Line robust_fit(const std::vector<Pt>& p, std::size_t lo, std::size_t hi, double reject_s) {
  Line L;
  if (hi <= lo) return L;
  const double k_ref = p[lo].k;
  std::vector<char> use(p.size(), 0);
  for (std::size_t i = lo; i < hi; ++i) use[i] = 1;
  double a = 0, T = 0;
  for (int iter = 0; iter < 6; ++iter) {
    if (!ols(p, lo, hi, use, k_ref, a, T)) return L;
    bool changed = false;
    for (std::size_t i = lo; i < hi; ++i) {
      const char u = std::fabs(p[i].t - (a + T * (p[i].k - k_ref))) < reject_s ? 1 : 0;
      if (u != use[i]) changed = true;
      use[i] = u;
    }
    if (!changed) break;
  }
  if (!ols(p, lo, hi, use, k_ref, a, T)) return L;
  double ss = 0;
  std::size_t cnt = 0;
  for (std::size_t i = lo; i < hi; ++i) {
    if (!use[i]) continue;
    const double r = p[i].t - (a + T * (p[i].k - k_ref));
    ss += r * r;
    ++cnt;
  }
  L.a = a;
  L.T = T;
  L.inliers = cnt;
  L.rms = cnt ? std::sqrt(ss / double(cnt)) : 1.0;
  L.frac = double(cnt) / double(hi - lo);
  return L;
}

std::vector<Pt> clean_points(const std::vector<Pt>& p) {
  if (p.size() < 5) return p;
  std::vector<Pt> out;
  out.push_back(p.front());
  for (std::size_t i = 1; i + 1 < p.size(); ++i) {
    const double pred = p[i - 1].t + (p[i + 1].t - p[i - 1].t) * (p[i].k - p[i - 1].k) /
                                         (p[i + 1].k - p[i - 1].k);
    if (std::fabs(p[i].t - pred) <= 0.035) out.push_back(p[i]);
  }
  out.push_back(p.back());
  return out;
}

// Binary segmentation of (k, t) into constant-tempo runs, using prefix sums for exact OLS costs.
struct Segmenter {
  const std::vector<Pt>& p;
  std::vector<double> sk, st, skk, skt, stt;
  std::vector<std::pair<std::size_t, std::size_t>> leaves;
  static constexpr std::size_t kMinLen = 12;
  static constexpr std::size_t kMaxLeaves = 48;

  explicit Segmenter(const std::vector<Pt>& pts) : p(pts) {
    const std::size_t n = p.size();
    sk.assign(n + 1, 0);
    st = skk = skt = stt = sk;
    const double t0 = p.empty() ? 0 : p[0].t, k0 = p.empty() ? 0 : p[0].k;
    for (std::size_t i = 0; i < n; ++i) {
      const double k = p[i].k - k0, t = p[i].t - t0;
      sk[i + 1] = sk[i] + k;
      st[i + 1] = st[i] + t;
      skk[i + 1] = skk[i] + k * k;
      skt[i + 1] = skt[i] + k * t;
      stt[i + 1] = stt[i] + t * t;
    }
  }
  double sse(std::size_t lo, std::size_t hi) const {
    const double n = double(hi - lo);
    const double a = sk[hi] - sk[lo], b = st[hi] - st[lo], c = skk[hi] - skk[lo];
    const double d = skt[hi] - skt[lo], e = stt[hi] - stt[lo];
    const double sxx = c - a * a / n, sxy = d - a * b / n, syy = e - b * b / n;
    if (sxx < 1e-9) return std::max(0.0, syy);
    return std::max(0.0, syy - sxy * sxy / sxx);
  }
  void run(std::size_t lo, std::size_t hi) {
    const double whole = sse(lo, hi);
    const std::size_t n = hi - lo;
    const double rms = std::sqrt(whole / double(n));
    if (rms <= 0.012 || n < 2 * kMinLen || leaves.size() + 2 > kMaxLeaves) {
      leaves.emplace_back(lo, hi);
      return;
    }
    double best = 1e30;
    std::size_t bs = 0;
    for (std::size_t s = lo + kMinLen; s + kMinLen <= hi; ++s) {
      const double tot = sse(lo, s) + sse(s, hi);
      if (tot < best) {
        best = tot;
        bs = s;
      }
    }
    if (bs == 0 || best > 0.5 * whole) {
      leaves.emplace_back(lo, hi);
      return;
    }
    run(lo, bs);
    run(bs, hi);
  }
};

double snap_bpm(double bpm) {
  double s = std::round(bpm * 100.0) / 100.0;
  if (std::fabs(s - std::round(s)) < 0.05) s = std::round(s);
  return s;
}

struct Section {
  double a = 0;  // first beat, seconds
  double T = 0;  // period, seconds
  double bpm = 0;
  std::size_t first_index = 0;
  std::size_t count = 0;
};

double median_of(std::vector<double> v) {
  if (v.empty()) return 0;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
  return v[v.size() / 2];
}

// ---------------------------------------------------------------- chroma / key

struct Chroma {
  std::vector<std::array<float, 12>> frames;
  std::vector<float> energy;  // per-frame total weight
  double hop_s = 0.186;
  double tuning = 0;  // semitones
};

Chroma compute_chroma(const std::vector<float>& x, double rate) {
  Chroma ch;
  const std::size_t N = rate <= 8200.0 ? 2048 : 4096;
  const std::size_t hop = N / 2;
  ch.hop_s = double(hop) / rate;
  if (x.size() < N) return ch;
  const std::size_t nf = x.size() / hop + 1;
  const double binhz = rate / double(N);
  const std::size_t kmin = std::max<std::size_t>(2, static_cast<std::size_t>(std::ceil(80.0 / binhz)));
  const std::size_t kmax = std::min<std::size_t>(N / 2 - 2, static_cast<std::size_t>(std::min(2000.0, 0.45 * rate) / binhz));
  Fft fft(N);
  std::vector<float> win(N);
  for (std::size_t i = 0; i < N; ++i)
    win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(N)));
  const float norm = 4.0f / float(N);

  struct Peak {
    float midi, w;
    std::uint32_t f;
  };
  std::vector<Peak> peaks;
  std::vector<float> re(N), im(N), ma(N / 2 + 1), mb(N / 2 + 1);
  auto fill = [&](std::size_t f, float* dst) {
    const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(f * hop) - static_cast<std::ptrdiff_t>(N / 2);
    for (std::size_t i = 0; i < N; ++i) {
      const std::ptrdiff_t idx = start + static_cast<std::ptrdiff_t>(i);
      dst[i] = (idx >= 0 && idx < static_cast<std::ptrdiff_t>(x.size())) ? x[idx] * win[i] : 0.f;
    }
  };
  auto pick = [&](const float* m, std::size_t f) {
    float mx = 0;
    for (std::size_t k = kmin; k <= kmax; ++k) mx = std::max(mx, m[k] * norm);
    if (mx < 2e-4f) return;
    for (std::size_t k = kmin; k <= kmax; ++k) {
      const float v = m[k] * norm;
      if (v < 0.05f * mx || !(m[k] > m[k - 1] && m[k] >= m[k + 1])) continue;
      const float a = m[k - 1], b = m[k], c = m[k + 1];
      const float den = a - 2 * b + c;
      const float delta = den != 0 ? std::clamp(0.5f * (a - c) / den, -0.5f, 0.5f) : 0.f;
      const double freq = (double(k) + delta) * binhz;
      const double midi = 69.0 + 12.0 * std::log2(freq / 440.0);
      peaks.push_back({static_cast<float>(midi), std::sqrt(v), static_cast<std::uint32_t>(f)});
    }
  };
  for (std::size_t f = 0; f < nf; f += 2) {
    fill(f, re.data());
    if (f + 1 < nf) fill(f + 1, im.data());
    else std::fill(im.begin(), im.end(), 0.f);
    fft.forward(re.data(), im.data());
    fft.split_magnitudes(re.data(), im.data(), ma.data(), mb.data());
    pick(ma.data(), f);
    if (f + 1 < nf) pick(mb.data(), f + 1);
  }
  // Tuning: circular mean of the deviation from the semitone grid.
  double cr = 0, ci = 0, wsum = 0;
  for (const Peak& p : peaks) {
    cr += p.w * std::cos(2.0 * kPi * p.midi);
    ci += p.w * std::sin(2.0 * kPi * p.midi);
    wsum += p.w;
  }
  if (wsum > 0 && std::hypot(cr, ci) / wsum > 0.25) ch.tuning = std::atan2(ci, cr) / (2.0 * kPi);
  ch.frames.assign(nf, std::array<float, 12>{});
  ch.energy.assign(nf, 0.f);
  for (const Peak& p : peaks) {
    const int pc = ((static_cast<int>(std::lround(p.midi - ch.tuning)) % 12) + 12) % 12;
    ch.frames[p.f][pc] += p.w;
    ch.energy[p.f] += p.w;
  }
  return ch;
}

const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

KeyResult key_from_chroma(const Chroma& ch) {
  KeyResult r;
  static const double kMajor[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
  static const double kMinor[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
  const std::size_t nf = ch.frames.size();
  const std::size_t block = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(4.0 / ch.hop_s)));
  if (nf < 2) {
    r.error = "not enough audio for key detection";
    return r;
  }
  struct Block {
    std::array<double, 12> c{};
    double energy = 0;
  };
  std::vector<Block> blocks;
  for (std::size_t s = 0; s < nf; s += block) {
    const std::size_t e = std::min(nf, s + block);
    if (e - s < block / 2 && !blocks.empty()) break;
    Block b;
    for (std::size_t f = s; f < e; ++f) {
      if (ch.energy[f] <= 0) continue;
      for (int i = 0; i < 12; ++i) b.c[i] += ch.frames[f][i] / ch.energy[f];
      b.energy += ch.energy[f];
    }
    blocks.push_back(b);
  }
  double maxe = 0;
  for (const Block& b : blocks) maxe = std::max(maxe, b.energy);
  if (maxe <= 0) {
    r.error = "no tonal content";
    return r;
  }
  std::array<double, 24> score{};
  int used = 0;
  for (const Block& b : blocks) {
    if (b.energy < 0.05 * maxe) continue;
    double mean = 0;
    for (double v : b.c) mean += v;
    mean /= 12;
    double cc = 0;
    for (double v : b.c) cc += (v - mean) * (v - mean);
    if (cc < 1e-12) continue;
    ++used;
    for (int key = 0; key < 24; ++key) {
      const double* prof = key < 12 ? kMajor : kMinor;
      const int tonic = key % 12;
      double pm = 0;
      for (int i = 0; i < 12; ++i) pm += prof[i];
      pm /= 12;
      double num = 0, pp = 0;
      for (int i = 0; i < 12; ++i) {
        const double pv = prof[((i - tonic) % 12 + 12) % 12] - pm;
        num += (b.c[i] - mean) * pv;
        pp += pv * pv;
      }
      score[key] += num / std::sqrt(cc * pp);
    }
  }
  if (used == 0) {
    r.error = "no tonal content";
    return r;
  }
  int best = 0;
  for (int k = 1; k < 24; ++k)
    if (score[k] > score[best]) best = k;
  double second = -1e30;
  for (int k = 0; k < 24; ++k)
    if (k != best) second = std::max(second, score[k]);
  const double bestc = score[best] / used, secondc = second / used;
  r.ok = true;
  r.key_index = best;
  r.name = key_name(best);
  r.wheel = key_wheel(best);
  r.confidence = clamp01((bestc - secondc) / 0.15) * clamp01(bestc / 0.5);
  r.tuning_cents = ch.tuning * 100.0;
  return r;
}

// ---------------------------------------------------------------- downbeats

double mean_range(const std::vector<float>& v, double hop_s, double t0, double t1) {
  if (v.empty()) return 0;
  long a = static_cast<long>(std::ceil(t0 / hop_s)), b = static_cast<long>(std::ceil(t1 / hop_s));
  const long n = static_cast<long>(v.size());
  if (b <= a) b = a + 1;
  a = std::clamp(a, 0L, n - 1);
  b = std::clamp(b, a + 1, n);
  double s = 0;
  for (long i = a; i < b; ++i) s += v[i];
  return s / double(b - a);
}

void zscore(std::vector<double>& v) {
  if (v.empty()) return;
  double m = 0;
  for (double x : v) m += x;
  m /= double(v.size());
  double s = 0;
  for (double x : v) s += (x - m) * (x - m);
  s = std::sqrt(s / double(v.size()));
  for (double& x : v) x = s > 1e-9 ? (x - m) / s : 0.0;
}

void detect_downbeats(BeatGrid& g, const Onset& od, const Chroma& ch) {
  const std::size_t nb = g.beats_ms.size();
  g.downbeat_phase = 0;
  g.downbeat_confidence = 0;
  if (nb < 8) return;
  std::vector<double> fa(nb), fb(nb), fc(nb), fd(nb);
  for (std::size_t j = 0; j < nb; ++j) {
    const double t = g.beats_ms[j] / 1000.0;
    const double T = (j + 1 < nb ? g.beats_ms[j + 1] - g.beats_ms[j] : g.beats_ms[j] - g.beats_ms[j - 1]) / 1000.0;
    const double after = mean_range(od.low, od.hop_s, t, t + 0.45 * T);
    const double before = mean_range(od.low, od.hop_s, t - 0.45 * T, t);
    fa[j] = std::log(after + 1e-4);
    fb[j] = std::log((after + 1e-4) / (before + 1e-4));
    double pk = 0;
    const long i0 = static_cast<long>(std::lround((t - 0.03) / od.hop_s));
    const long i1 = static_cast<long>(std::lround((t + 0.03) / od.hop_s));
    for (long i = i0; i <= i1; ++i)
      if (i >= 0 && i < static_cast<long>(od.env.size())) pk = std::max(pk, double(od.env[i]));
    fc[j] = pk;
    // Harmonic change: chroma of the beat before vs the beat after.
    std::array<double, 12> A{}, B{};
    auto accum = [&](std::array<double, 12>& dst, double t0, double t1) {
      long a = static_cast<long>(std::ceil(t0 / ch.hop_s)), b = static_cast<long>(std::ceil(t1 / ch.hop_s));
      const long n = static_cast<long>(ch.frames.size());
      if (n == 0) return;
      if (b <= a) b = a + 1;
      a = std::clamp(a, 0L, n - 1);
      b = std::clamp(b, a + 1, n);
      for (long i = a; i < b; ++i) {
        if (ch.energy[i] <= 0) continue;
        for (int c = 0; c < 12; ++c) dst[c] += ch.frames[i][c] / ch.energy[i];
      }
    };
    accum(A, t - T, t);
    accum(B, t, t + T);
    double dot = 0, na = 0, nbb = 0;
    for (int c = 0; c < 12; ++c) {
      dot += A[c] * B[c];
      na += A[c] * A[c];
      nbb += B[c] * B[c];
    }
    fd[j] = (na > 0 && nbb > 0) ? 1.0 - dot / std::sqrt(na * nbb) : 0.0;
  }
  zscore(fa);
  zscore(fb);
  zscore(fc);
  zscore(fd);
  double sum[4] = {0, 0, 0, 0};
  int cnt[4] = {0, 0, 0, 0};
  for (std::size_t j = 0; j < nb; ++j) {
    sum[j % 4] += fa[j] + fb[j] + 0.7 * fc[j] + fd[j];
    ++cnt[j % 4];
  }
  double sc[4];
  int best = 0;
  for (int p = 0; p < 4; ++p) {
    sc[p] = sum[p] / std::max(1, cnt[p]);
    if (sc[p] > sc[best]) best = p;
  }
  double second = -1e30;
  for (int p = 0; p < 4; ++p)
    if (p != best) second = std::max(second, sc[p]);
  g.downbeat_phase = best;
  g.downbeat_confidence = clamp01((sc[best] - second) / 1.5) * clamp01(double(nb) / 32.0);
}

// ---------------------------------------------------------------- beats

BeatGrid beats_from(const Prepared& prep, const Chroma& chroma, const AnalysisOptions& opt) {
  BeatGrid g;
  if (prep.duration_s < kMinBeatSeconds) {
    g.error = "track too short for beat analysis";
    return g;
  }
  const Onset od = compute_onset(prep.onset, prep.onset_rate);
  if (od.env.empty()) {
    g.error = "no rhythmic content";
    return g;
  }
  const double hint = tempo_from_onsets(od.env, od.fps, opt);
  if (hint <= 0) {
    g.error = "tempo not found";
    return g;
  }
  const double P = od.fps * 60.0 / hint;
  std::vector<int> bf = dp_beats(od.env, P);
  // Trim weak beats at both ends (intro/outro without a pulse).
  if (bf.size() >= 8) {
    std::vector<double> ev;
    for (int b : bf) ev.push_back(od.env[b]);
    const double thr = 0.2 * median_of(ev);
    std::size_t a = 0, b = bf.size();
    while (b - a > 8 && od.env[bf[a]] < thr) ++a;
    while (b - a > 8 && od.env[bf[b - 1]] < thr) --b;
    bf = std::vector<int>(bf.begin() + static_cast<std::ptrdiff_t>(a), bf.begin() + static_cast<std::ptrdiff_t>(b));
  }
  if (bf.size() < 8) {
    g.error = "too few beats found";
    return g;
  }
  // Beat times: parabolic peak, then sample-accurate energy-onset refinement.
  const double kBias = 0.006;  // spectral flux peaks about N/4 before the attack
  std::vector<double> times;
  for (int b : bf) {
    double d = 0;
    if (b > 0 && b + 1 < static_cast<int>(od.env.size())) {
      const double y0 = od.env[b - 1], y1 = od.env[b], y2 = od.env[b + 1];
      const double den = y0 - 2 * y1 + y2;
      if (den < 0) d = std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
    }
    const double coarse = (double(b) + d) * od.hop_s + kBias;
    times.push_back(refine_onset(prep.onset, prep.onset_rate, coarse, 0.025));
  }
  std::vector<double> dts;
  for (std::size_t i = 1; i < times.size(); ++i) dts.push_back(times[i] - times[i - 1]);
  const double M = median_of(dts);
  if (M <= 0) {
    g.error = "tempo not found";
    return g;
  }
  std::vector<Pt> pts;
  pts.push_back({0.0, times[0]});
  for (std::size_t i = 1; i < times.size(); ++i) {
    const double dk = std::max(1.0, std::round((times[i] - times[i - 1]) / M));
    pts.push_back({pts.back().k + dk, times[i]});
  }

  // Constant tempo if one robust line explains the beats, otherwise segment.
  std::vector<std::pair<std::size_t, std::size_t>> leaves;
  const Line whole = robust_fit(pts, 0, pts.size(), 0.025);
  std::vector<Pt> work;
  if (whole.T > 0 && whole.rms <= 0.012 && whole.frac >= 0.9) {
    work = pts;
    leaves.emplace_back(0, work.size());
  } else {
    work = clean_points(pts);
    Segmenter seg(work);
    seg.run(0, work.size());
    leaves = seg.leaves;
  }

  // Merge neighbours that one line explains, then drop runs too short to be a real tempo section.
  for (bool merged = true; merged && leaves.size() > 1;) {
    merged = false;
    for (std::size_t i = 0; i + 1 < leaves.size(); ++i) {
      const Line J = robust_fit(work, leaves[i].first, leaves[i + 1].second, 0.025);
      if (J.T > 0 && J.rms <= 0.012 && J.frac >= 0.85) {
        leaves[i].second = leaves[i + 1].second;
        leaves.erase(leaves.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        merged = true;
        break;
      }
    }
  }
  for (std::size_t i = 0; i < leaves.size() && leaves.size() > 1;) {
    if (leaves[i].second - leaves[i].first < 16) leaves.erase(leaves.begin() + static_cast<std::ptrdiff_t>(i));
    else ++i;
  }

  std::vector<Section> secs;
  double inl = 0, tot = 0, rms_acc = 0;
  for (const auto& lf : leaves) {
    const Line L = robust_fit(work, lf.first, lf.second, 0.025);
    if (L.T <= 0 || L.inliers < 4) continue;
    Section s;
    s.bpm = snap_bpm(60.0 / L.T);
    if (s.bpm <= 0) continue;
    s.T = 60.0 / s.bpm;
    // Re-centre the intercept for the snapped period over the inliers.
    const double k_ref = work[lf.first].k;
    double acc = 0;
    std::size_t n = 0;
    for (std::size_t i = lf.first; i < lf.second; ++i) {
      if (std::fabs(work[i].t - (L.a + L.T * (work[i].k - k_ref))) >= 0.025) continue;
      acc += work[i].t - s.T * (work[i].k - k_ref);
      ++n;
    }
    s.a = n ? acc / double(n) : L.a;
    inl += double(L.inliers);
    tot += double(lf.second - lf.first);
    rms_acc += L.rms * double(L.inliers);
    secs.push_back(s);
  }
  if (secs.empty()) {
    g.error = "beat grid could not be fitted";
    return g;
  }
  // Extend the first section back to the start of the track.
  {
    Section& s = secs.front();
    const double m = std::floor((s.a + 0.002) / s.T);
    s.a = std::max(0.0, s.a - m * s.T);
  }
  // Generate beats.
  for (std::size_t i = 0; i < secs.size(); ++i) {
    Section& s = secs[i];
    s.first_index = g.beats_ms.size();
    std::size_t n;
    if (i + 1 < secs.size()) {
      n = static_cast<std::size_t>(std::max<long>(1, std::lround((secs[i + 1].a - s.a) / s.T)));
    } else {
      n = static_cast<std::size_t>(std::max(1.0, std::ceil((prep.duration_s - s.a) / s.T)));
    }
    s.count = n;
    for (std::size_t k = 0; k < n; ++k) g.beats_ms.push_back((s.a + double(k) * s.T) * 1000.0);
  }
  detect_downbeats(g, od, chroma);
  double longest = 0;
  for (const Section& s : secs) {
    TempoSection bs;
    bs.first_beat_ms = s.a * 1000.0;
    bs.bpm = s.bpm;
    bs.bar_beat = static_cast<int>((((long(s.first_index) - g.downbeat_phase) % 4) + 4) % 4) + 1;
    g.sections.push_back(bs);
    const double len = double(s.count) * s.T;
    if (len > longest) {
      longest = len;
      g.bpm = s.bpm;
    }
  }
  const double frac = tot > 0 ? inl / tot : 0;
  const double rms = inl > 0 ? rms_acc / inl : 0.03;
  g.confidence = clamp01(frac * (1.0 - rms / 0.03));
  g.ok = true;
  return g;
}

// ---------------------------------------------------------------- loudness

struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  double run(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

// BS.1770 pre-filter (high shelf) and RLB high-pass, derived for any sample rate.
void k_weighting(double fs, Biquad& shelf, Biquad& hp) {
  {
    const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    const double K = std::tan(kPi * f0 / fs);
    const double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
    shelf.b1 = 2.0 * (K * K - Vh) / a0;
    shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
    shelf.a1 = 2.0 * (K * K - 1.0) / a0;
    shelf.a2 = (1.0 - K / Q + K * K) / a0;
  }
  {
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;
    const double K = std::tan(kPi * f0 / fs);
    const double a0 = 1.0 + K / Q + K * K;
    hp.b0 = 1.0;
    hp.b1 = -2.0;
    hp.b2 = 1.0;
    hp.a1 = 2.0 * (K * K - 1.0) / a0;
    hp.a2 = (1.0 - K / Q + K * K) / a0;
  }
}

LoudnessResult loudness_of(const float* pcm, std::uint64_t frames, std::uint32_t rate) {
  LoudnessResult r;
  double peak = 0;
  const std::size_t S = static_cast<std::size_t>(std::lround(0.1 * rate));
  const std::size_t nsb = static_cast<std::size_t>(frames) / S;
  Biquad sl, sr, hl, hr;
  k_weighting(rate, sl, hl);
  k_weighting(rate, sr, hr);
  std::vector<double> sq(nsb, 0.0);
  std::size_t i = 0;
  for (std::size_t b = 0; b < nsb; ++b) {
    double acc = 0;
    for (std::size_t e = i + S; i < e; ++i) {
      const double l = pcm[2 * i], rr = pcm[2 * i + 1];
      peak = std::max(peak, std::max(std::fabs(l), std::fabs(rr)));
      const double yl = hl.run(sl.run(l)), yr = hr.run(sr.run(rr));
      acc += yl * yl + yr * yr;
    }
    sq[b] = acc / double(S);
  }
  for (; i < frames; ++i) peak = std::max(peak, std::max(std::fabs(double(pcm[2 * i])), std::fabs(double(pcm[2 * i + 1]))));
  r.sample_peak = peak;
  r.sample_peak_dbfs = peak > 0 ? 20.0 * std::log10(peak) : -200.0;
  if (nsb < 4) {
    r.error = "not enough audio for loudness";
    return r;
  }
  std::vector<double> z(nsb - 3);
  for (std::size_t j = 0; j < z.size(); ++j) z[j] = 0.25 * (sq[j] + sq[j + 1] + sq[j + 2] + sq[j + 3]);
  const double abs_thr = std::pow(10.0, (-70.0 + 0.691) / 10.0);
  double sum = 0;
  std::size_t n = 0;
  for (double v : z)
    if (v > abs_thr) {
      sum += v;
      ++n;
    }
  if (n == 0) {
    r.error = "silent";
    return r;
  }
  const double rel_thr = std::max(abs_thr, 0.1 * (sum / double(n)));
  sum = 0;
  n = 0;
  for (double v : z)
    if (v > rel_thr) {
      sum += v;
      ++n;
    }
  if (n == 0) {
    r.error = "silent";
    return r;
  }
  r.integrated_lufs = -0.691 + 10.0 * std::log10(sum / double(n));
  r.ok = true;
  return r;
}

bool bad_input(const float* pcm, std::uint64_t frames, std::uint32_t rate) {
  return pcm == nullptr || frames == 0 || rate < 8000;
}

}  // namespace

// ---------------------------------------------------------------- public API

std::string key_name(int k) {
  if (k < 0 || k > 23) return {};
  return std::string(kNames[k % 12]) + (k >= 12 ? "m" : "");
}

std::string key_wheel(int k) {
  if (k < 0 || k > 23) return {};
  const bool minor = k >= 12;
  const int major_tonic = minor ? (k % 12 + 3) % 12 : k;  // relative major shares the number
  const int fifths = (major_tonic * 7) % 12;
  return std::to_string((fifths + 7) % 12 + 1) + (minor ? "A" : "B");
}

double auto_gain_db(const LoudnessResult& l, double target_lufs, double ceiling_dbfs) {
  if (!l.ok) return 0.0;
  double gain = target_lufs - l.integrated_lufs;
  if (l.sample_peak > 0 && l.sample_peak_dbfs + gain > ceiling_dbfs) gain = ceiling_dbfs - l.sample_peak_dbfs;
  return std::clamp(gain, -24.0, 24.0);
}

BeatGrid analyse_beats(const float* pcm, std::uint64_t frames, std::uint32_t rate, const AnalysisOptions& o) {
  BeatGrid g;
  if (bad_input(pcm, frames, rate)) {
    g.error = "invalid input";
    return g;
  }
  try {
    const Prepared p = prepare(pcm, frames, rate);
    if (p.duration_s < kMinBeatSeconds) {
      g.error = "track too short for beat analysis";
      return g;
    }
    const Chroma ch = compute_chroma(p.tonal, p.tonal_rate);
    return beats_from(p, ch, o);
  } catch (...) {
    g = BeatGrid{};
    g.error = "internal error";
    return g;
  }
}

KeyResult detect_key(const float* pcm, std::uint64_t frames, std::uint32_t rate) {
  KeyResult r;
  if (bad_input(pcm, frames, rate)) {
    r.error = "invalid input";
    return r;
  }
  if (double(frames) / rate < kMinKeySeconds) {
    r.error = "track too short for key detection";
    return r;
  }
  try {
    const Prepared p = prepare(pcm, frames, rate);
    return key_from_chroma(compute_chroma(p.tonal, p.tonal_rate));
  } catch (...) {
    r = KeyResult{};
    r.error = "internal error";
    return r;
  }
}

LoudnessResult measure_loudness(const float* pcm, std::uint64_t frames, std::uint32_t rate) {
  LoudnessResult r;
  if (bad_input(pcm, frames, rate)) {
    r.error = "invalid input";
    return r;
  }
  try {
    return loudness_of(pcm, frames, rate);
  } catch (...) {
    r = LoudnessResult{};
    r.error = "internal error";
    return r;
  }
}

AnalysisResult analyse(const float* pcm, std::uint64_t frames, std::uint32_t rate, const AnalysisOptions& o) {
  AnalysisResult r;
  if (bad_input(pcm, frames, rate)) {
    r.error = "invalid input";
    return r;
  }
  r.duration_ms = double(frames) * 1000.0 / double(rate);
  try {
    if (o.beats || o.key) {
      const Prepared p = prepare(pcm, frames, rate);
      const Chroma ch = compute_chroma(p.tonal, p.tonal_rate);
      if (o.beats) r.beats = beats_from(p, ch, o);
      if (o.key) {
        if (p.duration_s < kMinKeySeconds) r.key.error = "track too short for key detection";
        else r.key = key_from_chroma(ch);
      }
    }
    if (o.loudness) r.loudness = loudness_of(pcm, frames, rate);
  } catch (...) {
    r.error = "internal error";
    return r;
  }
  r.ok = true;
  if (o.beats && !r.beats.ok) { r.ok = false; r.error = "beats: " + r.beats.error; }
  else if (o.key && !r.key.ok) { r.ok = false; r.error = "key: " + r.key.error; }
  else if (o.loudness && !r.loudness.ok) { r.ok = false; r.error = "loudness: " + r.loudness.error; }
  DH_LOG(Info, "analysis: %.1f s, bpm %.2f (%zu sections), key %s %s, %.1f LUFS%s%s", r.duration_ms / 1000.0,
         r.beats.bpm, r.beats.sections.size(), r.key.name.c_str(), r.key.wheel.c_str(),
         r.loudness.integrated_lufs, r.ok ? "" : " - ", r.ok ? "" : r.error.c_str());
  return r;
}

}  // namespace deckhand
