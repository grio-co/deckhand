#include "deckhand/capture/cleanup.hpp"

#include <algorithm>
#include <cmath>

#include "biquad.hpp"
#include "deckhand/analysis/analysis.hpp"

namespace deckhand::capture {
namespace {

constexpr std::size_t kBlock = 1024, kOrder = 16, kContext = 256;

// Levinson-Durbin on the autocorrelation of w; returns a[1..p] with x[n] ~ sum a[k] x[n-k], or empty when degenerate.
std::vector<double> fit_ar(const std::vector<double>& w, std::size_t p) {
  std::vector<double> r(p + 1, 0.0);
  for (std::size_t k = 0; k <= p; ++k)
    for (std::size_t i = k; i < w.size(); ++i) r[k] += w[i] * w[i - k];
  if (r[0] < 1e-12) return {};
  r[0] *= 1.0 + 1e-9;
  std::vector<double> a(p + 1, 0.0), t(p + 1, 0.0);
  double err = r[0];
  for (std::size_t i = 1; i <= p; ++i) {
    double acc = r[i];
    for (std::size_t j = 1; j < i; ++j) acc -= a[j] * r[i - j];
    const double k = acc / err;
    t = a;
    a[i] = k;
    for (std::size_t j = 1; j < i; ++j) a[j] = t[j] - k * t[i - j];
    err *= 1.0 - k * k;
    if (err <= 0) return {};
  }
  return std::vector<double>(a.begin() + 1, a.end());
}

// Extrapolates `count` samples after hist (oldest first).
std::vector<double> extrapolate(const std::vector<double>& hist, std::size_t count) {
  std::vector<double> a = fit_ar(hist, std::min(kOrder, hist.size() / 4));
  std::vector<double> seq = hist;
  if (a.empty()) { for (std::size_t i = 0; i < count; ++i) seq.push_back(hist.back()); }
  else {
    for (std::size_t i = 0; i < count; ++i) {
      double y = 0;
      for (std::size_t k = 0; k < a.size(); ++k) y += a[k] * seq[seq.size() - 1 - k];
      seq.push_back(y);
    }
  }
  return std::vector<double>(seq.begin() + static_cast<std::ptrdiff_t>(hist.size()), seq.end());
}

void repair_span(std::vector<float>& x, std::uint64_t a, std::uint64_t len) {
  const std::uint64_t N = x.size(), b = a + len;  // damaged [a, b)
  if (a < 2 || b + 2 > N) {  // too close to an edge for context: straight line
    const double l = a > 0 ? x[a - 1] : (b < N ? x[b] : 0), r = b < N ? x[b] : l;
    for (std::uint64_t i = 0; i < len; ++i) x[a + i] = static_cast<float>(l + (r - l) * (i + 1.0) / (len + 1.0));
    return;
  }
  const std::size_t lc = static_cast<std::size_t>(std::min<std::uint64_t>(kContext, a)), rc = static_cast<std::size_t>(std::min<std::uint64_t>(kContext, N - b));
  if (len <= 2 || lc < 64 || rc < 64) {
    const double l = x[a - 1], r = x[b];
    for (std::uint64_t i = 0; i < len; ++i) x[a + i] = static_cast<float>(l + (r - l) * (i + 1.0) / (len + 1.0));
    return;
  }
  std::vector<double> left(lc), right(rc);
  for (std::size_t i = 0; i < lc; ++i) left[i] = x[a - lc + i];
  for (std::size_t i = 0; i < rc; ++i) right[i] = x[b + rc - 1 - i];  // reversed: predicts backwards in time
  const std::vector<double> fwd = extrapolate(left, static_cast<std::size_t>(len)), bwd = extrapolate(right, static_cast<std::size_t>(len));
  constexpr double kPi = 3.14159265358979323846;
  for (std::uint64_t i = 0; i < len; ++i) {
    const double w = 0.5 - 0.5 * std::cos(kPi * (i + 0.5) / static_cast<double>(len));
    const double v = (1 - w) * fwd[i] + w * bwd[len - 1 - i];
    x[a + i] = static_cast<float>(std::isfinite(v) ? v : x[a - 1]);
  }
}

std::vector<ClickSpan> detect_channel(const std::vector<float>& x, std::uint32_t channel, std::uint32_t rate, const CleanupSettings& s) {
  std::vector<ClickSpan> out;
  const std::uint64_t N = x.size();
  if (N < 16) return out;
  std::vector<float> r(N, 0.f);
  for (std::uint64_t n = 3; n < N; ++n) r[n] = x[n] - 3 * x[n - 1] + 3 * x[n - 2] - x[n - 3];
  const std::uint64_t blocks = (N + kBlock - 1) / kBlock;
  std::vector<float> sigma(blocks, 1e-6f), tmp;
  for (std::uint64_t b = 0; b < blocks; ++b) {
    tmp.clear();
    for (std::uint64_t n = b * kBlock; n < std::min(N, (b + 1) * kBlock); ++n) tmp.push_back(std::fabs(r[n]));
    std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(tmp.size() / 2), tmp.end());
    sigma[b] = std::max(1e-6f, 1.4826f * tmp[tmp.size() / 2]);
  }
  const std::uint64_t max_len = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(s.click_max_ms * 0.001 * rate));
  std::uint64_t n = 3;
  while (n < N) {
    const std::uint64_t b = n / kBlock;
    const float sg = std::max(sigma[b], b > 0 ? 0.5f * sigma[b - 1] : 0.f);
    if (std::fabs(r[n]) <= static_cast<float>(s.click_threshold) * sg) { ++n; continue; }
    std::uint64_t f0 = n, f1 = n, k = n + 1;
    while (k < N && k <= f1 + 4) {  // a burst: flagged samples within 4 of each other belong together
      const float sk = std::max(sigma[k / kBlock], 0.f);
      if (std::fabs(r[k]) > static_cast<float>(s.click_threshold) * sk) f1 = k;
      ++k;
    }
    n = f1 + 1;
    const std::uint64_t start = f0 > 0 ? f0 - 1 : 0;           // one sample of margin each side
    const std::uint64_t last = std::min(N - 1, (f1 >= 3 ? f1 - 3 : f1) + 1);
    if (last < start) continue;
    const std::uint64_t len = last - start + 1;
    if (len > max_len) continue;
    out.push_back({channel, start, len});
  }
  return out;
}

