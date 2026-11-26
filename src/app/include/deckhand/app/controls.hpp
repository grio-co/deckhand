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

// ---- effect chain controls (protocol 5) --------------------------------------------------------------------------------
// <target>.chain.<verb>[.<n>] where <target> is deck1..deck4, mixer (master), mic, sampler or deckN.stemM (M 1..8):
//   insert[.pos] / insert_on[.pos]  value = effect number (insert_on starts the unit on); pos is 0-based, default the end
//   remove.<id> (trigger)  move.<id> (value = new index)  replace.<id> (value = effect number)
//   unit.on|wet|param|param2|param3|beats|seconds.<id>  (value)      bypass (0/1)   wet (0..1)   clear (trigger)
//   save (returns the preset JSON in `text`)   load (the preset JSON is passed as `text`)
// Chain edits are applied at once and cannot be scheduled.
bool is_chain_control(std::string_view name);
// `text` is the preset for load; `out_text` receives the preset of save and the new unit id for inserts (as a number).
bool apply_chain_control(IEngine& engine, std::string_view name, bool has_value, double value, const std::string& text,
                         std::string& out_text, std::string& error);
// "deck1".."deck4", "master" ("mixer"), "mic", "sampler", "deck2.stem3" / "deck2stem3" -> engine target. False when unknown.
bool parse_fx_target(std::string_view text, int& target);
std::string fx_target_name(int target);  // inverse of parse_fx_target ("deck1", "master", "mic", "sampler", "deck2.stem3")

// Current value of a control as shown by the engine (for skins and API reads). Returns false if
// the control has no readable state.
bool read_control(const IEngine& engine, std::string_view name, double& value);

}  // namespace deckhand
