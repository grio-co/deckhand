#include "deckhand/stems/demucs_dsp.hpp"

#include <cmath>
#include <complex>

namespace deckhand::stems {
namespace {
constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

struct Fft {
  std::vector<cd> tw;
  std::vector<unsigned> rev;
  Fft() {
    const std::size_t n = DemucsDsp::kNfft;
    tw.resize(n / 2);
    for (std::size_t i = 0; i < n / 2; ++i) tw[i] = std::polar(1.0, -2 * kPi * double(i) / double(n));
    rev.resize(n);
    unsigned bits = 0;
    while ((1u << bits) < n) ++bits;
    for (unsigned i = 0; i < n; ++i) { unsigned r = 0; for (unsigned b = 0; b < bits; ++b) if (i & (1u << b)) r |= 1u << (bits - 1 - b); rev[i] = r; }
  }
  // in-place forward transform (inverse = conj, transform, conj)
  void run(std::vector<cd>& a, bool inverse) const {
    const std::size_t n = a.size();
    if (inverse) for (auto& x : a) x = std::conj(x);
    for (std::size_t i = 0; i < n; ++i) if (rev[i] > i) std::swap(a[i], a[rev[i]]);
    for (std::size_t len = 2; len <= n; len <<= 1) {
      std::size_t step = n / len;
      for (std::size_t i = 0; i < n; i += len)
        for (std::size_t k = 0; k < len / 2; ++k) {
          cd u = a[i + k], v = a[i + k + len / 2] * tw[k * step];
          a[i + k] = u + v; a[i + k + len / 2] = u - v;
        }
    }
    if (inverse) for (auto& x : a) x = std::conj(x);
  }
};
const Fft& fft() { static Fft f; return f; }
const std::vector<double>& window() {
  static std::vector<double> w = [] {
    std::vector<double> v(DemucsDsp::kNfft);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = 0.5 - 0.5 * std::cos(2 * kPi * double(i) / double(v.size()));
    return v;
  }();
  return w;
}
// torch 'reflect' padding (edge sample is not repeated)
std::size_t reflect(long i, long n) {
  if (n == 1) return 0;
  long period = 2 * (n - 1);
  i %= period;
  if (i < 0) i += period;
  return std::size_t(i < n ? i : period - i);
}
}  // namespace

void DemucsDsp::analyse(const float* mix, std::size_t length, std::vector<float>& spec) {
  const std::size_t le = frames_for(length), hl = kHop, N = kNfft;
  const long pad = long(hl / 2 * 3);                       // demucs: pad = hl // 2 * 3
  const std::size_t padded = le * hl + 2 * std::size_t(pad);  // after the first reflect pad
  spec.assign(4 * kBins * le, 0.f);
  const auto& w = window();
  std::vector<cd> buf(N);
  const double scale = 1.0 / std::sqrt(double(N));          // normalized=True
  for (int ch = 0; ch < 2; ++ch) {
    const float* x = mix + std::size_t(ch) * length;
    // demucs pads (pad, pad + le*hl - length) with reflect; torch.stft(center=True) then reflect-pads N/2 on both sides
    auto sample = [&](long i /* index into the fully padded signal */) {
      long j = i - long(N / 2);                              // undo the stft centre pad
      j = long(reflect(j, long(padded)));                    // reflect within the first-padded signal
      long k = j - pad;                                      // undo demucs pad
      return double(x[reflect(k, long(length))]);
    };
    const std::size_t frames = 1 + (padded) / hl;            // stft frames; we need [2, 2 + le)
    (void)frames;
    for (std::size_t f = 0; f < le; ++f) {
      const std::size_t fr = f + 2;
      for (std::size_t n = 0; n < N; ++n) buf[n] = cd(sample(long(fr * hl + n)) * w[n], 0.0);
      fft().run(buf, false);
      for (std::size_t b = 0; b < kBins; ++b) {
        spec[((std::size_t(ch) * 2 + 0) * kBins + b) * le + f] = float(buf[b].real() * scale);
        spec[((std::size_t(ch) * 2 + 1) * kBins + b) * le + f] = float(buf[b].imag() * scale);
      }
    }
  }
}

void DemucsDsp::synthesise(const float* xspec, int S, std::size_t length, std::vector<float>& out) {
  const std::size_t le = frames_for(length), hl = kHop, N = kNfft;
  const long pad = long(hl / 2 * 3);
  const std::size_t le_out = hl * le + 2 * std::size_t(pad);
  const std::size_t frames = le + 4;  // z is zero-padded by 2 frames each side
  out.assign(std::size_t(S) * 2 * length, 0.f);
  const auto& w = window();
  std::vector<double> env((frames - 1) * hl + N, 0.0);
  for (std::size_t f = 0; f < frames; ++f) for (std::size_t n = 0; n < N; ++n) env[f * hl + n] += w[n] * w[n];
  std::vector<cd> buf(N);
  std::vector<double> ola(env.size());
  const double scale = std::sqrt(double(N));  // normalized=True undone
  for (int s = 0; s < S; ++s)
    for (int ch = 0; ch < 2; ++ch) {
      std::fill(ola.begin(), ola.end(), 0.0);
      for (std::size_t f = 0; f < frames; ++f) {
        std::fill(buf.begin(), buf.end(), cd(0, 0));
        if (f >= 2 && f < 2 + le) {
          const std::size_t t = f - 2;
          for (std::size_t b = 0; b < kBins; ++b) {
            double re = xspec[((std::size_t(s) * 4 + std::size_t(ch) * 2 + 0) * kBins + b) * le + t];
            double im = xspec[((std::size_t(s) * 4 + std::size_t(ch) * 2 + 1) * kBins + b) * le + t];
            buf[b] = cd(re, im) * scale;
            if (b > 0) buf[N - b] = std::conj(buf[b]);
          }
          buf[0] = cd(buf[0].real(), 0.0);  // irfft ignores the imaginary part of DC; Nyquist bin is zero
        }
        fft().run(buf, true);
        for (std::size_t n = 0; n < N; ++n) ola[f * hl + n] += buf[n].real() / double(N) * w[n];
      }
      float* o = &out[(std::size_t(s) * 2 + std::size_t(ch)) * length];
      for (std::size_t i = 0; i < length; ++i) {
        std::size_t pos = N / 2 + std::size_t(pad) + i;  // istft(center=True) trims N/2; demucs trims `pad`
        o[i] = pos < le_out + N / 2 && env[pos] > 1e-11 ? float(ola[pos] / env[pos]) : 0.f;
      }
    }
}

}  // namespace deckhand::stems
