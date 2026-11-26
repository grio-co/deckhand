#pragma once
// One table of named controls. The API, skins and scripts all address the engine through
// these names, e.g. "deck1.play", "deck2.hotcue.3", "mixer.crossfader".
#include <string>
#include <string_view>
#include <vector>

#include "deckhand/audio/engine.hpp"

namespace deckhand {

struct ControlInfo {
  std::string name;   // with deck number and slot filled in
  std::string kind;   // "trigger" | "toggle" | "button" | "value"
  std::string range;  // human-readable value range
  std::string help;
};

// Every control name that apply_control accepts.
std::vector<ControlInfo> list_controls();

// Controls that edit the track on a deck (beat grid, cues, loops, waveform zoom). The application carries them out (they
// need its library and track state), so apply_control() refuses them: use parse_prep_control() and App::api_prep.
// Names look like deck1.grid_shift, deck2.memory_cue_set, deck1.hotcue_style.3 (slot 1..8). `action` is the dotted
// name api_prep takes ("grid.shift", "cue.memory_set"...).
bool parse_prep_control(std::string_view name, int& deck, std::string& action, int& slot);

// Applies a control. has_value = false means "press" for triggers/buttons and "toggle" for toggles.
// Returns false and sets error when the name or value is not valid.
bool apply_control(IEngine& engine, std::string_view name, bool has_value, double value, std::string& error);

// The engine commands a control change consists of, without sending them (for scheduling). Toggles are
// resolved against the engine state at the time of this call.
bool build_control(const IEngine& engine, std::string_view name, bool has_value, double value, std::vector<Command>& out,
                   std::string& error);

// Current value of a control as shown by the engine (for skins and API reads). Returns false if
// the control has no readable state.
bool read_control(const IEngine& engine, std::string_view name, double& value);

}  // namespace deckhand
