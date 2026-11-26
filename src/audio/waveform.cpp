#include <algorithm>
#include <cmath>

#include "deckhand/audio/track.hpp"
#include "deckhand/core/waveform_bands.hpp"

namespace deckhand {

// Three-band peak waveform: one-pole splits at roughly 200 Hz and 2.5 kHz (shared with the USB exporter's stem
// tracks through core/waveform_bands.hpp).
void build_waveform(TrackAudio& a) {
  a.wave.clear();
  if (a.frames == 0 || a.sample_rate == 0) return;
  a.wave.resize(waveform::band_points(a.frames, a.sample_rate, a.wave_rate));
  waveform::band_waveform(
      a.frames, a.sample_rate, a.wave_rate, [&](std::size_t f) { return 0.5f * (a.pcm[f * 2] + a.pcm[f * 2 + 1]); },
      [&](std::size_t p, std::uint8_t lo, std::uint8_t mi, std::uint8_t hi) { a.wave[p] = {lo, mi, hi}; });
}

}  // namespace deckhand
