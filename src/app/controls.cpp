#include "deckhand/app/controls.hpp"

#include "deckhand/app/chain_presets.hpp"
#include "deckhand/app/plugin_service.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace deckhand {
namespace {

enum class Kind { Trigger, Toggle, Button, Value };

struct Def {
  const char* name;
  Kind kind;
  Op op;
  double lo, hi;
  const char* range;
  const char* help;
  bool slot = false;  // name ends in .<slot 1..16>
};

// Order matters only for listing. Op values are the engine's own.
const Def kDeck[] = {
    {"play", Kind::Toggle, Op::PlayToggle, 0, 1, "0 pause, 1 play, none = toggle", "Start or pause playback"},
    {"cue", Kind::Button, Op::CueDown, 0, 1, "1 press, 0 release, none = tap", "CUE button (set / return / preview while held)"},
    {"sync", Kind::Toggle, Op::SyncToggle, 0, 1, "0 off, 1 on, none = toggle", "Beat sync to the other deck"},
    {"seek", Kind::Value, Op::Seek, 0, 1e9, "seconds from the start", "Move the playhead"},
    {"beatjump", Kind::Value, Op::BeatJump, -512, 512, "beats, signed", "Jump by a number of beats"},
    {"tempo", Kind::Value, Op::Tempo, -1, 1, "-1..1 of the tempo range (positive = faster)", "Tempo fader"},
    {"tempo_range", Kind::Value, Op::TempoRange, 0.01, 1, "fraction, e.g. 0.06 0.10 0.16 1.0", "Tempo fader range"},
    {"hotcue", Kind::Button, Op::HotCueDown, 0, 1, "1 press, 0 release, none = tap", "Hot cue pad: set if empty, else jump", true},
    {"hotcue_clear", Kind::Trigger, Op::HotCueClear, 0, 1, "none", "Delete a hot cue", true},
    {"loop_in", Kind::Trigger, Op::LoopIn, 0, 1, "none", "Set the loop start"},
    {"loop_out", Kind::Trigger, Op::LoopOut, 0, 1, "none", "Set the loop end and start looping"},
    {"loop", Kind::Value, Op::LoopToggle, 0.03125, 64, "beats used when no loop is active (default 4)", "Toggle a loop"},
    {"beatloop", Kind::Value, Op::BeatLoop, 0.03125, 64, "beats", "Start a loop of this length"},
    {"reloop", Kind::Trigger, Op::Reloop, 0, 1, "none", "Return to the last loop / exit"},
    {"loop_scale", Kind::Value, Op::LoopScale, 0.5, 2, "0.5 halve, 2 double", "Resize the active loop"},
    {"cue_call", Kind::Value, Op::CueCall, -1, 1, "-1 previous, 1 next", "Jump to the previous / next memory cue"},
    {"trim", Kind::Value, Op::Trim, 0, 1, "0..1 (0.5 = unity)", "Channel trim"},
    {"eq_high", Kind::Value, Op::EqHigh, 0, 1, "0..1 (0.5 = flat)", "High EQ"},
    {"eq_mid", Kind::Value, Op::EqMid, 0, 1, "0..1 (0.5 = flat)", "Mid EQ"},
    {"eq_low", Kind::Value, Op::EqLow, 0, 1, "0..1 (0.5 = flat)", "Low EQ"},
    {"filter", Kind::Value, Op::Filter, 0, 1, "0..1 (0.5 = off, below low-pass, above high-pass)", "Channel filter"},
    {"fader", Kind::Value, Op::Fader, 0, 1, "0..1", "Channel fader"},
    {"pfl", Kind::Toggle, Op::Pfl, 0, 1, "0 off, 1 on, none = toggle", "Headphone cue for this channel"},
    {"eject", Kind::Trigger, Op::Eject, 0, 1, "none", "Unload the track (refused while playing)"},
    {"key_lock", Kind::Toggle, Op::KeyLock, 0, 1, "0 off, 1 on, none = toggle", "Keep the pitch when the tempo changes"},
    {"key_shift", Kind::Value, Op::KeyShift, -12, 12, "semitones", "Transpose the track"},
    {"key_sync", Kind::Toggle, Op::KeyFollow, 0, 1, "0 off, 1 on, none = toggle", "Key sync: keep this deck's key matching the other deck as tempos change (sets key shift, engages key lock)"},
    {"key_sync_mode", Kind::Value, Op::KeySyncConfig, 0, 1, "0 match the key, 1 any compatible key (keeps the fine / limit settings)", "Key sync mode"},
    {"key_match", Kind::Trigger, Op::KeyMatch, 0, 1, "none", "Key sync once: shift this deck's key to fit the other deck now"},
    {"key_reset", Kind::Trigger, Op::KeyReset, 0, 1, "none", "Key shift back to 0 and key sync off"},
    {"quantize", Kind::Toggle, Op::Quantize, 0, 1, "0 off, 1 on, none = toggle", "Snap cues, loops and jumps to the beat grid"},
    {"slip", Kind::Toggle, Op::Slip, 0, 1, "0 off, 1 on, none = toggle", "Slip mode: resume where the track would have been"},
    {"reverse", Kind::Toggle, Op::Reverse, 0, 1, "0 off, 1 on, none = toggle", "Play backwards"},
    {"loop_move", Kind::Value, Op::LoopMove, -64, 64, "beats, signed", "Move the active loop"},
    {"loop_exit", Kind::Trigger, Op::LoopExit, 0, 1, "none", "Leave the active loop"},
    {"color_fx", Kind::Value, Op::ColorFx, 0, 5, "0 filter, 1 noise, 2 dub echo, 3 crush, 4 pitch, 5 space", "Effect driven by the filter knob"},
    {"auto_gain", Kind::Value, Op::AutoGain, -24, 12, "dB", "Loudness trim applied before the channel"},
    {"metronome", Kind::Toggle, Op::Metronome, 0, 1, "0 off, 1 on, none = toggle", "Click on every grid beat, on the headphone bus only"},
};

const Def kMixer[] = {
    {"crossfader", Kind::Value, Op::Crossfader, 0, 1, "0..1 (0 = left)", "Crossfader"},
    {"head_mix", Kind::Value, Op::HeadMix, 0, 1, "0 cue .. 1 master", "Headphone mix"},
    {"head_level", Kind::Value, Op::HeadLevel, 0, 1, "0..1", "Headphone level"},
    {"master_level", Kind::Value, Op::MasterLevel, 0, 1, "0..1", "Master level"},
    {"master_bpm", Kind::Value, Op::MasterBpm, 0, 300, "BPM, 0 = follow the decks", "Tempo used by beat-synced effects and the sampler"},
    {"sampler_level", Kind::Value, Op::SamplerLevel, 0, 1, "0..1", "Sampler level"},
    {"mic_on", Kind::Toggle, Op::MicOn, 0, 1, "0 off, 1 on, none = toggle", "Microphone channel on (needs a microphone input chosen in the settings)"},
    {"mic_level", Kind::Value, Op::MicLevel, 0, 1, "0..1 (0.8 = unity)", "Microphone level"},
    {"mic_eq_low", Kind::Value, Op::MicEqLow, 0, 1, "0..1 (0.5 = flat)", "Microphone low EQ"},
    {"mic_eq_high", Kind::Value, Op::MicEqHigh, 0, 1, "0..1 (0.5 = flat)", "Microphone high EQ"},
    {"mic_talkover", Kind::Toggle, Op::MicTalkover, 0, 2, "0 off, 1 on (ducks the music while you speak), 2 always duck, none = toggle", "Talkover: duck the music under the microphone"},
    {"mic_talkover_depth", Kind::Value, Op::MicTalkoverDepth, -24, 0, "dB of ducking, -24..0", "How far talkover lowers the music"},
    {"mic_monitor", Kind::Toggle, Op::MicMonitor, 0, 1, "0 off, 1 on, none = toggle", "Also play the microphone in the headphones"},
    {"mic_gate", Kind::Value, Op::MicGate, -80, -20, "dB threshold, -80 = noise gate off", "Microphone noise gate"},
    {"metronome_level", Kind::Value, Op::MetronomeLevel, 0, 1, "0..1", "Level of the metronome click (headphones only)"},
};

// Pre-listen player controls: preview.play ... (loading a track is POST /v1/preview/load or the browser's preview strip).
const Def kPreview[] = {
    {"play", Kind::Toggle, Op::PreviewPlay, 0, 1, "0 pause, 1 play, none = toggle", "Play or pause the pre-listen player (headphones only)"},
    {"stop", Kind::Trigger, Op::PreviewStop, 0, 1, "none", "Stop pre-listening and unload the track"},
    {"seek", Kind::Value, Op::PreviewSeek, 0, 1e9, "seconds from the start", "Move the pre-listen position"},
    {"level", Kind::Value, Op::PreviewLevel, 0, 1, "0..1 (0.7 = default)", "Pre-listen level"},
    {"master", Kind::Toggle, Op::PreviewMaster, 0, 1, "0 headphones only (default), 1 also on the master", "Route the pre-listen to the master output as well. The room hears it: leave this off"},
};

// Effect slot controls: deck1.fx2.wet, mixer.fx1.select ... (mixer = the master bus).
const Def kFx[] = {
    {"select", Kind::Value, Op::FxSelect, 0, static_cast<double>(EffectKind::Count) - 1, "effect number from GET /v1/effects (0 = none)", "Choose the effect in this slot"},
    {"on", Kind::Toggle, Op::FxEnable, 0, 1, "0 off, 1 on, none = toggle", "Switch the effect on"},
    {"wet", Kind::Value, Op::FxWet, 0, 1, "0..1", "Effect level"},
    {"param", Kind::Value, Op::FxParam, 0, 1, "0..1", "The effect's own parameter"},
    {"time", Kind::Value, Op::FxTime, 0.0625, 16, "beats (1/16..16)", "Beat-synced time"},
};
// Sampler slot controls: sampler.3.trigger
const Def kSampler[] = {
    {"trigger", Kind::Trigger, Op::SamplerTrigger, 0, 1, "none", "Play the sample"},
    {"stop", Kind::Trigger, Op::SamplerStop, 0, 1, "none", "Stop the sample"},
    {"gain", Kind::Value, Op::SamplerGain, 0, 1, "0..1 (0.8 = unity)", "Slot level"},
    {"loop", Kind::Value, Op::SamplerLoop, 0, 1, "0 one-shot, 1 loop", "Loop the sample"},
    {"sync", Kind::Value, Op::SamplerSync, 0, 1, "0 off, 1 on", "Follow the master tempo"},
};

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Trigger: return "trigger";
    case Kind::Toggle: return "toggle";
    case Kind::Button: return "button";
    case Kind::Value: return "value";
  }
  return "";
}

