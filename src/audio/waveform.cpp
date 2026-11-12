#include <algorithm>
#include <cmath>

#include "deckhand/audio/track.hpp"

namespace deckhand {

// Three-band peak waveform: one-pole splits at roughly 200 Hz and 2.5 kHz.
void build_waveform(TrackAudio& a) {
  a.wave.clear();
  if (a.frames == 0 || a.sample_rate == 0) return;
  const double per_point = a.sample_rate / a.wave_rate;
  const std::size_t points = static_cast<std::size_t>(std::ceil(a.frames / per_point));
  a.wave.resize(points);

  const double fs = a.sample_rate;
  const float k_low = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979 * 200.0 / fs));
  const float k_mid = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979 * 2500.0 / fs));
  float lp_low = 0, lp_mid = 0;

  std::size_t frame = 0;
  for (std::size_t p = 0; p < points; ++p) {
    const std::size_t end = std::min<std::size_t>(a.frames, static_cast<std::size_t>((p + 1) * per_point));
    float pk_low = 0, pk_mid = 0, pk_high = 0;
    for (; frame < end; ++frame) {
      const float x = 0.5f * (a.pcm[frame * 2] + a.pcm[frame * 2 + 1]);
      lp_low += k_low * (x - lp_low);
      lp_mid += k_mid * (x - lp_mid);
      pk_low = std::max(pk_low, std::fabs(lp_low));
      pk_mid = std::max(pk_mid, std::fabs(lp_mid - lp_low));
      pk_high = std::max(pk_high, std::fabs(x - lp_mid));
    }
    const auto q = [](float v, float gain) {
      return static_cast<std::uint8_t>(std::min(255.0f, std::sqrt(std::min(1.0f, v * gain)) * 255.0f));
    };
    a.wave[p] = {q(pk_low, 1.0f), q(pk_mid, 1.6f), q(pk_high, 2.5f)};
  }
}

}  // namespace deckhand