void deinterleave(const float* pcm, std::uint64_t frames, std::uint32_t ch, std::uint32_t c, std::vector<float>& x) {
  x.resize(static_cast<std::size_t>(frames));
  for (std::uint64_t i = 0; i < frames; ++i) x[i] = pcm[i * ch + c];
}

}  // namespace

std::vector<ClickSpan> detect_clicks(const float* pcm, std::uint64_t frames, std::uint32_t channels, std::uint32_t rate, const CleanupSettings& s) {
  std::vector<ClickSpan> all;
  std::vector<float> x;
  for (std::uint32_t c = 0; c < channels; ++c) {
    deinterleave(pcm, frames, channels, c, x);
    auto v = detect_channel(x, c, rate, s);
    all.insert(all.end(), v.begin(), v.end());
  }
  return all;
}

CleanupReport apply_cleanup(float* pcm, std::uint64_t frames, std::uint32_t ch, std::uint32_t rate, const CleanupSettings& s) {
  CleanupReport rep;
  rep.dc_removed.assign(ch, 0.0);
  if (frames == 0 || ch == 0) return rep;
  std::vector<float> x;
  for (std::uint32_t c = 0; c < ch; ++c) {
    deinterleave(pcm, frames, ch, c, x);
    bool changed = false;
    if (s.remove_dc) {
      double m = 0;
      for (float v : x) m += v;
      m /= static_cast<double>(frames);
      rep.dc_removed[c] = m;
      for (float& v : x) v = static_cast<float>(v - m);
      changed = true;
    }
    if (s.rumble_filter) {
      const double fc = std::min(s.rumble_hz, rate * 0.2);
      HighPass4 f(fc, rate);
      std::vector<double> y(x.begin(), x.end());
      const std::size_t prime = static_cast<std::size_t>(rate / std::max(1.0, fc)) * 8;  // settle on the first sample, so a DC start is not a step
      for (std::size_t i = 0; i < prime; ++i) f.process(y.front());
      for (double& v : y) v = f.process(v);
      f.reset();
      for (std::size_t i = 0; i < prime; ++i) f.process(y.back());
      for (std::size_t i = y.size(); i-- > 0;) y[i] = f.process(y[i]);
      for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(y[i]);
      changed = true;
    }
    if (s.click_repair) {
      const auto spans = detect_channel(x, c, rate, s);
      for (const auto& sp : spans) {
        repair_span(x, sp.start, sp.length);
        ++rep.clicks_repaired;
        rep.samples_repaired += sp.length;
      }
      changed = changed || !spans.empty();
    }
    if (changed)
      for (std::uint64_t i = 0; i < frames; ++i) pcm[i * ch + c] = x[i];
  }
  if (s.normalise) {
    std::vector<float> st;
    const float* p = pcm;
    if (ch != 2) {
      st.resize(static_cast<std::size_t>(frames) * 2);
      for (std::uint64_t i = 0; i < frames; ++i) {
        st[i * 2] = pcm[i * ch];
        st[i * 2 + 1] = pcm[i * ch + (ch > 1 ? 1 : 0)];
      }
      p = st.data();
    }
    const LoudnessResult l = measure_loudness(p, frames, rate);
    if (l.ok) {
      rep.measured_lufs = l.integrated_lufs;
      rep.gain_db = auto_gain_db(l, s.target_lufs, s.ceiling_dbfs);
      const float g = static_cast<float>(std::pow(10.0, rep.gain_db / 20.0));
      for (std::uint64_t i = 0; i < frames * ch; ++i) pcm[i] *= g;
    }
  }
  constexpr double kPi = 3.14159265358979323846;
  const std::uint64_t fi = std::min<std::uint64_t>(frames / 2, static_cast<std::uint64_t>(s.fade_in_ms * 0.001 * rate));
  const std::uint64_t fo = std::min<std::uint64_t>(frames / 2, static_cast<std::uint64_t>(s.fade_out_ms * 0.001 * rate));
  for (std::uint64_t i = 0; i < fi; ++i) {
    const float g = static_cast<float>(0.5 - 0.5 * std::cos(kPi * (i + 0.5) / static_cast<double>(fi)));
    for (std::uint32_t c = 0; c < ch; ++c) pcm[i * ch + c] *= g;
  }
  for (std::uint64_t i = 0; i < fo; ++i) {
    const float g = static_cast<float>(0.5 - 0.5 * std::cos(kPi * (i + 0.5) / static_cast<double>(fo)));
    const std::uint64_t at = frames - 1 - i;
    for (std::uint32_t c = 0; c < ch; ++c) pcm[at * ch + c] *= g;
  }
  return rep;
}

}  // namespace deckhand::capture