bool parse_int(std::string_view s, int& out) {
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

struct Parsed {
  const Def* def = nullptr;
  int deck = 0;  // 0-based; unused for the mixer
  int slot = 0;  // 0-based
  bool mixer = false;
  int fx_slot = -1;  // >= 0: an effect slot control
  bool sampler = false;
  bool preview = false;
};

bool parse(std::string_view name, Parsed& p, std::string& error) {
  const auto dot = name.find('.');
  if (dot == std::string_view::npos) {
    error = "control names look like deck1.play or mixer.crossfader";
    return false;
  }
  const std::string_view group = name.substr(0, dot);
  std::string_view rest = name.substr(dot + 1);
  if (group == "preview") {
    p.preview = true;
    for (const Def& d : kPreview) {
      if (rest == d.name) {
        p.def = &d;
        return true;
      }
    }
    error = "unknown preview control";
    return false;
  }
  if (group == "sampler") {  // sampler.<slot>.<control>
    const auto d2 = rest.find('.');
    int slot = 0;
    if (d2 == std::string_view::npos || !parse_int(rest.substr(0, d2), slot) || slot < 1 || slot > kSamplerSlots) {
      error = "sampler controls look like sampler.1.trigger (slots 1..16)";
      return false;
    }
    p.sampler = true;
    p.deck = slot - 1;
    for (const Def& d : kSampler) {
      if (rest.substr(d2 + 1) == d.name) {
        p.def = &d;
        return true;
      }
    }
    error = "unknown sampler control";
    return false;
  }
  const auto fx_control = [&](std::string_view text) {  // "fx2.wet"
    const auto d2 = text.find('.');
    int slot = 0;
    if (text.substr(0, 2) != "fx" || d2 == std::string_view::npos || !parse_int(text.substr(2, d2 - 2), slot) || slot < 1 ||
        slot > kFxSlots)
      return false;
    for (const Def& d : kFx) {
      if (text.substr(d2 + 1) == d.name) {
        p.def = &d;
        p.fx_slot = slot - 1;
        return true;
      }
    }
    return false;
  };
  if (group == "mixer") {
    p.mixer = true;
    if (rest.substr(0, 2) == "fx") {
      p.deck = kFxMaster;
      if (fx_control(rest)) return true;
      error = "effect controls look like mixer.fx1.wet";
      return false;
    }
    for (const Def& d : kMixer) {
      if (rest == d.name) {
        p.def = &d;
        return true;
      }
    }
    error = "unknown mixer control";
    return false;
  }
  if (group.substr(0, 4) != "deck" || !parse_int(group.substr(4), p.deck) || p.deck < 1 || p.deck > kMaxDecks) {
    error = "unknown control group (use deck1..deck4 or mixer)";
    return false;
  }
  --p.deck;
  if (rest.substr(0, 2) == "fx") {
    if (fx_control(rest)) return true;
    error = "effect controls look like deck1.fx1.wet";
    return false;
  }
  std::string_view slot_text;
  if (const auto d2 = rest.find('.'); d2 != std::string_view::npos) {
    slot_text = rest.substr(d2 + 1);
    rest = rest.substr(0, d2);
  }
  for (const Def& d : kDeck) {
    if (rest != d.name) continue;
    if (d.slot != !slot_text.empty()) {
      error = d.slot ? "this control needs a slot, e.g. deck1.hotcue.1" : "this control takes no slot";
      return false;
    }
    if (d.slot && (!parse_int(slot_text, p.slot) || p.slot < 1 || p.slot > kHotCues)) {
      error = "slot must be 1..16";
      return false;
    }
    --p.slot;
    p.def = &d;
    return true;
  }
  error = "unknown deck control";
  return false;
}

// ---- controls the application carries out (grid editing, cues, loops) ---------------------------------------------------
struct PrepDef {
  const char* name;
  const char* action;
  const char* range;
  const char* help;
  bool slot = false;
};
const PrepDef kPrep[] = {
    {"grid_edit", "grid.edit", "0 off, 1 on, none = toggle", "Grid edit mode: the scrolling waveform shows the beat grid prominently"},
    {"grid_shift", "grid.shift", "milliseconds, signed", "Move every beat of the grid earlier or later"},
    {"grid_align", "grid.align", "none", "Move the grid so the beat nearest the playhead lies on it"},
    {"grid_first_beat", "grid.set_first_beat", "none", "The playhead becomes a beat and bar beat 1 (grid moved to it)"},
    {"grid_downbeat", "grid.downbeat", "none", "The beat nearest the playhead becomes bar beat 1"},
    {"grid_rotate", "grid.rotate", "steps, signed (default 1)", "Cycle which beat is bar beat 1"},
    {"grid_bpm", "grid.bpm_delta", "BPM change, signed (0.01, 0.1 ...)", "Change the tempo of the grid section at the playhead"},
    {"grid_set_bpm", "grid.set_bpm", "BPM 30..300", "Set the tempo of the grid section at the playhead"},
    {"grid_scale", "grid.scale", "0.5 halve, 2 double", "Halve or double the grid's BPM"},
    {"grid_tap", "grid.tap", "none", "Tap tempo: tap in time, the grid takes the tempo from the third tap"},
    {"grid_add_section", "grid.add_section", "none, or seconds", "Start a new tempo section at the playhead"},
    {"grid_remove_section", "grid.remove_section", "none, or seconds", "Remove the tempo section at the playhead"},
    {"grid_lock", "grid.lock", "0 unlock, 1 lock, none = toggle", "A locked grid is never replaced by analysis"},
    {"grid_reanalyse", "grid.reanalyse", "none", "Analyse the track again (refused while the grid is locked)"},
    {"grid_undo", "grid.undo", "none", "Undo the last grid change"},
    {"memory_cue_set", "cue.memory_set", "none = at the playhead, or seconds", "Add a memory cue (body: text = name, color = 0xRRGGBB as a number)"},
    {"memory_cue_delete", "cue.memory_delete", "none; slot-less: nearest to the playhead, or the 1-based number in body slot", "Delete a memory cue"},
    {"suggest_cues", "cue.suggest", "none = free hot slots, 1 = and memory cues at phrase boundaries", "Fill the free hot-cue slots from the phrase analysis (never overwrites a cue)"},
    {"loop_save", "cue.loop_save", "none", "Store the current loop as a saved (memory) loop"},
    {"loop_recall", "cue.loop_recall", "none = next saved loop, or body slot = its number", "Set the deck's loop to a saved loop"},
    {"loop_delete", "cue.loop_delete", "body slot = its number", "Delete a saved loop"},
    {"cue_set", "cue.main_set", "none = at the playhead, or seconds", "Move the main cue point"},
    {"hotcue_set", "cue.hot_set", "none = at the playhead, or seconds", "Set or move a hot cue", true},
    {"hotcue_style", "cue.hot_style", "body: text = name, color = 0xRRGGBB or null", "Name and colour a hot cue", true},
    {"hotcue_loop", "cue.hot_loop", "none = current loop length, or beats", "Make a hot cue a loop (again: back to a plain cue)", true},
    {"wave_zoom", "wave.zoom", "seconds of track visible in the scrolling waveform, 1.5..48", "Zoom the scrolling waveform"},
    {"wave_zoom_in", "wave.zoom_in", "none", "Zoom the scrolling waveform in"},
    {"wave_zoom_out", "wave.zoom_out", "none", "Zoom the scrolling waveform out"},
};

struct LightingDef {
  const char* name;
  const char* range;
  const char* help;
};
const LightingDef kLighting[] = {
    {"blackout", "0 off, 1 on, none = toggle", "Lights out at once (the show carries on underneath)"},
    {"hold", "0 off, 1 on, none = toggle", "Freeze the current look (strobe stops)"},
    {"master", "0..1", "Master dimmer of the lighting"},
    {"strobe", "0 no strobes, 1 allowed, none = toggle", "Allow strobing (0 removes every strobe: for photosensitive audiences)"},
    {"enable", "0 off, 1 on, none = toggle", "Run the lighting show (outputs still need to be enabled in the lighting settings)"},
};

struct AutomixDef {
  const char* name;
  const char* range;
  const char* help;
};
const AutomixDef kAutomix[] = {
    {"run", "0 stop, 1 start, none = toggle", "Start automix (plays the queue) or stop it"},
    {"start", "none", "Start automix: plays the play queue, or carries on from what is playing"},
    {"stop", "none", "Stop automix; the decks go on playing, a running transition finishes first"},
    {"pause", "0 resume, 1 pause, none = toggle", "Pause automix: no new transition starts, what plays goes on"},
    {"skip", "none", "Start the next transition now, on the next bar"},
    {"next_type", "none", "Cycle the global transition type (crossfade, cut, fade, echo out)"},
    {"next_length", "none", "Cycle the global transition length (4, 8, 16, 32 beats)"},
    {"queue_clear", "none", "Empty the play queue (what plays now is not touched)"},
    {"queue_shuffle", "none", "Shuffle the queue behind the play-next entries"},
    {"queue_skip", "none", "Drop the next entry of the play queue without playing it"},
    {"queue_back", "none", "Go back to the previous track (the one playing now follows it)"},
};


// ---- stem controls: deck1.stem.vocals.mute, deck2.stems.acapella ... -----------------------------------------------------
const char* const kStemNames[kStemCount] = {"vocals", "drums", "bass", "other"};
struct StemCtl {
  int deck = 0;
  int stem = -1;       // 0..3, or -1 for a preset / enable / detach control
  std::string verb;    // mute solo isolate level | acapella instrumental drums_only no_drums reset enable detach
};
bool parse_stem_control(std::string_view name, StemCtl& out) {
  if (name.size() < 12 || name.substr(0, 4) != "deck") return false;
  const auto dot = name.find('.');
  int deck = 0;
  if (dot == std::string_view::npos || !parse_int(name.substr(4, dot - 4), deck) || deck < 1 || deck > kMaxDecks) return false;
  std::string_view rest = name.substr(dot + 1);
  if (rest.substr(0, 5) == "stem.") {
    rest.remove_prefix(5);
    const auto d2 = rest.find('.');
    if (d2 == std::string_view::npos) return false;
    int stem = -1;
    for (int i = 0; i < kStemCount; ++i)
      if (rest.substr(0, d2) == kStemNames[i]) stem = i;
    const std::string_view verb = rest.substr(d2 + 1);
    if (stem < 0 || (verb != "mute" && verb != "solo" && verb != "isolate" && verb != "level")) return false;
    out = StemCtl{deck - 1, stem, std::string(verb)};
    return true;
  }
  if (rest.substr(0, 6) == "stems.") {
    const std::string_view verb = rest.substr(6);
    for (const char* v : {"acapella", "instrumental", "drums_only", "no_drums", "reset", "enable", "detach"})
      if (verb == v) {
        out = StemCtl{deck - 1, -1, std::string(verb)};
        return true;
      }
  }
  return false;
}
bool is_stem_control(std::string_view name) {
  StemCtl c;
  return parse_stem_control(name, c);
}
bool build_stem_control(std::string_view name, bool has_value, double value, std::vector<Command>& out, std::string& error) {
  StemCtl c;
  if (!parse_stem_control(name, c)) return false;
  if (has_value && !std::isfinite(value)) {
    error = "value must be a finite number";
    return false;
  }
  Command cmd;
  cmd.deck = static_cast<std::uint8_t>(c.deck);
  if (c.stem >= 0) {
    cmd.op = Op::StemParam;
    StemField f = StemField::Mute;
    if (c.verb == "solo") f = StemField::Solo;
    else if (c.verb == "isolate") f = StemField::Isolate;
    else if (c.verb == "level") f = StemField::Level;
    cmd.i = (c.stem << 8) | static_cast<int>(f);
    if (f == StemField::Level) {
      if (!has_value || value < 0 || value > kStemMaxLevel) {
        error = "value needed: 0..1.5 (1 = unity)";
        return false;
      }
      cmd.f = static_cast<float>(value);
    } else {
      cmd.f = has_value ? (value != 0 ? 1.0f : 0.0f) : -1.0f;
    }
  } else if (c.verb == "enable") {
    cmd.op = Op::StemEnable;
    cmd.i = has_value ? (value != 0 ? 1 : 0) : -1;
  } else if (c.verb == "detach") {
    cmd.op = Op::StemDetach;
  } else {
    cmd.op = Op::StemPreset;
    cmd.i = static_cast<int>(c.verb == "acapella" ? StemPresetId::Acapella : c.verb == "instrumental" ? StemPresetId::Instrumental
                             : c.verb == "drums_only" ? StemPresetId::DrumsOnly : c.verb == "no_drums" ? StemPresetId::NoDrums
                                                                                                          : StemPresetId::Reset);
  }
  out.push_back(cmd);
  return true;
}
bool read_stem_control(const IEngine& engine, std::string_view name, double& value) {
  StemCtl c;
  if (!parse_stem_control(name, c)) return false;
  const DeckView v = engine.deck(c.deck);
  if (c.stem >= 0) {
    const std::uint32_t bit = 1u << c.stem;
    if (c.verb == "mute") value = (v.stem_mute & bit) ? 1 : 0;
    else if (c.verb == "solo") value = (v.stem_solo & bit) ? 1 : 0;
    else if (c.verb == "isolate") value = (v.stem_isolate & bit) ? 1 : 0;
    else value = v.stem_level[c.stem];
    return true;
  }
  if (c.verb == "enable") {
    value = v.stems_on ? 1 : 0;
    return true;
  }
  return false;
}

// ---- step sequencer and slicing controls (protocol 8) -------------------------------------------------------------------
//   seq.run, seq.record (toggle)            seq.bpm (0 = follow the decks / 120)     seq.swing (0..0.75)
//   seq.pattern.N.select / .queue (N 1..8)  seq.clear (none = this pattern, 0 = all, N = pattern N)
//   seq.step.S.slotK (S 1..16, K 1..16): a step of the pattern being played: 0 off, 0..1 on at that velocity, none = toggle
//   sampler.slot.K.slices (0 / 8 / 16)   sampler.slot.K.slice.J (J 1..16, value = velocity 0..1)
//   sampler.slot.K.scratch (1 grab, 0 release, none = toggle)   sampler.slot.K.scratch.delta (seconds of the sample, signed)
struct SeqCtl {
  enum Kind { Run, Record, Bpm, Swing, Select, Queue, Clear, Step, Slices, Slice, Scratch, ScratchDelta } kind = Run;
  int a = 0, b = 0;  // pattern | step, slot | slice (0-based)
};

bool parse_seq_control(std::string_view name, SeqCtl& out) {
  const auto number = [](std::string_view s, int lo, int hi, int& v) { return parse_int(s, v) && v >= lo && v <= hi; };
  if (name.substr(0, 4) == "seq.") {
    const std::string_view r = name.substr(4);
    if (r == "run") return out.kind = SeqCtl::Run, true;
    if (r == "record") return out.kind = SeqCtl::Record, true;
    if (r == "bpm") return out.kind = SeqCtl::Bpm, true;
    if (r == "swing") return out.kind = SeqCtl::Swing, true;
    if (r == "clear") return out.kind = SeqCtl::Clear, true;
    if (r.substr(0, 8) == "pattern.") {
      const auto dot = r.find('.', 8);
      int n = 0;
      if (dot == std::string_view::npos || !number(r.substr(8, dot - 8), 1, kSeqPatterns, n)) return false;
      const std::string_view verb = r.substr(dot + 1);
      if (verb != "select" && verb != "queue") return false;
      out.kind = verb == "select" ? SeqCtl::Select : SeqCtl::Queue;
      out.a = n - 1;
      return true;
    }
    if (r.substr(0, 5) == "step.") {  // step.S.slotK
      const auto dot = r.find('.', 5);
      int s = 0, k = 0;
      if (dot == std::string_view::npos || !number(r.substr(5, dot - 5), 1, kSeqSteps, s)) return false;
      const std::string_view tail = r.substr(dot + 1);
      if (tail.substr(0, 4) != "slot" || !number(tail.substr(4), 1, kSamplerSlots, k)) return false;
      out.kind = SeqCtl::Step;
      out.a = s - 1;
      out.b = k - 1;
      return true;
    }
    return false;
  }
  if (name.substr(0, 13) == "sampler.slot.") {
    const std::string_view r = name.substr(13);
    const auto dot = r.find('.');
    int k = 0;
    if (dot == std::string_view::npos || !number(r.substr(0, dot), 1, kSamplerSlots, k)) return false;
    out.a = k - 1;
    const std::string_view tail = r.substr(dot + 1);
    if (tail == "slices") return out.kind = SeqCtl::Slices, true;
    if (tail == "scratch") return out.kind = SeqCtl::Scratch, true;
    if (tail == "scratch.delta") return out.kind = SeqCtl::ScratchDelta, true;
    int j = 0;
    if (tail.substr(0, 6) == "slice." && number(tail.substr(6), 1, 16, j)) {
      out.kind = SeqCtl::Slice;
      out.b = j - 1;
      return true;
    }
  }
  return false;
}

bool build_seq_control(const IEngine& engine, std::string_view name, bool has_value, double value, std::vector<Command>& out,
                       std::string& error) {
  SeqCtl s;
  if (!parse_seq_control(name, s)) return false;
  if (has_value && !std::isfinite(value)) {
    error = "value must be a finite number";
    return false;
  }
  const auto need = [&](double lo, double hi, const char* range) {
    if (has_value && value >= lo && value <= hi) return true;
    error = std::string("value needed: ") + range;
    return false;
  };
  Command c;
  switch (s.kind) {
    case SeqCtl::Run:
    case SeqCtl::Record:
      c.op = s.kind == SeqCtl::Run ? Op::SeqRun : Op::SeqRecord;
      c.i = has_value ? (value != 0 ? 1 : 0) : -1;
      break;
    case SeqCtl::Bpm:
      if (!need(0, 400, "0..400 (0 = follow the decks, else 120)")) return false;
      c.op = Op::SeqBpm;
      c.f = static_cast<float>(value);
      break;
    case SeqCtl::Swing:
      if (!need(0, 0.75, "0..0.75")) return false;
      c.op = Op::SeqSwing;
      c.f = static_cast<float>(value);
      break;
    case SeqCtl::Select:
    case SeqCtl::Queue:
      c.op = Op::SeqSelect;
      c.i = s.a;
      c.d = s.kind == SeqCtl::Queue ? 1 : 0;
      break;
    case SeqCtl::Clear: {
      c.op = Op::SeqClear;
      c.d = -1;
      if (!has_value) c.i = engine.step_seq().current;
      else if (value == 0) c.i = -1;
      else if (value >= 1 && value <= kSeqPatterns) c.i = static_cast<int>(std::lround(value)) - 1;
      else {
        error = "value: none = this pattern, 0 = all patterns, 1..8 = that pattern";
        return false;
      }
      break;
    }
    case SeqCtl::Step: {
      const StepSeqView v = engine.step_seq();
      c.op = Op::SeqStep;
      c.deck = static_cast<std::uint8_t>(s.b);
      c.i = v.current * kSeqSteps + s.a;
      bool on = false;
      c.f = -1;
      if (has_value) {
        if (value < 0 || value > 1) {
          error = "value: 0 off, 0..1 on at that velocity, none = toggle";
          return false;
        }
        on = value > 0;
        if (on) c.f = static_cast<float>(value);
      } else {
        on = (v.pattern[v.current].cell[s.a][s.b] & 0x80) == 0;
        if (on) c.f = 0.8f;
      }
      c.d = on ? 1 : 0;
      break;
    }
    case SeqCtl::Slices:
      if (!need(0, 16, "0 (off), 8 or 16") || (value != 0 && value != 8 && value != 16)) {
        if (error.empty()) error = "value needed: 0 (off), 8 or 16";
        return false;
      }
      c.op = Op::SamplerSlice;
      c.deck = static_cast<std::uint8_t>(s.a);
      c.i = static_cast<int>(value);
      break;
    case SeqCtl::Slice:
      if (has_value && (value < 0 || value > 1)) {
        error = "value: velocity 0..1 (0 or none = full)";
        return false;
      }
      c.op = Op::SamplerSliceTrigger;
      c.deck = static_cast<std::uint8_t>(s.a);
      c.i = s.b;
      c.f = has_value ? static_cast<float>(value) : 0.0f;
      break;
    case SeqCtl::Scratch:
      c.op = Op::SamplerScratch;
      c.deck = static_cast<std::uint8_t>(s.a);
      c.i = has_value ? (value != 0 ? 1 : 0) : (engine.sampler(s.a).scratching ? 0 : 1);
      break;
    case SeqCtl::ScratchDelta:
      if (!need(-3600, 3600, "seconds of the sample, signed")) return false;
      c.op = Op::SamplerScratch;
      c.deck = static_cast<std::uint8_t>(s.a);
      c.i = 2;
      c.d = value;
      break;
  }
  out.push_back(c);
  return true;
}

bool read_seq_control(const IEngine& engine, std::string_view name, double& value) {
  SeqCtl s;
  if (!parse_seq_control(name, s)) return false;
  const StepSeqView v = engine.step_seq();
  switch (s.kind) {
    case SeqCtl::Run: value = v.running; return true;
    case SeqCtl::Record: value = v.recording; return true;
    case SeqCtl::Bpm: value = v.bpm; return true;
    case SeqCtl::Swing: value = v.swing; return true;
    case SeqCtl::Select: value = v.current == s.a ? 1 : 0; return true;
    case SeqCtl::Queue: value = v.queued == s.a ? 1 : 0; return true;
    case SeqCtl::Step: {
      const std::uint8_t cell = v.pattern[v.current].cell[s.a][s.b];
      value = (cell & 0x80) ? (cell & 0x7F) / static_cast<double>(kSeqVelMax) : 0;
      return true;
    }
    case SeqCtl::Slices: value = engine.sampler(s.a).slices; return true;
    case SeqCtl::Scratch: value = engine.sampler(s.a).scratching ? 1 : 0; return true;
    default: return false;
  }
}

void list_seq_controls(std::vector<ControlInfo>& out) {
  out.push_back({"seq.run", "toggle", "0 stop, 1 run, none = toggle", "Start or stop the step sequencer"});
  out.push_back({"seq.record", "toggle", "0 off, 1 on, none = toggle", "While running, sampler triggers are written into the pattern"});
  out.push_back({"seq.bpm", "value", "0..400 (0 = follow the decks, else 120)", "Sequencer tempo when no deck leads"});
  out.push_back({"seq.swing", "value", "0..0.75", "Delay every second step by this fraction of a step"});
  for (int n = 1; n <= kSeqPatterns; ++n) {
    out.push_back({"seq.pattern." + std::to_string(n) + ".select", "trigger", "none", "Play this pattern now"});
    out.push_back({"seq.pattern." + std::to_string(n) + ".queue", "trigger", "none", "Play this pattern from the next bar"});
  }
  out.push_back({"seq.clear", "trigger", "none = this pattern, 0 = all, 1..8 = that pattern", "Empty a pattern"});
  out.push_back({"seq.step.<1..16>.slot<1..16>", "toggle", "0 off, 0..1 on at that velocity, none = toggle", "One cell of the pattern being played (256 controls)"});
  for (int k = 1; k <= kSamplerSlots; ++k) {
    const std::string b = "sampler.slot." + std::to_string(k) + ".";
    out.push_back({b + "slices", "value", "0 (off), 8 or 16", "Cut the sample into equal slices"});
    out.push_back({b + "slice.<1..16>", "trigger", "velocity 0..1, none = full", "Play one slice once"});
    out.push_back({b + "scratch", "toggle", "1 grab, 0 release, none = toggle", "Hold the sample's position so it can be scratched"});
    out.push_back({b + "scratch.delta", "value", "seconds of the sample, signed", "Move the held sample"});
  }
}
}  // namespace

