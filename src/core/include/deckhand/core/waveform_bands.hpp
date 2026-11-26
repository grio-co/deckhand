// The three-band overview waveform: one-pole splits at about 200 Hz and 2.5 kHz, band peaks per 1/wave_rate second,
// square-root companding. Header-only and dependency free so the application (audio/waveform.cpp) and the USB exporter
// (stem tracks, export/export_stems.cpp) run literally the same code and cannot drift apart.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace deckhand::waveform {

// Number of points for `frames` frames; 0 when the input is empty.
inline std::size_t band_points(std::uint64_t frames, double sample_rate, double wave_rate) {
  if (frames == 0 || sample_rate <= 0 || wave_rate <= 0) return 0;
  return static_cast<std::size_t>(std::ceil(static_cast<double>(frames) / (sample_rate / wave_rate)));
}

// mono_at(frame) -> float (called once per frame, in ascending order); emit(point, low, mid, high) gets 0..255 bytes.
template <class MonoAt, class Emit>
void band_waveform(std::uint64_t frames, double sample_rate, double wave_rate, MonoAt&& mono_at, Emit&& emit) {
  const std::size_t points = band_points(frames, sample_rate, wave_rate);
  if (points == 0) return;
  const double per_point = sample_rate / wave_rate;
  const float k_low = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979 * 200.0 / sample_rate));
  const float k_mid = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979 * 2500.0 / sample_rate));
  float lp_low = 0, lp_mid = 0;
  std::size_t frame = 0;
  for (std::size_t p = 0; p < points; ++p) {
    const std::size_t end = std::min<std::size_t>(static_cast<std::size_t>(frames), static_cast<std::size_t>((p + 1) * per_point));
    float pk_low = 0, pk_mid = 0, pk_high = 0;
    for (; frame < end; ++frame) {
      const float x = mono_at(frame);
      lp_low += k_low * (x - lp_low);
      lp_mid += k_mid * (x - lp_mid);
      pk_low = std::max(pk_low, std::fabs(lp_low));
      pk_mid = std::max(pk_mid, std::fabs(lp_mid - lp_low));
      pk_high = std::max(pk_high, std::fabs(x - lp_mid));
    }
    const auto q = [](float v, float gain) {
      return static_cast<std::uint8_t>(std::min(255.0f, std::sqrt(std::min(1.0f, v * gain)) * 255.0f));
    };
    emit(p, q(pk_low, 1.0f), q(pk_mid, 1.6f), q(pk_high, 2.5f));
  }
}

}  // namespace deckhand::waveform
