#include "deckhand/stems/roformer_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

namespace deckhand::stems {
namespace {
constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

struct Fft2048 {
  static constexpr std::size_t N = RoformerDsp::kNfft;
  std::vector<cd> tw;
  std::vector<unsigned> rev;
  std::vector<double> win;
  Fft2048() {
    tw.resize(N / 2);
    for (std::size_t i = 0; i < N / 2; ++i) tw[i] = std::polar(1.0, -2 * kPi * double(i) / double(N));
    rev.resize(N);
    unsigned bits = 0;
    while ((1u << bits) < N) ++bits;
    for (unsigned i = 0; i < N; ++i) { unsigned r = 0; for (unsigned b = 0; b < bits; ++b) if (i & (1u << b)) r |= 1u << (bits - 1 - b); rev[i] = r; }
    win.resize(N);
    for (std::size_t i = 0; i < N; ++i) win[i] = 0.5 - 0.5 * std::cos(2 * kPi * double(i) / double(N));  // periodic Hann
  }
  void run(std::vector<cd>& a, bool inverse) const {
    if (inverse) for (auto& x : a) x = std::conj(x);
    for (std::size_t i = 0; i < N; ++i) if (rev[i] > i) std::swap(a[i], a[rev[i]]);
    for (std::size_t len = 2; len <= N; len <<= 1) {
      std::size_t step = N / len;
      for (std::size_t i = 0; i < N; i += len)
        for (std::size_t k = 0; k < len / 2; ++k) {
          cd u = a[i + k], v = a[i + k + len / 2] * tw[k * step];
          a[i + k] = u + v; a[i + k + len / 2] = u - v;
        }
    }
    if (inverse) for (auto& x : a) x = std::conj(x);
  }
};
const Fft2048& fft() { static Fft2048 f; return f; }

long reflect_index(long i, long n) {  // torch 'reflect' padding: the edge sample is not repeated
  if (n == 1) return 0;
  long period = 2 * (n - 1);
  i %= period;
  if (i < 0) i += period;
  return i < n ? i : period - i;
}
}  // namespace

bool RoformerDsp::set_bands(const std::vector<std::pair<int, int>>& b, std::string* error) {
  if (b.empty() || b.size() > 1024) { if (error) *error = "no mel bands"; return false; }
  std::size_t total = 0;
  std::vector<int> covered(kBins, 0);
  for (auto& [first, count] : b) {
    if (first < 0 || count <= 0 || std::size_t(first) + std::size_t(count) > kBins) { if (error) *error = "mel band outside the spectrum"; return false; }
    for (int k = 0; k < count; ++k) covered[std::size_t(first + k)]++;
    total += std::size_t(count);
  }
  for (int c : covered) if (c == 0) { if (error) *error = "mel bands leave a bin uncovered"; return false; }
  bands = b;
  features = total * 4;
  return true;
}

void RoformerDsp::analyse(const float* mix, std::size_t length, std::vector<float>& stft, std::vector<float>& x) const {
  const std::size_t T = frames_for(length), N = kNfft;
  stft.assign(2 * kBins * T * 2, 0.f);
  const auto& F = fft();
  std::vector<cd> buf(N);
  for (int ch = 0; ch < 2; ++ch) {
    const float* s = mix + std::size_t(ch) * length;
    for (std::size_t t = 0; t < T; ++t) {
      for (std::size_t n = 0; n < N; ++n) buf[n] = cd(double(s[std::size_t(reflect_index(long(t * kHop + n) - long(N / 2), long(length)))]) * F.win[n], 0.0);
      F.run(buf, false);
      for (std::size_t b = 0; b < kBins; ++b) {
        stft[(((std::size_t(ch) * kBins + b) * T) + t) * 2 + 0] = float(buf[b].real());
        stft[(((std::size_t(ch) * kBins + b) * T) + t) * 2 + 1] = float(buf[b].imag());
      }
    }
  }
  x.assign(T * features, 0.f);
  for (std::size_t t = 0; t < T; ++t) {
    float* row = &x[t * features];
    for (auto& [first, count] : bands)
      for (int k = 0; k < count; ++k)
        for (int ch = 0; ch < 2; ++ch)
          for (int c = 0; c < 2; ++c) *row++ = stft[(((std::size_t(ch) * kBins + std::size_t(first + k)) * T) + t) * 2 + std::size_t(c)];
  }
}

void RoformerDsp::synthesise(const std::vector<float>& stft, const float* masks, std::size_t length, std::vector<float>& out) const {
  const std::size_t T = frames_for(length), N = kNfft, outlen = kHop * (T - 1);
  const auto& F = fft();
  // mask averaging: sum the masks of every band that contains a bin, divide by the number of such bands (>= 1e-8)
  std::vector<int> nb(kBins, 0);
  for (auto& [first, count] : bands) for (int k = 0; k < count; ++k) nb[std::size_t(first + k)]++;
  std::vector<float> msum(2 * kBins * T * 2, 0.f);  // [ch][bin][t][re/im]
  for (std::size_t t = 0; t < T; ++t) {
    const float* row = masks + t * features;
    for (auto& [first, count] : bands)
      for (int k = 0; k < count; ++k)
        for (int ch = 0; ch < 2; ++ch)
          for (int c = 0; c < 2; ++c) msum[(((std::size_t(ch) * kBins + std::size_t(first + k)) * T) + t) * 2 + std::size_t(c)] += *row++;
  }
  out.assign(2 * outlen, 0.f);
  std::vector<cd> buf(N);
  std::vector<double> ola((T - 1) * kHop + N), env(ola.size(), 0.0);
  for (std::size_t t = 0; t < T; ++t) for (std::size_t n = 0; n < N; ++n) env[t * kHop + n] += F.win[n] * F.win[n];
  for (int ch = 0; ch < 2; ++ch) {
    std::fill(ola.begin(), ola.end(), 0.0);
    for (std::size_t t = 0; t < T; ++t) {
      std::fill(buf.begin(), buf.end(), cd(0, 0));
      for (std::size_t b = 0; b < kBins; ++b) {
        std::size_t i = (((std::size_t(ch) * kBins + b) * T) + t) * 2;
        double den = std::max(double(nb[b]), 1e-8);
        cd m(double(msum[i]) / den, double(msum[i + 1]) / den);
        cd y = cd(double(stft[i]), double(stft[i + 1])) * m;
        buf[b] = y;
        if (b > 0 && b < N / 2) buf[N - b] = std::conj(y);
      }
      buf[0] = cd(buf[0].real(), 0.0);
      buf[N / 2] = cd(buf[N / 2].real(), 0.0);
      F.run(buf, true);
      for (std::size_t n = 0; n < N; ++n) ola[t * kHop + n] += buf[n].real() / double(N) * F.win[n];
    }
    float* o = &out[std::size_t(ch) * outlen];
    for (std::size_t i = 0; i < outlen; ++i) {
      std::size_t pos = N / 2 + i;
      o[i] = env[pos] > 1e-11 ? float(ola[pos] / env[pos]) : 0.f;
    }
  }
}

}  // namespace deckhand::stems