bool parse_automix_control(std::string_view name, std::string& action) {
  if (name.substr(0, 8) != "automix.") return false;
  const std::string_view rest = name.substr(8);
  for (const AutomixDef& d : kAutomix)
    if (rest == d.name) {
      action = d.name;
      return true;
    }
  return false;
}

bool parse_lighting_control(std::string_view name, std::string& action) {
  if (name.substr(0, 9) != "lighting.") return false;
  const std::string_view rest = name.substr(9);
  for (const LightingDef& d : kLighting)
    if (rest == d.name) {
      action = d.name;
      return true;
    }
  return false;
}

bool parse_prep_control(std::string_view name, int& deck, std::string& action, int& slot) {
  const auto dot = name.find('.');
  if (dot == std::string_view::npos || name.substr(0, 4) != "deck" || !parse_int(name.substr(4, dot - 4), deck) || deck < 1 || deck > kMaxDecks) return false;
  --deck;
  std::string_view rest = name.substr(dot + 1);
  std::string_view slot_text;
  if (const auto d2 = rest.find('.'); d2 != std::string_view::npos) {
    slot_text = rest.substr(d2 + 1);
    rest = rest.substr(0, d2);
  }
  for (const PrepDef& d : kPrep) {
    if (rest != d.name) continue;
    slot = 0;
    if (d.slot != !slot_text.empty()) return false;
    if (d.slot && (!parse_int(slot_text, slot) || slot < 1 || slot > kHotCues)) return false;
    action = d.action;
    return true;
  }
  return false;
}

