#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <thread>

#include "internal.hpp"

namespace deckhand::stems {

const char* stem_name(int s) {
  static const char* n[] = {"vocals", "drums", "bass", "other"};
  return s >= 0 && s < kStemCount ? n[s] : "?";
}
int stem_from_name(const std::string& n) {
  for (int i = 0; i < kStemCount; ++i)
    if (n == stem_name(i)) return i;
  return -1;
}

bool stems_available() {
#if defined(DECKHAND_STEMS_DISABLED)
  return false;
#else
  return true;  // the built-in graph executor (src/stems/graph) is always compiled in
#endif
}
const char* backend_name() { return stems_available() ? "builtin" : "none"; }

// ---- resampler -------------------------------------------------------------------------------------------------
namespace {
double bessel_i0(double x) {
  double sum = 1, term = 1, q = x * x / 4;
  for (int k = 1; k < 60; ++k) {
    term *= q / (double(k) * k);
    sum += term;
    if (term < 1e-14 * sum) break;
  }
  return sum;
}
constexpr double kPi = 3.14159265358979323846;
// Kaiser-windowed sinc, cutoff fc (fraction of the input Nyquist), half-width hw in input samples.
double kernel(double x, double fc, double hw, double beta, double i0b) {
  if (std::fabs(x) >= hw) return 0;
  double s = std::fabs(x) < 1e-12 ? fc : std::sin(kPi * fc * x) / (kPi * x);
  double r = x / hw;
  return s * bessel_i0(beta * std::sqrt(1 - r * r)) / i0b;
}
}  // namespace

std::vector<float> resample(const std::vector<float>& in, int from, int to) {
  if (from <= 0 || to <= 0) return {};
  if (from == to) return in;
  const std::size_t n = in.size();
  const std::size_t m = std::size_t(std::llround(double(n) * to / from));
  std::vector<float> out(m, 0.f);
  if (n == 0) return out;
  const double fc = std::min(1.0, double(to) / from) * 0.97;  // anti-alias cutoff relative to input Nyquist
  const double hw = 24.0 / std::max(fc, 0.05) * 0.5 + 8;       // half-width in input samples
  const double beta = 9.0, i0b = bessel_i0(beta);
  const int taps = int(std::ceil(hw)) * 2 + 2;
  const int g = int(std::gcd(from, to));
  const std::int64_t L = to / g, M = from / g;
  const bool table = L <= 4096;
  std::vector<float> tab;
  if (table) {
    tab.assign(std::size_t(L) * taps, 0.f);
    for (std::int64_t p = 0; p < L; ++p) {
      double frac = double(p) / double(L);  // fractional input position of this phase
      double sum = 0;
      std::vector<double> k(taps);
      for (int t = 0; t < taps; ++t) {
        int off = t - taps / 2 + 1;
        k[std::size_t(t)] = kernel(off - frac, fc, hw, beta, i0b);
        sum += k[std::size_t(t)];
      }
      for (int t = 0; t < taps; ++t) tab[std::size_t(p) * taps + t] = float(k[std::size_t(t)] / sum);
    }
  }
  std::vector<double> tmp(taps);
  for (std::size_t j = 0; j < m; ++j) {
    std::int64_t num = std::int64_t(j) * M;
    std::int64_t base = num / L;
    std::int64_t p = num % L;
    const float* kk = nullptr;
    if (table) {
      kk = &tab[std::size_t(p) * taps];
    } else {
      double frac = double(p) / double(L), sum = 0;
      for (int t = 0; t < taps; ++t) { tmp[std::size_t(t)] = kernel((t - taps / 2 + 1) - frac, fc, hw, beta, i0b); sum += tmp[std::size_t(t)]; }
      for (auto& v : tmp) v /= sum;
    }
    double acc = 0;
    for (int t = 0; t < taps; ++t) {
      std::int64_t idx = base + t - taps / 2 + 1;
      if (idx < 0 || idx >= std::int64_t(n)) continue;
      acc += double(in[std::size_t(idx)]) * (table ? double(kk[t]) : tmp[std::size_t(t)]);
    }
    out[j] = float(acc);
  }
  return out;
}

// ---- separation -------------------------------------------------------------------------------------------------
namespace {

// Closes the mixture exactly: sum(stems) == mix within float rounding.
void make_consistent(const Audio& mix, Stems& st, MixtureMode mode) {
  if (mode == MixtureMode::None) return;
  const std::size_t n = mix.frames();
  std::vector<float>* sc[kStemCount][2];
  for (int s = 0; s < kStemCount; ++s) { sc[s][0] = &st[std::size_t(s)].l; sc[s][1] = &st[std::size_t(s)].r; }
  const std::vector<float>* mc[2] = {&mix.l, &mix.r};

  if (mode == MixtureMode::Proportional) {
    const std::size_t B = 1024;
    const std::size_t nb = (n + B - 1) / B;
    std::vector<float> W(nb * kStemCount);
    for (std::size_t b = 0; b < nb; ++b) {
      double P[kStemCount], tot = 0;
      std::size_t lo = b * B, hi = std::min(n, lo + B);
      for (int s = 0; s < kStemCount; ++s) {
        double e = 0;
        for (int c = 0; c < 2; ++c)
          for (std::size_t i = lo; i < hi; ++i) { double v = (*sc[s][c])[i]; e += v * v; }
        P[s] = e / double(hi - lo) + 1e-10;
        tot += P[s];
      }
      for (int s = 0; s < kStemCount; ++s) W[b * kStemCount + std::size_t(s)] = float(P[s] / tot);
    }
    for (std::size_t i = 0; i < n; ++i) {
      double pos = (double(i) + 0.5) / B - 0.5;
      long b0 = long(std::floor(pos));
      double f = pos - b0;
      long b1 = b0 + 1;
      b0 = std::max(0L, std::min(long(nb) - 1, b0));
      b1 = std::max(0L, std::min(long(nb) - 1, b1));
      for (int c = 0; c < 2; ++c) {
        double sum = 0;
        for (int s = 0; s < kStemCount; ++s) sum += (*sc[s][c])[i];
        double r = double((*mc[c])[i]) - sum;
        for (int s = 0; s < kStemCount; ++s) {
          double w = (1 - f) * W[std::size_t(b0) * kStemCount + std::size_t(s)] + f * W[std::size_t(b1) * kStemCount + std::size_t(s)];
          (*sc[s][c])[i] = float(double((*sc[s][c])[i]) + w * r);
        }
      }
    }
  }
  // "other" closes the sum exactly (also the whole of ToOther mode).
  for (int c = 0; c < 2; ++c)
    for (std::size_t i = 0; i < n; ++i) {
      float partial = (*sc[0][c])[i] + (*sc[1][c])[i];
      partial += (*sc[2][c])[i];
      (*sc[3][c])[i] = (*mc[c])[i] - partial;
    }
}

bool wait_if_paused(const SeparateOptions& o) {
  while (o.pause && o.pause->load(std::memory_order_relaxed)) {
    if (o.cancel && o.cancel->load(std::memory_order_relaxed)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return true;
}

}  // namespace

SeparateResult separate(const Audio& in, StemModel& model, const SeparateOptions& opt) {
  using clock = std::chrono::steady_clock;
  auto t0 = clock::now();
  SeparateResult res;
  auto done = [&](SeparateResult::Status s, std::string e = {}) {
    res.status = s;
    res.error = std::move(e);
    res.seconds = std::chrono::duration<double>(clock::now() - t0).count();
    return res;
  };
#if defined(DECKHAND_STEMS_DISABLED)
  return done(SeparateResult::Status::Failed, "stem separation is disabled in this build");
#endif
  const ModelInfo& mi = model.info();
  if (mi.channels != 2 || mi.segment_frames < 64 || mi.sample_rate <= 0) return done(SeparateResult::Status::Failed, "unsupported model geometry");
  if (in.sample_rate <= 0 || in.l.size() != in.r.size()) return done(SeparateResult::Status::Failed, "invalid input audio");
  const std::size_t n = in.frames();
  for (auto& s : res.stems) s.sample_rate = in.sample_rate;
  if (n == 0) {
    if (opt.progress) opt.progress(1.f);
    return done(SeparateResult::Status::Ok);
  }

  // 1. to the model rate
  const bool rs = in.sample_rate != mi.sample_rate;
  std::vector<float> xl = rs ? resample(in.l, in.sample_rate, mi.sample_rate) : std::vector<float>();
  std::vector<float> xr = rs ? resample(in.r, in.sample_rate, mi.sample_rate) : std::vector<float>();
  const std::vector<float>& L = rs ? xl : in.l;
  const std::vector<float>& R = rs ? xr : in.r;
  const std::size_t m = L.size();

  // 2. chunk plan
  const std::size_t S = mi.segment_frames;
  const float ov = std::clamp(opt.overlap, 0.f, 0.5f);
  const std::size_t O = std::size_t(double(S) * ov);
  const std::size_t hop = S - O;
  std::vector<std::size_t> starts{0};
  while (starts.back() + S < m) starts.push_back(starts.back() + hop);
  if (starts.size() > 1 && starts.back() + S > m) {
    std::size_t last = m - S;  // keep the final chunk inside the signal (m > S here)
    if (last == starts[starts.size() - 2]) starts.pop_back(); else starts.back() = last;
  }
  const std::size_t nc = starts.size();

  // 3. run + window + overlap-add
  std::vector<float> acc(std::size_t(kStemCount) * 2 * m, 0.f), wsum(m, 0.f);
  std::vector<float> inbuf(2 * S), outbuf(std::size_t(kStemCount) * 2 * S), w(S);
  if (opt.progress) opt.progress(0.f);
  for (std::size_t c = 0; c < nc; ++c) {
    if (opt.cancel && opt.cancel->load(std::memory_order_relaxed)) return done(SeparateResult::Status::Cancelled);
    if (!wait_if_paused(opt)) return done(SeparateResult::Status::Cancelled);
    const std::size_t s0 = starts[c];
    const std::size_t avail = std::min(S, m - s0);
    std::fill(inbuf.begin(), inbuf.end(), 0.f);
    std::copy(L.begin() + long(s0), L.begin() + long(s0 + avail), inbuf.begin());
    std::copy(R.begin() + long(s0), R.begin() + long(s0 + avail), inbuf.begin() + long(S));
    std::string err;
    if (!model.run(inbuf.data(), outbuf.data(), &err)) return done(SeparateResult::Status::Failed, err.empty() ? "model run failed" : err);
    // complementary sin^2 / cos^2 ramps of length O on interior borders; none at the very start and end
    for (std::size_t i = 0; i < S; ++i) {
      double v = 1;
      if (c > 0 && i < O) { double s = std::sin(kPi / 2 * (double(i) + 0.5) / double(O)); v *= s * s; }
      if (c + 1 < nc && i >= S - O) { double s = std::cos(kPi / 2 * (double(i - (S - O)) + 0.5) / double(O)); v *= s * s; }
      w[i] = float(v);
    }
    for (int s = 0; s < kStemCount; ++s)
      for (int ch = 0; ch < 2; ++ch) {
        const float* src = &outbuf[(std::size_t(s) * 2 + std::size_t(ch)) * S];
        float* dst = &acc[(std::size_t(s) * 2 + std::size_t(ch)) * m + s0];
        for (std::size_t i = 0; i < avail; ++i) dst[i] += w[i] * src[i];
      }
    for (std::size_t i = 0; i < avail; ++i) wsum[s0 + i] += w[i];
    if (opt.progress) opt.progress(float(c + 1) / float(nc) * 0.98f);
  }

  // 4. normalise, back to the input rate
  for (int s = 0; s < kStemCount; ++s)
    for (int ch = 0; ch < 2; ++ch) {
      float* p = &acc[(std::size_t(s) * 2 + std::size_t(ch)) * m];
      for (std::size_t i = 0; i < m; ++i) p[i] = wsum[i] > 1e-6f ? p[i] / wsum[i] : 0.f;
      std::vector<float> v(p, p + m);
      if (rs) v = resample(v, mi.sample_rate, in.sample_rate);
      v.resize(n, 0.f);
      (ch == 0 ? res.stems[std::size_t(s)].l : res.stems[std::size_t(s)].r) = std::move(v);
    }
  acc.clear(); acc.shrink_to_fit();

  // 5. mixture consistency
  make_consistent(in, res.stems, opt.mixture);
  if (opt.progress) opt.progress(1.f);
  return done(SeparateResult::Status::Ok);
}

}  // namespace deckhand::stems
