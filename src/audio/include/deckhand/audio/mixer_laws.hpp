// The mixer's gain laws, as pure functions (the engine uses them on the audio thread; tests and panels can call them).
// Every default reproduces the engine's original behaviour exactly.
#pragma once

#include <algorithm>
#include <cmath>

namespace deckhand::mixer {

// Engine-wide mixer settings (Op::MixerSetting: i = MixerField, f = value). Ranges are clamped by clamp_setting().
enum class MixerField : int {
  FaderLaw = 0,     // channel fader law: 0 audio taper (gain = k^2, default), 1 linear, 2 dB-linear over FaderRangeDb
  FaderRangeDb,     // dB-linear fader: dB at the bottom of the travel, 20 .. 90 (default 60); the last step reaches true silence
  CrossfaderLaw,    // 0 engine (full until the centre, then linear out: default), 1 constant power, 2 linear, 3 sharp cut
  CrossfaderCutIn,  // sharp cut: the far channel opens over this fraction of the travel from its end, 0.005 .. 0.5 (default 0.05)
  EqKillDb,         // depth of an EQ band at its minimum, in dB below flat, 6 .. 120 (default 26; 120 = full kill)
  EqBoostDb,        // EQ band at its maximum, dB above flat, 0 .. 12 (default 6)
  EqLowHz,          // low / mid crossover: low shelf corner, 60 .. 500 Hz (default 180)
  EqHighHz,         // mid / high crossover: high shelf corner, 1500 .. 10000 Hz (default 5000)
  TrimMaxDb,        // trim at its maximum, dB above unity, 0 .. 24 (default 9)
  TrimCutDb,        // 0 = trim falls to silence (default); else the dB the trim cuts at its minimum (dB-linear), 1 .. 40
  Count,
};
enum class FaderLaw : int { Audio = 0, Linear, DbLinear, Count };
enum class CrossfaderLaw : int { Engine = 0, ConstantPower, Linear, Sharp, Count };

struct Settings {
  int fader_law = 0;
  float fader_range_db = 60.0f;
  int crossfader_law = 0;
  float crossfader_cut_in = 0.05f;
  float eq_kill_db = 26.0f;
  float eq_boost_db = 6.0f;
  float eq_low_hz = 180.0f;
  float eq_high_hz = 5000.0f;
  float trim_max_db = 9.0f;
  float trim_cut_db = 0.0f;
};

inline float clamp_setting(MixerField f, float v) {
  if (!std::isfinite(v)) v = 0;
  switch (f) {
    case MixerField::FaderLaw: return std::floor(std::clamp(v + 0.5f, 0.0f, static_cast<float>(static_cast<int>(FaderLaw::Count) - 1)));
    case MixerField::FaderRangeDb: return std::clamp(v, 20.0f, 90.0f);
    case MixerField::CrossfaderLaw: return std::floor(std::clamp(v + 0.5f, 0.0f, static_cast<float>(static_cast<int>(CrossfaderLaw::Count) - 1)));
    case MixerField::CrossfaderCutIn: return std::clamp(v, 0.005f, 0.5f);
    case MixerField::EqKillDb: return std::clamp(v, 6.0f, 120.0f);
    case MixerField::EqBoostDb: return std::clamp(v, 0.0f, 12.0f);
    case MixerField::EqLowHz: return std::clamp(v, 60.0f, 500.0f);
    case MixerField::EqHighHz: return std::clamp(v, 1500.0f, 10000.0f);
    case MixerField::TrimMaxDb: return std::clamp(v, 0.0f, 24.0f);
    case MixerField::TrimCutDb: return std::clamp(v, 0.0f, 40.0f);
    default: return v;
  }
}

inline void set_field(Settings& s, MixerField f, float v) {
  v = clamp_setting(f, v);
  switch (f) {
    case MixerField::FaderLaw: s.fader_law = static_cast<int>(v); break;
    case MixerField::FaderRangeDb: s.fader_range_db = v; break;
    case MixerField::CrossfaderLaw: s.crossfader_law = static_cast<int>(v); break;
    case MixerField::CrossfaderCutIn: s.crossfader_cut_in = v; break;
    case MixerField::EqKillDb: s.eq_kill_db = v; break;
    case MixerField::EqBoostDb: s.eq_boost_db = v; break;
    case MixerField::EqLowHz: s.eq_low_hz = v; break;
    case MixerField::EqHighHz: s.eq_high_hz = v; break;
    case MixerField::TrimMaxDb: s.trim_max_db = v; break;
    case MixerField::TrimCutDb: s.trim_cut_db = v; break;
    default: break;
  }
}

// Channel fader position (0..1) to gain.
inline float fader_gain(const Settings& s, float k) {
  if (s.fader_law == static_cast<int>(FaderLaw::Linear)) return k;
  if (s.fader_law == static_cast<int>(FaderLaw::DbLinear)) {
    if (k <= 0) return 0;
    if (k >= 1) return 1;
    const double m = std::pow(10.0, -static_cast<double>(s.fader_range_db) / 20.0);
    return static_cast<float>((std::pow(10.0, -static_cast<double>(s.fader_range_db) * (1.0 - k) / 20.0) - m) / (1.0 - m));
  }
  return k * k;
}
// The fader position that gives gain g (inverse of fader_gain, for gain-domain ramps).
inline float fader_for_gain(const Settings& s, double g) {
  g = std::clamp(g, 0.0, 1.0);
  if (s.fader_law == static_cast<int>(FaderLaw::Linear)) return static_cast<float>(g);
  if (s.fader_law == static_cast<int>(FaderLaw::DbLinear)) {
    if (g <= 0) return 0;
    const double m = std::pow(10.0, -static_cast<double>(s.fader_range_db) / 20.0);
    return static_cast<float>(std::clamp(1.0 + 20.0 * std::log10(g * (1.0 - m) + m) / s.fader_range_db, 0.0, 1.0));
  }
  return static_cast<float>(std::sqrt(g));
}

// One side's crossfader gain. `u` = how far the crossfader is towards this channel's side, 0..1 (1 = at its end).
inline float crossfader_gain(const Settings& s, float u) {
  switch (s.crossfader_law) {
    case static_cast<int>(CrossfaderLaw::ConstantPower): return static_cast<float>(std::sin(static_cast<double>(u) * 1.5707963267948966));
    case static_cast<int>(CrossfaderLaw::Linear): return u;
    case static_cast<int>(CrossfaderLaw::Sharp): return std::min(1.0f, u / s.crossfader_cut_in);
    default: return std::min(1.0f, 2.0f * u);
  }
}

// Knob position (0..1, centre = flat) to EQ gain in dB.
inline double eq_db(const Settings& s, float k) {
  return k < 0.5f ? -static_cast<double>(s.eq_kill_db) * (1.0 - 2.0 * k) : static_cast<double>(s.eq_boost_db) * (2.0 * k - 1.0);
}

// Trim knob to gain: unity at the centre; the top boosts by trim_max_db; the bottom is silence (quadratic) or trim_cut_db.
inline float trim_gain(const Settings& s, float k) {
  if (k < 0.5f) {
    if (s.trim_cut_db <= 0) return (2 * k) * (2 * k);
    return static_cast<float>(std::pow(10.0, -static_cast<double>(s.trim_cut_db) * (1.0 - 2.0 * k) / 20.0));
  }
  return static_cast<float>(std::pow(10.0, (2.0 * k - 1.0) * static_cast<double>(s.trim_max_db) / 20.0));
}

// ---- ramps (Op::MixRamp): a knob moves to a target over a number of frames, evaluated every block ----
enum class RampField : int { Fader = 0, EqLow, EqMid, EqHigh, Trim, Filter, Count };
enum class RampShape : int { Linear = 0, EqualPower = 1 };  // EqualPower: fader only, in the gain domain (sin up / cos down)

// The knob value at progress t (0..1) of a ramp.
inline float ramp_value(const Settings& s, RampField f, RampShape shape, float from, float to, double t) {
  t = std::clamp(t, 0.0, 1.0);
  if (t >= 1.0) return to;
  if (shape == RampShape::EqualPower && f == RampField::Fader) {
    const double g0 = fader_gain(s, from), g1 = fader_gain(s, to);
    const double g = g1 >= g0 ? g0 + (g1 - g0) * std::sin(t * 1.5707963267948966) : g1 + (g0 - g1) * std::cos(t * 1.5707963267948966);
    return fader_for_gain(s, g);
  }
  return static_cast<float>(from + (to - from) * t);
}

}  // namespace deckhand::mixer