std::vector<ControlInfo> list_controls() {
  std::vector<ControlInfo> out;
  for (int deck = 1; deck <= kMaxDecks; ++deck) {
    for (const Def& d : kDeck) {
      const std::string base = "deck" + std::to_string(deck) + "." + d.name;
      if (!d.slot) {
        out.push_back({base, kind_name(d.kind), d.range, d.help});
      } else {
        for (int s = 1; s <= kHotCues; ++s) out.push_back({base + "." + std::to_string(s), kind_name(d.kind), d.range, d.help});
      }
    }
  }
  for (int deck = 1; deck <= kMaxDecks; ++deck) {
    for (const PrepDef& d : kPrep) {
      const std::string base = "deck" + std::to_string(deck) + "." + d.name;
      const bool value = std::strcmp(d.range, "none") != 0;
      if (!d.slot) {
        out.push_back({base, value ? "value" : "trigger", d.range, d.help});
      } else {
        for (int s = 1; s <= kHotCues; ++s) out.push_back({base + "." + std::to_string(s), value ? "value" : "trigger", d.range, d.help});
      }
    }
  }
  for (const AutomixDef& d : kAutomix)
    out.push_back({std::string("automix.") + d.name, std::strcmp(d.range, "none") != 0 ? "value" : "trigger", d.range, d.help});
  for (const LightingDef& d : kLighting)
    out.push_back({std::string("lighting.") + d.name, std::strcmp(d.range, "0..1") == 0 ? "value" : "toggle", d.range, d.help});
  for (const Def& d : kMixer) out.push_back({std::string("mixer.") + d.name, kind_name(d.kind), d.range, d.help});
  for (int unit = 1; unit <= kMaxDecks + 1; ++unit) {
    const std::string group = unit <= kMaxDecks ? "deck" + std::to_string(unit) : std::string("mixer");
    for (int slot = 1; slot <= kFxSlots; ++slot)
      for (const Def& d : kFx) out.push_back({group + ".fx" + std::to_string(slot) + "." + d.name, kind_name(d.kind), d.range, d.help});
  }
  {
    std::vector<std::string> groups = {"deck1", "deck2", "deck3", "deck4", "mixer", "mic", "sampler"};
    for (int deck = 1; deck <= kMaxDecks; ++deck)
      for (int st = 1; st <= kFxStemsPerDeck; ++st) groups.push_back("deck" + std::to_string(deck) + ".stem" + std::to_string(st));
    for (const std::string& g : groups) {
      const std::string b = g + ".chain.";
      out.push_back({b + "insert", "value", "effect number", "Add an effect unit at the end of the chain (also insert.<pos>; insert_on starts it on)"});
      out.push_back({b + "remove.<id>", "trigger", "none", "Remove a unit"});
      out.push_back({b + "move.<id>", "value", "new index", "Move a unit"});
      out.push_back({b + "replace.<id>", "value", "effect number", "Replace a unit's effect, keeping its place"});
      for (const char* f : {"on", "wet", "param", "param2", "param3", "beats", "seconds"})
        out.push_back({b + "unit." + f + ".<id>", "value", "see GET /v1/effects", std::string("Unit ") + f});
      out.push_back({b + "bypass", "toggle", "0 off, 1 on", "Bypass the whole chain"});
      out.push_back({b + "wet", "value", "0..1", "Chain wet / dry"});
      out.push_back({b + "clear", "trigger", "none", "Remove every unit"});
      out.push_back({b + "insert_plugin", "trigger", "body text = plug-in id (GET /v1/fx/plugins)", "Add a plug-in unit at the end of the chain (also insert_plugin.<pos>; insert_plugin_on starts it on)"});
      out.push_back({b + "unit.pp<index>.<id>", "value", "0..1", "A plug-in unit's parameter by its position in the unit's parameter list"});
      out.push_back({b + "preset_save", "trigger", "body text = preset name", "Save the chain (and its plug-in states) as a named preset"});
      out.push_back({b + "preset_load", "trigger", "body text = preset name", "Replace the chain with a named preset"});
      out.push_back({b + "preset_delete", "trigger", "body text = preset name", "Delete a named preset"});
      out.push_back({b + "save", "trigger", "none", "Returns the chain preset as JSON"});
      out.push_back({b + "load", "trigger", "body text = preset JSON", "Replace the chain with a preset"});
    }
  }
  for (int deck = 1; deck <= kMaxDecks; ++deck) {
    const std::string dk = "deck" + std::to_string(deck);
    for (int st = 0; st < kStemCount; ++st) {
      const std::string b = dk + ".stem." + kStemNames[st] + ".";
      out.push_back({b + "mute", "toggle", "0 off, 1 on, none = toggle", std::string("Mute the ") + kStemNames[st] + " stem (needs stems on the deck)"});
      out.push_back({b + "solo", "toggle", "0 off, 1 on, none = toggle", std::string("Solo the ") + kStemNames[st] + " stem"});
      out.push_back({b + "isolate", "button", "1 press (solo while held), 0 release, none = toggle", std::string("Momentary solo of the ") + kStemNames[st] + " stem"});
      out.push_back({b + "level", "value", "0..1.5 (1 = unity)", std::string("Level of the ") + kStemNames[st] + " stem"});
    }
    out.push_back({dk + ".stems.acapella", "trigger", "none", "Vocals only"});
    out.push_back({dk + ".stems.instrumental", "trigger", "none", "Everything except the vocals"});
    out.push_back({dk + ".stems.drums_only", "trigger", "none", "Drums only"});
    out.push_back({dk + ".stems.no_drums", "trigger", "none", "Everything except the drums"});
    out.push_back({dk + ".stems.reset", "trigger", "none", "All stems on at unity level"});
    out.push_back({dk + ".stems.enable", "toggle", "0 original mix, 1 stems, none = toggle", "Play the stems or the original mix"});
    out.push_back({dk + ".stems.detach", "trigger", "none", "Drop the stems of the loaded track"});
  }
  out.push_back({"plugins.rescan", "trigger", "none", "Scan for installed effect plug-ins and probe the new ones (progress in the status line)"});
  list_seq_controls(out);
  for (const Def& d : kPreview) out.push_back({std::string("preview.") + d.name, kind_name(d.kind), d.range, d.help});
  for (int slot = 1; slot <= kSamplerSlots; ++slot)
    for (const Def& d : kSampler) out.push_back({"sampler." + std::to_string(slot) + "." + d.name, kind_name(d.kind), d.range, d.help});
  return out;
}

