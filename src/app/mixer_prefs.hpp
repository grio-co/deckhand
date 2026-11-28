#pragma once
// The mixer's gain laws as application settings (keys "mixer.*"): the table the settings panel, the REST routes and the start-up code share.
// The values are sent to the engine with Op::MixerSetting and handed to AutomixService::set_mixer_laws.
#include <string>
#include <vector>

#include "deckhand/audio/mixer_laws.hpp"

namespace deckhand {
class Library;

struct MixerPref {
  const char* key;          // library setting, e.g. "mixer.fader_law"
  const char* name;         // REST / JSON field, e.g. "fader_law"
  mixer::MixerField field;
  bool choice;              // a few named laws (shown as buttons) instead of a number
  int choices;              // number of laws when choice
  const char* unit;         // "dB", "Hz", "" ; for display
  float lo, hi, step;       // number range and the +/- step (also the default of a plain setting is mixer::Settings{})
  bool smooth_needed;       // changing it rebuilds filter coefficients (EQ depth, crossovers): not click-free while the band is cut
};
const std::vector<MixerPref>& mixer_prefs();
const MixerPref* mixer_pref_by_name(const std::string& name);
float mixer_value(const mixer::Settings& s, mixer::MixerField f);
mixer::Settings mixer_settings_load(Library& lib);
// Stores one value (clamped) under its key. Returns the stored (clamped) value.
float mixer_setting_store(Library& lib, const MixerPref& p, float v);
std::string mixer_value_text(float v);  // setting text for a value ("26", "0.05")

}  // namespace deckhand
