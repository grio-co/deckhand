#pragma once
// Synthetic vinyl sides for the capture tests: tones standing in for music, groove noise in the gaps, crackle, clicks and
// turntable rumble, with the true track boundaries kept so detection can be judged against them.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace synth {

constexpr double kPi = 3.14159265358979323846;

struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed = 1) : s(seed * 2685821657736338717ull + 12345) {}
  double uni() {  // 0..1
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return static_cast<double>((s * 2685821657736338717ull) >> 11) / 9007199254740992.0;
  }
  double gauss() { double a = 0; for (int i = 0; i < 6; ++i) a += uni(); return (a - 3.0) * 1.4142; }  // ~unit variance
};

struct TrackSpec {
  double seconds = 40;
  double level = 0.2;          // peak amplitude of the loudest part
  double intro_s = 0;          // a quiet intro before the track opens up
  double intro_level = 0.0125; // constant tone level during the intro
  bool intro_fade = false;     // true: the intro is a log-linear fade-in from silence over intro_s instead
  double base_hz = 220;
  double end_fade_s = 0.05;
};

struct SideSpec {
  std::uint32_t rate = 44100;
  std::vector<TrackSpec> tracks;
  double gap_s = 2.5, lead_in_s = 3.0, lead_out_s = 4.0;
  double noise = 0.002;        // groove noise rms
  double crackle_per_s = 0;    // random clicks
  double click_amp = 0.25;
  double rumble = 0;           // 30 Hz amplitude
  std::uint64_t seed = 7;
};

struct Side {
  std::vector<float> pcm;  // interleaved stereo
  std::uint32_t rate = 44100;
  std::vector<std::uint64_t> starts, ends;  // true audible start / end of every track
  std::uint64_t frames() const { return pcm.size() / 2; }
};

// Music-like programme: chord of harmonics with a slow swell and short noise "hits" so the envelope is not flat.
inline void add_music(std::vector<float>& pcm, std::uint64_t at, const TrackSpec& t, std::uint32_t rate, Rng& rng) {
  const std::uint64_t n = static_cast<std::uint64_t>(t.seconds * rate);
  const std::uint64_t intro = static_cast<std::uint64_t>(t.intro_s * rate);
  for (std::uint64_t i = 0; i < n; ++i) {
    const double tt = static_cast<double>(i) / rate;
    double env = 1.0;
    if (i < intro) {
      env = t.intro_fade ? std::pow(10.0, (-35.0 * (1.0 - static_cast<double>(i) / intro)) / 20.0) * 1.0 : t.intro_level / t.level;
    }
    const double swell = 0.75 + 0.25 * std::sin(2 * kPi * 0.3 * tt);
    const double f = t.base_hz;
    double x = 0.5 * std::sin(2 * kPi * f * tt) + 0.3 * std::sin(2 * kPi * 2 * f * tt + 0.7) + 0.2 * std::sin(2 * kPi * 3.01 * f * tt + 1.9);
    const double beat = std::fmod(tt, 0.5);
    if (beat < 0.04) x += 0.3 * rng.gauss() * (1.0 - beat / 0.04);
    double fade = 1.0;
    const double left = t.seconds - tt;
    if (left < t.end_fade_s) fade = left / t.end_fade_s;
    x *= env * (i < intro ? 1.0 : swell) * t.level * fade;
    if (at + i >= pcm.size() / 2) break;
    pcm[(at + i) * 2] += static_cast<float>(x);
    pcm[(at + i) * 2 + 1] += static_cast<float>(x * 0.9);
  }
}

inline Side make_side(const SideSpec& sp) {
  Side s;
  s.rate = sp.rate;
  Rng rng(sp.seed);
  std::uint64_t total = static_cast<std::uint64_t>(sp.lead_in_s * sp.rate);
  std::vector<std::uint64_t> at;
  for (std::size_t k = 0; k < sp.tracks.size(); ++k) {
    at.push_back(total);
    total += static_cast<std::uint64_t>(sp.tracks[k].seconds * sp.rate);
    if (k + 1 < sp.tracks.size()) total += static_cast<std::uint64_t>(sp.gap_s * sp.rate);
  }
  total += static_cast<std::uint64_t>(sp.lead_out_s * sp.rate);
  s.pcm.assign(static_cast<std::size_t>(total * 2), 0.f);
  for (std::size_t k = 0; k < sp.tracks.size(); ++k) {
    add_music(s.pcm, at[k], sp.tracks[k], sp.rate, rng);
    s.starts.push_back(at[k]);
    s.ends.push_back(at[k] + static_cast<std::uint64_t>(sp.tracks[k].seconds * sp.rate));
  }
  for (std::uint64_t i = 0; i < total; ++i) {  // groove noise everywhere, rumble
    const double tt = static_cast<double>(i) / sp.rate;
    const double r = sp.rumble * std::sin(2 * kPi * 30.0 * tt);
    s.pcm[i * 2] += static_cast<float>(sp.noise * rng.gauss() + r);
    s.pcm[i * 2 + 1] += static_cast<float>(sp.noise * rng.gauss() + r);
  }
  if (sp.crackle_per_s > 0) {
    const std::uint64_t clicks = static_cast<std::uint64_t>(sp.crackle_per_s * static_cast<double>(total) / sp.rate);
    for (std::uint64_t k = 0; k < clicks; ++k) {
      const std::uint64_t p = static_cast<std::uint64_t>(rng.uni() * static_cast<double>(total - 8));
      const int len = 1 + static_cast<int>(rng.uni() * 3);
      const double a = sp.click_amp * (0.3 + 0.7 * rng.uni()) * (rng.uni() < 0.5 ? -1 : 1);
      for (int j = 0; j < len; ++j) {
        s.pcm[(p + j) * 2] += static_cast<float>(a * std::pow(0.5, j));
        if (rng.uni() < 0.7) s.pcm[(p + j) * 2 + 1] += static_cast<float>(a * std::pow(0.5, j));
      }
    }
  }
  for (auto& v : s.pcm) v = std::clamp(v, -1.0f, 1.0f);
  return s;
}

}  // namespace synth