bool build_control(const IEngine& engine, std::string_view name, bool has_value, double value, std::vector<Command>& out,
                   std::string& error) {
  if (is_stem_control(name)) return build_stem_control(name, has_value, value, out, error);
  {
    SeqCtl sc;
    if (parse_seq_control(name, sc)) return build_seq_control(engine, name, has_value, value, out, error);
  }
  if (is_chain_control(name)) {
    error = "effect chain edits are applied at once and cannot be scheduled";
    return false;
  }
  Parsed p;
  if (!parse(name, p, error)) return false;
  const Def& d = *p.def;
  if (has_value && !std::isfinite(value)) {
    error = "value must be a finite number";
    return false;
  }
  Command c;
  c.op = d.op;
  c.deck = static_cast<std::uint8_t>(p.deck);
  c.i = p.slot;
  if (p.preview) {
    c.deck = 0;
    c.i = 0;
    if (d.kind == Kind::Toggle) {
      c.i = has_value ? (value != 0 ? 1 : 0) : -1;
    } else if (d.kind == Kind::Value) {
      if (!has_value || value < d.lo || value > d.hi) {
        error = std::string("value needed: ") + d.range;
        return false;
      }
      if (d.op == Op::PreviewSeek) {
        const PreviewView pv = engine.preview();
        if (!pv.loaded) {
          error = "nothing is loaded in the pre-listen player";
          return false;
        }
        c.d = std::min(value * pv.sample_rate, std::max(0.0, pv.length - 1));
      } else {
        c.f = static_cast<float>(value);
      }
    }
    out.push_back(c);
    return true;
  }
  const DeckView view = p.mixer || p.sampler ? DeckView{} : engine.deck(p.deck);
  if (p.fx_slot >= 0) {  // effect slot: i = slot, f (or d for select) = value
    c.i = p.fx_slot;
    if (d.op == Op::FxEnable) {
      const bool on = p.mixer ? engine.mixer().fx[p.fx_slot].on : view.fx[p.fx_slot].on;
      c.f = (has_value ? value != 0 : !on) ? 1.0f : 0.0f;
    } else {
      if (!has_value || value < d.lo || value > d.hi) {
        error = std::string("value needed: ") + d.range;
        return false;
      }
      if (d.op == Op::FxSelect) {
        const int n = static_cast<int>(std::lround(value));
        const EffectInfo* info = n == 0 ? nullptr : effect_info(static_cast<EffectKind>(n));
        if (n != 0 && (!info || info->colour)) {
          error = "unknown effect number";
          return false;
        }
        c.d = n;
      } else {
        c.f = static_cast<float>(value);
      }
    }
    out.push_back(c);
    return true;
  }
  if (p.sampler) {
    if (d.kind == Kind::Value) {
      if (!has_value || value < d.lo || value > d.hi) {
        error = std::string("value needed: ") + d.range;
        return false;
      }
      c.f = static_cast<float>(value);
      c.i = value != 0 ? 1 : 0;
    }
    out.push_back(c);
    return true;
  }

  switch (d.kind) {
    case Kind::Trigger: out.push_back(c); return true;
    case Kind::Toggle: {
      const bool current = d.op == Op::PlayToggle ? view.playing : d.op == Op::SyncToggle ? view.sync : view.pfl;
      if (d.op != Op::PlayToggle && d.op != Op::SyncToggle) {  // these take 1 / 0 / -1 (toggle)
        c.i = has_value ? (value != 0 ? 1 : 0) : -1;
        if (d.op == Op::MicTalkover && has_value && std::lround(value) == 2) c.i = 2;
        out.push_back(c);
      } else if (!has_value || (value != 0) != current) {
        out.push_back(c);
      }
      return true;
    }
    case Kind::Button: {
      const Op down = d.op, up = d.op == Op::CueDown ? Op::CueUp : Op::HotCueUp;
      if (!has_value || value != 0) {
        c.op = down;
        out.push_back(c);
      }
      if (!has_value || value == 0) {
        c.op = up;
        out.push_back(c);
      }
      return true;
    }
    case Kind::Value: {
      if (!has_value) {
        if (d.op == Op::LoopToggle) {
          value = 4;
        } else {
          error = "this control needs a value";
          return false;
        }
      }
      if (value < d.lo || value > d.hi) {
        error = std::string("value out of range: ") + d.range;
        return false;
      }
      if (d.op == Op::Seek) {
        c.d = std::min(value * view.sample_rate, std::max(0.0, view.length - 1));
      } else if (d.op == Op::CueCall) {
        c.i = value < 0 ? -1 : 1;
      } else if (d.op == Op::ColorFx) {
        c.i = static_cast<int>(std::lround(value));
      } else if (d.op == Op::KeySyncConfig) {  // mode only: fine and the limit stay as they are
        c.i = static_cast<int>(std::lround(value)) | (view.key_sync_fine ? 0x100 : 0);
        c.f = view.key_sync_limit;
      } else {
        c.f = static_cast<float>(value);
      }
      out.push_back(c);
      return true;
    }
  }
  return false;
}

