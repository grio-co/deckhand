#include "mixer_prefs.hpp"

#include <cstdio>
#include <cstdlib>

#include "deckhand/library/library.hpp"

namespace deckhand {

using mixer::MixerField;

const std::vector<MixerPref>& mixer_prefs() {
  static const std::vector<MixerPref> t = {
      {"mixer.fader_law", "fader_law", MixerField::FaderLaw, true, 3, "", 0, 2, 1, false},
      {"mixer.fader_range_db", "fader_range_db", MixerField::FaderRangeDb, false, 0, "dB", 20, 90, 5, false},
      {"mixer.crossfader_law", "crossfader_law", MixerField::CrossfaderLaw, true, 4, "", 0, 3, 1, false},
      {"mixer.crossfader_cut_in", "crossfader_cut_in", MixerField::CrossfaderCutIn, false, 0, "%", 0.005f, 0.5f, 0.01f, false},
      {"mixer.eq_kill_db", "eq_kill_db", MixerField::EqKillDb, false, 0, "dB", 6, 120, 6, true},
      {"mixer.eq_boost_db", "eq_boost_db", MixerField::EqBoostDb, false, 0, "dB", 0, 12, 1, false},
      {"mixer.eq_low_hz", "eq_low_hz", MixerField::EqLowHz, false, 0, "Hz", 60, 500, 20, true},
      {"mixer.eq_high_hz", "eq_high_hz", MixerField::EqHighHz, false, 0, "Hz", 1500, 10000, 250, true},
      {"mixer.trim_max_db", "trim_max_db", MixerField::TrimMaxDb, false, 0, "dB", 0, 24, 1, false},
      {"mixer.trim_cut_db", "trim_cut_db", MixerField::TrimCutDb, false, 0, "dB", 0, 40, 1, false},
  };
  return t;
}

const MixerPref* mixer_pref_by_name(const std::string& name) {
  for (const MixerPref& p : mixer_prefs())
    if (name == p.name) return &p;
  return nullptr;
}

float mixer_value(const mixer::Settings& s, MixerField f) {
  switch (f) {
    case MixerField::FaderLaw: return static_cast<float>(s.fader_law);
    case MixerField::FaderRangeDb: return s.fader_range_db;
    case MixerField::CrossfaderLaw: return static_cast<float>(s.crossfader_law);
    case MixerField::CrossfaderCutIn: return s.crossfader_cut_in;
    case MixerField::EqKillDb: return s.eq_kill_db;
    case MixerField::EqBoostDb: return s.eq_boost_db;
    case MixerField::EqLowHz: return s.eq_low_hz;
    case MixerField::EqHighHz: return s.eq_high_hz;
    case MixerField::TrimMaxDb: return s.trim_max_db;
    case MixerField::TrimCutDb: return s.trim_cut_db;
    default: return 0;
  }
}

mixer::Settings mixer_settings_load(Library& lib) {
  mixer::Settings s;
  for (const MixerPref& p : mixer_prefs()) {
    const std::string v = lib.setting(p.key);
    if (v.empty()) continue;
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    if (end == v.c_str()) continue;  // not a number: keep the default
    mixer::set_field(s, p.field, f);  // clamps
  }
  return s;
}

std::string mixer_value_text(float v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", static_cast<double>(v));
  return b;
}

float mixer_setting_store(Library& lib, const MixerPref& p, float v) {
  const float c = mixer::clamp_setting(p.field, v);
  lib.set_setting(p.key, mixer_value_text(c));
  return c;
}

}  // namespace deckhand
