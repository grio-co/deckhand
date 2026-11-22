#include "deckhand/app/controls.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

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
  bool slot = false;  // name ends in .<slot 1..8>
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
    {"quantize", Kind::Toggle, Op::Quantize, 0, 1, "0 off, 1 on, none = toggle", "Snap cues, loops and jumps to the beat grid"},
    {"slip", Kind::Toggle, Op::Slip, 0, 1, "0 off, 1 on, none = toggle", "Slip mode: resume where the track would have been"},
    {"reverse", Kind::Toggle, Op::Reverse, 0, 1, "0 off, 1 on, none = toggle", "Play backwards"},
    {"loop_move", Kind::Value, Op::LoopMove, -64, 64, "beats, signed", "Move the active loop"},
    {"loop_exit", Kind::Trigger, Op::LoopExit, 0, 1, "none", "Leave the active loop"},
    {"color_fx", Kind::Value, Op::ColorFx, 0, 5, "0 filter, 1 noise, 2 dub echo, 3 crush, 4 pitch, 5 space", "Effect driven by the filter knob"},
    {"auto_gain", Kind::Value, Op::AutoGain, -24, 12, "dB", "Loudness trim applied before the channel"},
};

const Def kMixer[] = {
    {"crossfader", Kind::Value, Op::Crossfader, 0, 1, "0..1 (0 = left)", "Crossfader"},
    {"head_mix", Kind::Value, Op::HeadMix, 0, 1, "0 cue .. 1 master", "Headphone mix"},
    {"head_level", Kind::Value, Op::HeadLevel, 0, 1, "0..1", "Headphone level"},
    {"master_level", Kind::Value, Op::MasterLevel, 0, 1, "0..1", "Master level"},
    {"master_bpm", Kind::Value, Op::MasterBpm, 0, 300, "BPM, 0 = follow the decks", "Tempo used by beat-synced effects and the sampler"},
    {"sampler_level", Kind::Value, Op::SamplerLevel, 0, 1, "0..1", "Sampler level"},
};

// Effect slot controls: deck1.fx2.wet, mixer.fx1.select ... (mixer = the master bus).
const Def kFx[] = {
    {"select", Kind::Value, Op::FxSelect, 0, 14, "effect number from GET /v1/effects (0 = none)", "Choose the effect in this slot"},
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
};

bool parse(std::string_view name, Parsed& p, std::string& error) {
  const auto dot = name.find('.');
  if (dot == std::string_view::npos) {
    error = "control names look like deck1.play or mixer.crossfader";
    return false;
  }
  const std::string_view group = name.substr(0, dot);
  std::string_view rest = name.substr(dot + 1);
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
      error = "slot must be 1..8";
      return false;
    }
    --p.slot;
    p.def = &d;
    return true;
  }
  error = "unknown deck control";
  return false;
}

}  // namespace

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
  for (const Def& d : kMixer) out.push_back({std::string("mixer.") + d.name, kind_name(d.kind), d.range, d.help});
  for (int unit = 1; unit <= kMaxDecks + 1; ++unit) {
    const std::string group = unit <= kMaxDecks ? "deck" + std::to_string(unit) : std::string("mixer");
    for (int slot = 1; slot <= kFxSlots; ++slot)
      for (const Def& d : kFx) out.push_back({group + ".fx" + std::to_string(slot) + "." + d.name, kind_name(d.kind), d.range, d.help});
  }
  for (int slot = 1; slot <= kSamplerSlots; ++slot)
    for (const Def& d : kSampler) out.push_back({"sampler." + std::to_string(slot) + "." + d.name, kind_name(d.kind), d.range, d.help});
  return out;
}

bool build_control(const IEngine& engine, std::string_view name, bool has_value, double value, std::vector<Command>& out,
                   std::string& error) {
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
  std::vector<Command> commands;
  if (!build_control(engine, name, has_value, value, commands, error)) return false;
  for (const Command& c : commands) engine.send(c);
  return true;
}

bool read_control(const IEngine& engine, std::string_view name, double& value) {
  Parsed p;
  std::string error;
  if (!parse(name, p, error)) return false;
  if (p.sampler) return false;
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
    case Op::Quantize: value = v.quantize; return true;
    case Op::Slip: value = v.slip; return true;
    case Op::Reverse: value = v.reverse; return true;
    case Op::ColorFx: value = static_cast<double>(v.color_fx); return true;
    case Op::AutoGain: value = v.auto_gain_db; return true;
    default: return false;
  }
}

}  // namespace deckhand