bool apply_control(IEngine& engine, std::string_view name, bool has_value, double value, std::string& error) {
  if (name == "plugins.rescan") {
    if (!plugin_service().status().supported) {
      error = "this build cannot host plug-ins";
      return false;
    }
    plugin_service().rescan(true);
    return true;
  }
  {
    int deck = 0, slot = 0;
    std::string action;
    if (parse_prep_control(name, deck, action, slot)) {
      error = "this control edits the loaded track and is carried out by the application, not the engine";
      return false;
    }
  }
  {
    std::string action;
    if (parse_automix_control(name, action)) {
      error = "this control drives automix and is carried out by the application, not the engine";
      return false;
    }
  }
  {
    std::string action;
    if (parse_lighting_control(name, action)) {
      error = "this control drives the lighting module and is carried out by the application, not the engine";
      return false;
    }
  }
  if (is_chain_control(name)) {
    std::string out;
    return apply_chain_control(engine, name, has_value, value, std::string(), out, error);
  }
  std::vector<Command> commands;
  if (!build_control(engine, name, has_value, value, commands, error)) return false;
  for (const Command& c : commands) engine.send(c);
  return true;
}

KeySyncSettings parse_key_sync_settings(const std::string& mode, const std::string& fine, const std::string& limit) {
  KeySyncSettings st;
  st.mode = mode == "1" ? 1 : 0;
  st.fine = fine == "1";
  if (!limit.empty()) {
    char* end = nullptr;
    const double v = std::strtod(limit.c_str(), &end);
    if (end != limit.c_str() && std::isfinite(v)) st.limit = std::clamp(v, 0.0, 12.0);
  }
  return st;
}

