#pragma once
// Per-controller preferences: what the user chose for one hardware controller (jog feel, calibration, soft takeover,
// LEDs, which mapping to use). Stored as library settings under controller.<slug>.<name> where the slug comes from the
// controller's name, so two controllers keep separate preferences. The hub applies them (DeviceOptions).
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"
#include "deckhand/midi/controller.hpp"

namespace deckhand {

// "Pioneer DDJ-FLX4" -> "pioneer_ddj_flx4": lower case letters and digits, runs of anything else become one '_'.
std::string controller_slug(const std::string& name);

struct ControllerPrefs {
  double jog_scale = 1.0;     // 0.1 .. 8
  double ticks_per_rev = 0;   // 0 = the mapping's value
  int pickup = -1;            // -1 as the mapping says, 0 off, 1 on
  bool leds = true;
  std::string mapping;        // preferred mapping name when several match; "" = automatic
  // Deck assignment, jog touch and mixer-control behaviour (see DeviceOptions). Stored together as one text setting
  // (controller.<slug>.options, "key=value ...") so more can be added without migrating anything.
  int deck_pair = 0;          // DeckPair: 0 decks 1/2, 1 decks 3/4, 2 switchable
  bool jog_vinyl = true;
  bool jog_finish_scratch = false;
  int crossfader_curve = 0;   // CrossfaderCurve: 0 engine, 1 cut, 2 smooth
  bool eq_kill = false;
  bool filter_snap = false;
  bool invert_fader = false, invert_crossfader = false, invert_tempo = false;
  bool operator==(const ControllerPrefs& o) const {
    return jog_scale == o.jog_scale && ticks_per_rev == o.ticks_per_rev && pickup == o.pickup && leds == o.leds && mapping == o.mapping &&
           deck_pair == o.deck_pair && jog_vinyl == o.jog_vinyl && jog_finish_scratch == o.jog_finish_scratch &&
           crossfader_curve == o.crossfader_curve && eq_kill == o.eq_kill && filter_snap == o.filter_snap &&
           invert_fader == o.invert_fader && invert_crossfader == o.invert_crossfader && invert_tempo == o.invert_tempo;
  }
};

ControllerPrefs load_controller_prefs(Library& lib, const std::string& name);
void save_controller_prefs(Library& lib, const std::string& name, const ControllerPrefs& p);  // only what differs from the default is stored
DeviceOptions to_device_options(const ControllerPrefs& p);

// Controllers that have been connected before (names as the hub reports them), oldest first.
std::vector<std::string> seen_controllers(Library& lib);
void note_seen_controller(Library& lib, const std::string& name);
// Every stored mapping choice (the names to pass to ControllerHub::prefer_mappings).
std::vector<std::string> preferred_mappings(Library& lib, const std::vector<std::string>& names);

// What a .dhmap file says, for the panel.
struct MappingFileInfo {
  std::string name, match, audio_match, path;
  std::vector<std::string> pad_modes;   // modes the pad buttons select
  std::vector<std::string> alt_modes;   // "sampler / stems": the button that has a second mode, as "first / second"
  std::vector<int> decks;               // 1-based decks the controller's rules address
  double ticks_per_rev = 0;
  bool pickup_default = false;
  int master_channel = 0, phones_channel = 2;
  bool hid = false;
};
// Every *.dhmap in the folders (later folders override earlier ones by name, like load_mappings); unreadable files are skipped.
std::vector<MappingFileInfo> scan_mapping_files(const std::vector<std::string>& folders);
// Files whose `match` overlaps `match` (one contains the other): the mappings that can compete for one device.
std::vector<const MappingFileInfo*> competing_mappings(const std::vector<MappingFileInfo>& files, const std::string& match);

}  // namespace deckhand