void apply_key_sync_settings(IEngine& engine, const KeySyncSettings& st, int deck) {
  for (int d = 0; d < kMaxDecks; ++d)
    if (deck < 0 || d == deck) engine.key_sync_config(d, st.mode, st.fine, st.limit);
}

bool read_control(const IEngine& engine, std::string_view name, double& value) {
  if (is_stem_control(name)) return read_stem_control(engine, name, value);
  if (read_seq_control(engine, name, value)) return true;
  if (is_chain_control(name)) {
    const auto at = name.find(".chain.");
    int target = 0;
    if (!parse_fx_target(name.substr(0, at), target)) return false;
    const std::string_view verb = name.substr(at + 7);
    const FxChainInfo ci = engine.fx_chain(target);
    if (verb == "bypass") { value = ci.bypass; return true; }
    if (verb == "wet") { value = ci.wet; return true; }
    if (verb.substr(0, 5) != "unit.") return false;
    const auto d2 = verb.rfind('.');
    int id = 0;
    if (d2 <= 5 || !parse_int(verb.substr(d2 + 1), id)) return false;
    const std::string_view f = verb.substr(5, d2 - 5);
    for (int i = 0; i < ci.count && i < kFxChainMax; ++i) {
      const FxUnitInfo& u = ci.unit[i];
      if (u.id != id) continue;
      if (f == "on") value = u.on;
      else if (f == "wet") value = u.wet;
      else if (f == "param") value = u.param;
      else if (f == "param2") value = u.param2;
      else if (f == "param3") value = u.param3;
      else if (f == "beats") value = u.beats;
      else if (f == "seconds") value = u.seconds;
      else return false;
      return true;
    }
    return false;
  }
  Parsed p;
  std::string error;
  if (!parse(name, p, error)) return false;
  if (p.sampler) return false;
  if (p.preview) {
    const PreviewView pv = engine.preview();
    switch (p.def->op) {
      case Op::PreviewPlay: value = pv.playing; return true;
      case Op::PreviewSeek: value = pv.sample_rate > 0 ? pv.position / pv.sample_rate : 0; return true;
      case Op::PreviewLevel: value = pv.level; return true;
      case Op::PreviewMaster: value = pv.on_master; return true;
      default: return false;
    }
  }
  if (p.fx_slot >= 0) {
    const FxSlotView f = p.mixer ? engine.mixer().fx[p.fx_slot] : engine.deck(p.deck).fx[p.fx_slot];
    switch (p.def->op) {
      case Op::FxSelect: value = static_cast<double>(f.kind); return true;
      case Op::FxEnable: value = f.on; return true;
      case Op::FxWet: value = f.wet; return true;
      case Op::FxParam: value = f.param; return true;
      case Op::FxTime: value = f.beats; return true;
      default: return false;
    }
  }
  if (p.mixer) {
    const MixerView m = engine.mixer();
    switch (p.def->op) {
      case Op::Crossfader: value = m.crossfader; return true;
      case Op::HeadMix: value = m.head_mix; return true;
      case Op::HeadLevel: value = m.head_level; return true;
      case Op::MasterLevel: value = m.master_level; return true;
      case Op::MasterBpm: value = m.master_bpm; return true;
      case Op::SamplerLevel: value = m.sampler_level; return true;
      case Op::MicOn: value = m.mic_on; return true;
      case Op::MicLevel: value = m.mic_level; return true;
      case Op::MicEqLow: value = m.mic_eq_low; return true;
      case Op::MicEqHigh: value = m.mic_eq_high; return true;
      case Op::MicTalkover: value = m.mic_talkover; return true;
      case Op::MicTalkoverDepth: value = m.mic_depth_db; return true;
      case Op::MicMonitor: value = m.mic_monitor; return true;
      case Op::MicGate: value = m.mic_gate_db; return true;
      case Op::MetronomeLevel: value = m.metro_level; return true;
      default: return false;
    }
  }
  const DeckView v = engine.deck(p.deck);
  switch (p.def->op) {
    case Op::PlayToggle: value = v.playing; return true;
    case Op::CueDown: value = v.at_cue; return true;
    case Op::SyncToggle: value = v.sync; return true;
    case Op::Seek: value = v.sample_rate > 0 ? v.position / v.sample_rate : 0; return true;
    case Op::Tempo: value = v.tempo_range > 0 ? v.tempo / v.tempo_range : 0; return true;
    case Op::TempoRange: value = v.tempo_range; return true;
    case Op::HotCueDown: value = v.hot_cue[p.slot] >= 0; return true;
    case Op::LoopToggle:
    case Op::BeatLoop: value = v.loop_active; return true;
    case Op::Trim: value = v.trim; return true;
    case Op::EqHigh: value = v.eq_high; return true;
    case Op::EqMid: value = v.eq_mid; return true;
    case Op::EqLow: value = v.eq_low; return true;
    case Op::Filter: value = v.filter; return true;
    case Op::Fader: value = v.fader; return true;
    case Op::Pfl: value = v.pfl; return true;
    case Op::KeyLock: value = v.key_lock; return true;
    case Op::KeyShift: value = v.key_shift; return true;
    case Op::KeyFollow: value = v.key_sync_follow; return true;
    case Op::KeySyncConfig: value = v.key_sync_mode; return true;
    case Op::Quantize: value = v.quantize; return true;
    case Op::Slip: value = v.slip; return true;
    case Op::Reverse: value = v.reverse; return true;
    case Op::ColorFx: value = static_cast<double>(v.color_fx); return true;
    case Op::AutoGain: value = v.auto_gain_db; return true;
    case Op::Metronome: value = v.metronome; return true;
    default: return false;
  }
}

// ---- effect chain controls ---------------------------------------------------------------------------------------------
bool parse_fx_target(std::string_view t, int& target) {
  if (t == "master" || t == "mixer") { target = kFxMaster; return true; }
  if (t == "mic") { target = kFxTargetMic; return true; }
  if (t == "sampler") { target = kFxTargetSampler; return true; }
  if (t.size() < 5 || t.substr(0, 4) != "deck") return false;
  t.remove_prefix(4);
  int deck = 0;
  const auto st = t.find("stem");
  if (st == std::string_view::npos) {
    if (!parse_int(t, deck) || deck < 1 || deck > kMaxDecks) return false;
    target = deck - 1;
    return true;
  }
  std::string_view dtext = t.substr(0, st);
  if (!dtext.empty() && dtext.back() == '.') dtext.remove_suffix(1);
  int stem = 0;
  if (!parse_int(dtext, deck) || deck < 1 || deck > kMaxDecks || !parse_int(t.substr(st + 4), stem) || stem < 1 || stem > kFxStemsPerDeck) return false;
  target = fx_target_stem(deck - 1, stem - 1);
  return true;
}

std::string fx_target_name(int t) {
  if (t >= 0 && t < kMaxDecks) return "deck" + std::to_string(t + 1);
  if (t == kFxMaster) return "master";
  if (t == kFxTargetMic) return "mic";
  if (t == kFxTargetSampler) return "sampler";
  if (t >= kFxTargetStemBase && t < kFxTargetCount) {
    const int r = t - kFxTargetStemBase;
    return "deck" + std::to_string(r / kFxStemsPerDeck + 1) + ".stem" + std::to_string(r % kFxStemsPerDeck + 1);
  }
  return "";
}

bool is_chain_control(std::string_view name) { return name.find(".chain.") != std::string_view::npos; }

bool apply_chain_control(IEngine& engine, std::string_view name, bool has_value, double value, const std::string& text,
                         std::string& out_text, std::string& error) {
  const auto at = name.find(".chain.");
  int target = 0;
  if (at == std::string_view::npos || !parse_fx_target(name.substr(0, at), target)) {
    error = "chain controls look like deck1.chain.insert, mixer.chain.bypass, mic.chain.clear, deck2.stem1.chain.wet";
    return false;
  }
  std::string_view verb = name.substr(at + 7);
  int n = -1;
  bool has_n = false;
  const auto num_suffix = [&]() {  // strips a trailing ".<int>"
    const auto d = verb.rfind('.');
    int v = 0;
    if (d == std::string_view::npos || !parse_int(verb.substr(d + 1), v)) return false;
    n = v;
    has_n = true;
    verb = verb.substr(0, d);
    return true;
  };
  num_suffix();
  const auto need_value = [&](double lo, double hi, const char* what) {
    if (!has_value || !std::isfinite(value) || value < lo || value > hi) {
      error = std::string("value needed: ") + what;
      return false;
    }
    return true;
  };
  const auto need_id = [&]() {
    if (!has_n || n <= 0) {
      error = "this control needs a unit id, e.g. deck1.chain.remove.3 (ids are in GET /v1/fx/chains/{target})";
      return false;
    }
    return true;
  };
  FxChainEdit e;
  e.target = target;
  FxChainResult r;
  const auto run = [&]() {
    if (!engine.fx_chain_edit(e, r)) {
      error = r.error.empty() ? "the engine refused the chain edit" : r.error;
      return false;
    }
    return true;
  };
  const double kind_hi = static_cast<double>(EffectKind::Plugin) - 1;  // plug-ins have their own control
  if (verb == "insert_plugin" || verb == "insert_plugin_on") {
    if (text.empty()) {
      error = "insert_plugin needs the plug-in id as the request's \"text\" (ids are in GET /v1/fx/plugins)";
      return false;
    }
    e.verb = FxChainVerb::Insert;
    e.kind = EffectKind::Plugin;
    e.text = text;
    e.pos = has_n ? n : -1;
    e.value = verb == "insert_plugin_on" ? 1 : 0;
    if (!run()) return false;
    out_text = std::to_string(r.id);
    return true;
  }
  if (verb == "preset_save" || verb == "preset_load" || verb == "preset_delete") {
    const std::string clean = chain_preset_clean_name(text);
    if (clean.empty()) {
      error = "give the preset's name as the request's \"text\"";
      return false;
    }
    if (verb == "preset_delete") return chain_preset_delete(clean, error);
    if (verb == "preset_save") return chain_preset_save(clean, fx_chain_save_full(engine, target), error);
    std::string body;
    if (!chain_preset_read(clean, body, error)) return false;
    std::string note;
    if (!fx_chain_load_full(engine, target, body, &note)) {
      error = note.empty() ? "the engine refused the preset" : note;
      return false;
    }
    out_text = note;  // plug-ins that could not be restored, if any
    return true;
  }
  if (verb == "insert" || verb == "insert_on") {
    if (!need_value(1, kind_hi, "effect number 1.. from GET /v1/effects")) return false;
    e.verb = FxChainVerb::Insert;
    e.kind = static_cast<EffectKind>(static_cast<int>(value));
    e.pos = has_n ? n : -1;
    e.value = verb == "insert_on" ? 1 : 0;
    if (!run()) return false;
    out_text = std::to_string(r.id);
    return true;
  }
  if (verb == "remove") {
    if (!need_id()) return false;
    e.verb = FxChainVerb::Remove;
    e.id = n;
    return run();
  }
  if (verb == "move") {
    if (!need_id() || !need_value(0, kFxChainMax, "new index 0..7")) return false;
    e.verb = FxChainVerb::Move;
    e.id = n;
    e.pos = static_cast<int>(value);
    return run();
  }
  if (verb == "replace") {
    if (!need_id() || !need_value(1, kind_hi, "effect number 1..")) return false;
    e.verb = FxChainVerb::Replace;
    e.id = n;
    e.kind = static_cast<EffectKind>(static_cast<int>(value));
    return run();
  }
  if (verb.substr(0, 5) == "unit.") {
    if (!need_id()) return false;
    const std::string_view f = verb.substr(5);
    e.verb = FxChainVerb::Set;
    e.id = n;
    if (f == "on") {
      e.field = static_cast<int>(FxField::On);
      if (has_value) {
        if (!need_value(0, 1, "0 off, 1 on")) return false;
        e.value = value;
      } else {  // toggle
        e.value = 1;
        for (int i = 0; i < kFxChainMax; ++i) {
          const FxChainInfo ci = engine.fx_chain(target);
          if (i < ci.count && ci.unit[i].id == n) e.value = ci.unit[i].on ? 0 : 1;
        }
      }
      return run();
    }
    if (f.size() > 2 && f.substr(0, 2) == "pp") {  // a plug-in parameter by index
      int idx = 0;
      if (!parse_int(f.substr(2), idx) || idx < 0 || idx > 4095) {
        error = "plug-in parameters look like unit.pp3.<id> (the index in the unit's parameter list)";
        return false;
      }
      if (!need_value(0, 1, "0..1")) return false;
      e.field = kFxFieldPluginParam + idx;
      e.value = value;
      return run();
    }
    if (f == "wet") e.field = static_cast<int>(FxField::Wet);
    else if (f == "param") e.field = static_cast<int>(FxField::Param);
    else if (f == "param2") e.field = static_cast<int>(FxField::Param2);
    else if (f == "param3") e.field = static_cast<int>(FxField::Param3);
    else if (f == "beats") e.field = static_cast<int>(FxField::Beats);
    else if (f == "seconds") e.field = static_cast<int>(FxField::Seconds);
    else {
      error = "unit controls: on, wet, param, param2, param3, beats, seconds";
      return false;
    }
    if (!need_value(0, f == "beats" ? 64 : f == "seconds" ? 60 : 1, f == "beats" ? "beats 0.0625..64" : f == "seconds" ? "seconds, 0 = use beats" : "0..1")) return false;
    e.value = value;
    return run();
  }
  if (verb == "bypass" || verb == "wet") {
    e.verb = FxChainVerb::SetChain;
    if (verb == "bypass") {
      e.field = 0;
      e.value = has_value ? (value != 0 ? 1 : 0) : (engine.fx_chain(target).bypass ? 0 : 1);
    } else {
      if (!need_value(0, 1, "0..1")) return false;
      e.field = 1;
      e.value = value;
    }
    return run();
  }
  if (verb == "clear") {
    e.verb = FxChainVerb::Clear;
    return run();
  }
  if (verb == "save") {
    e.verb = FxChainVerb::Save;
    if (!run()) return false;
    out_text = r.text;
    return true;
  }
  if (verb == "load") {
    if (text.empty()) {
      error = "load needs the preset JSON in the request body as \"text\"";
      return false;
    }
    e.verb = FxChainVerb::Load;
    e.text = text;
    return run();
  }
  error = "unknown chain control";
  return false;
}

}  // namespace deckhand
