#include "deckhand/midi/controller.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "deckhand/log/log.hpp"

namespace deckhand {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
  return s;
}

// Splits a line into tokens; "quoted strings" keep their spaces; # starts a comment.
std::vector<std::string> tokens(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false, any = false;
  for (char c : line) {
    if (quoted) {
      if (c == '"') quoted = false;
      else cur += c;
    } else if (c == '"') {
      quoted = any = true;
    } else if (c == '#') {
      break;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (any || !cur.empty()) out.push_back(cur);
      cur.clear();
      any = false;
    } else {
      cur += c;
    }
  }
  if (any || !cur.empty()) out.push_back(cur);
  return out;
}

bool hex_byte(const std::string& s, std::uint8_t& out) {
  if (s.empty() || s.size() > 2) return false;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 16);
  if (*end != '\0' || v < 0 || v > 255) return false;
  out = static_cast<std::uint8_t>(v);
  return true;
}

struct Named {
  const char* name;
  int value;
};
const Named kActions[] = {
    {"ignore", int(Action::None)}, {"play", int(Action::Play)}, {"cue", int(Action::Cue)},
    {"sync", int(Action::Sync)}, {"tempo_range", int(Action::TempoRange)}, {"seek_start", int(Action::SeekStart)},
    {"jog_touch", int(Action::JogTouch)}, {"jog", int(Action::Jog)}, {"tempo", int(Action::Tempo)},
    {"loop_in", int(Action::LoopIn)}, {"loop_out", int(Action::LoopOut)}, {"loop_toggle", int(Action::LoopToggle)},
    {"reloop", int(Action::Reloop)}, {"cue_call", int(Action::CueCall)}, {"beatloop", int(Action::BeatLoop)},
    {"beatjump", int(Action::BeatJump)}, {"hotcue", int(Action::HotCue)}, {"hotcue_clear", int(Action::HotCueClear)},
    {"trim", int(Action::Trim)}, {"eq_high", int(Action::EqHigh)}, {"eq_mid", int(Action::EqMid)},
    {"eq_low", int(Action::EqLow)}, {"filter", int(Action::Filter)}, {"fader", int(Action::Fader)},
    {"crossfader", int(Action::Crossfader)}, {"head_mix", int(Action::HeadMix)}, {"head_level", int(Action::HeadLevel)},
    {"master_level", int(Action::MasterLevel)}, {"pfl", int(Action::Pfl)}, {"browse", int(Action::Browse)},
    {"browse_press", int(Action::BrowsePress)}, {"load", int(Action::Load)}, {"pad_mode", int(Action::PadMode)},
    {"key_lock", int(Action::KeyLock)}, {"key_shift", int(Action::KeyShift)}, {"quantize", int(Action::Quantize)},
    {"slip", int(Action::Slip)}, {"reverse", int(Action::Reverse)}, {"loop_move", int(Action::LoopMove)},
    {"loop_exit", int(Action::LoopExit)}, {"loop_halve", int(Action::LoopHalve)},
    {"loop_double", int(Action::LoopDouble)}, {"auto_gain", int(Action::AutoGain)}, {"color_fx", int(Action::ColorFx)},
    {"fx_select", int(Action::FxSelect)}, {"fx_on", int(Action::FxOn)}, {"fx_wet", int(Action::FxWet)},
    {"fx_param", int(Action::FxParam)}, {"fx_time", int(Action::FxTime)}, {"sampler", int(Action::Sampler)},
    {"sampler_stop", int(Action::SamplerStop)}, {"sampler_level", int(Action::SamplerLevel)},
    {"master_bpm", int(Action::MasterBpm)}, {"keyboard", int(Action::Keyboard)}, {"keyshift", int(Action::KeyShiftPad)},
    {"padfx", int(Action::PadFx)}, {"modifier", int(Action::Modifier)},
};
const Named kStates[] = {
    {"playing", int(LedState::Playing)}, {"cue", int(LedState::Cue)}, {"sync", int(LedState::Sync)},
    {"pfl", int(LedState::Pfl)}, {"loop", int(LedState::Loop)}, {"loop_in", int(LedState::LoopIn)},
    {"hotcue", int(LedState::HotCue)}, {"loaded", int(LedState::Loaded)}, {"pad_mode", int(LedState::PadMode)},
    {"level", int(LedState::Level)}, {"key_lock", int(LedState::KeyLock)}, {"quantize", int(LedState::Quantize)},
    {"slip", int(LedState::Slip)}, {"reverse", int(LedState::Reverse)}, {"fx_on", int(LedState::FxOn)},
    {"sampler_playing", int(LedState::SamplerPlaying)}, {"loop_beats", int(LedState::LoopBeats)},
    {"key_shift", int(LedState::KeyShift)}, {"modifier", int(LedState::Modifier)},
};
const Named kModes[] = {{"hotcue", 0}, {"padfx1", 1}, {"beatjump", 2}, {"sampler", 3},
                        {"keyboard", 4}, {"padfx2", 5}, {"beatloop", 6}, {"keyshift", 7}};
const Named kInKinds[] = {{"button", int(InKind::Button)}, {"abs", int(InKind::Abs)}, {"abs14", int(InKind::Abs14)},
                          {"rel", int(InKind::Rel)}, {"rel2", int(InKind::Rel2)}};
const Named kEffects[] = {
    {"none", int(EffectKind::None)}, {"echo", int(EffectKind::Echo)}, {"ping_pong", int(EffectKind::PingPong)},
    {"spiral", int(EffectKind::Spiral)}, {"reverb", int(EffectKind::Reverb)}, {"flanger", int(EffectKind::Flanger)},
    {"phaser", int(EffectKind::Phaser)}, {"low_cut_echo", int(EffectKind::LowCutEcho)},
    {"filter_sweep", int(EffectKind::FilterSweep)}, {"roll", int(EffectKind::Roll)},
    {"transform", int(EffectKind::Transform)}, {"pitch", int(EffectKind::Pitch)}, {"crusher", int(EffectKind::Crusher)},
    {"vinyl_brake", int(EffectKind::VinylBrake)}, {"backspin", int(EffectKind::Backspin)},
};
const Named kColorFx[] = {{"filter", int(ColorFx::Filter)}, {"noise", int(ColorFx::Noise)},
                          {"dub_echo", int(ColorFx::DubEcho)}, {"crush", int(ColorFx::Crush)},
                          {"pitch", int(ColorFx::Pitch)}, {"space", int(ColorFx::Space)}};

template <std::size_t N>
bool lookup(const Named (&table)[N], const std::string& name, int& out) {
  for (const Named& n : table)
    if (name == n.name) {
      out = n.value;
      return true;
    }
  return false;
}

// key=value arguments shared by "in" and "out" lines. Decks and slots are 1-based in files.
struct Args {
  int deck = 0, slot = 0, mode = -1;
  bool top = false, invert = false, blink = false;
  bool has_deck = false, has_slot = false, has_beats = false, has_dir = false, has_value = false, has_range = false;
  bool hold = false, master_unit = false;
  float amount = 1, value = 0, lo = 0, hi = 1, deadzone = -1;
  int pickup = -1, pad = -1, offset = 0;
  int on = 0x7F, off = 0, min = -1, max = -1;
  std::string match, effect, when, name;
  int master = -1, phones = -1;
  double ticks_per_rev = -1;
};

bool parse_args(const std::vector<std::string>& t, std::size_t from, Args& a, std::string& error) {
  for (std::size_t i = from; i < t.size(); ++i) {
    const auto eq = t[i].find('=');
    if (eq == std::string::npos) {
      error = "expected key=value, got '" + t[i] + "'";
      return false;
    }
    const std::string k = t[i].substr(0, eq), v = t[i].substr(eq + 1);
    const float num = static_cast<float>(std::atof(v.c_str()));
    std::uint8_t b = 0;
    if (k == "deck") { a.deck = std::atoi(v.c_str()) - 1; a.has_deck = true; }
    else if (k == "slot") { a.slot = std::atoi(v.c_str()) - 1; a.has_slot = true; }
    else if (k == "mode") { if (!lookup(kModes, v, a.mode)) { error = "unknown pad mode '" + v + "'"; return false; } }
    else if (k == "surface") a.top = v == "top";
    else if (k == "invert") a.invert = v == "1";
    else if (k == "blink") a.blink = v == "1";
    else if (k == "hold") a.hold = v == "1";
    else if (k == "beats") { a.amount = num; a.has_beats = true; }
    else if (k == "dir") { a.amount = num; a.has_dir = true; }
    else if (k == "scale") a.amount = num;
    else if (k == "value" || k == "semitones") { a.value = num; a.has_value = true; }
    else if (k == "lo") { a.lo = num; a.has_range = true; }
    else if (k == "hi") { a.hi = num; a.has_range = true; }
    else if (k == "deadzone") a.deadzone = std::max(0.0f, std::min(0.45f, num));
    else if (k == "pickup") a.pickup = v == "1" ? 1 : 0;
    else if (k == "unit") {
      if (v != "deck" && v != "master") { error = "unit must be deck or master"; return false; }
      a.master_unit = v == "master";
    }
    else if (k == "effect") a.effect = v;
    else if (k == "pad") a.pad = std::atoi(v.c_str()) - 1;
    else if (k == "offset") a.offset = std::atoi(v.c_str());
    else if (k == "when") a.when = v;
    else if (k == "name") a.name = v;
    else if (k == "on" && hex_byte(v, b)) a.on = b;
    else if (k == "off" && hex_byte(v, b)) a.off = b;
    else if (k == "min" && hex_byte(v, b)) a.min = b;
    else if (k == "max" && hex_byte(v, b)) a.max = b;
    else if (k == "match") a.match = v;
    else if (k == "master") a.master = std::atoi(v.c_str()) - 1;
    else if (k == "phones") a.phones = std::atoi(v.c_str()) - 1;
    else if (k == "ticks_per_rev") a.ticks_per_rev = std::atof(v.c_str());
    else { error = "unknown or malformed argument '" + t[i] + "'"; return false; }
  }
  if (a.deck < 0 || a.deck >= kMaxDecks) { error = "deck out of range"; return false; }
  if (a.slot < 0 || a.slot >= kSamplerSlots) { error = "slot out of range"; return false; }
  if (a.offset < 0 || a.offset >= kSamplerSlots) { error = "offset out of range"; return false; }
  return true;
}

int intern(Mapping& m, const std::string& name) {
  for (std::size_t i = 0; i < m.modifiers.size(); ++i)
    if (m.modifiers[i] == name) return static_cast<int>(i);
  m.modifiers.push_back(name);
  return static_cast<int>(m.modifiers.size() - 1);
}

bool resolve_effect(const Action action, const std::string& name, int& out) {
  if (name.empty()) return true;
  if (action == Action::ColorFx) return lookup(kColorFx, name, out);
  return lookup(kEffects, name, out);
}

const float kTempoRanges[] = {0.06f, 0.10f, 0.16f, 1.0f};
const float kFxBeats[] = {0.0625f, 0.125f, 0.25f, 0.5f, 1, 2, 4, 8, 16};
constexpr int kFxBeatCount = 9;

enum class Class { Button, Value, Delta, Any };
Class action_class(Action a) {
  switch (a) {
    case Action::Tempo: case Action::Trim: case Action::EqHigh: case Action::EqMid: case Action::EqLow:
    case Action::Filter: case Action::Fader: case Action::Crossfader: case Action::HeadMix: case Action::HeadLevel:
    case Action::MasterLevel: case Action::AutoGain: case Action::FxWet: case Action::FxParam:
    case Action::SamplerLevel:
      return Class::Value;
    case Action::Jog: case Action::Browse: return Class::Delta;
    case Action::KeyShift: case Action::MasterBpm: case Action::FxSelect: case Action::FxTime: return Class::Any;
    default: return Class::Button;
  }
}

int next_effect(int cur, int dir) {  // cycles through the effects a slot can hold (no colour-only ones)
  std::vector<int> list;
  for (int i = 0; i < effect_count(); ++i)
    if (!effect_info_at(i).colour) list.push_back(static_cast<int>(effect_info_at(i).kind));
  if (list.empty()) return 0;
  const auto it = std::find(list.begin(), list.end(), cur);
  int idx = it == list.end() ? (dir > 0 ? -1 : 0) : static_cast<int>(it - list.begin());
  idx = (idx + (dir > 0 ? 1 : -1) + static_cast<int>(list.size())) % static_cast<int>(list.size());
  return list[idx];
}

float step_beats(float cur, int dir) {
  int best = 0;
  for (int i = 1; i < kFxBeatCount; ++i)
    if (std::fabs(std::log2(kFxBeats[i]) - std::log2(std::max(cur, 0.001f))) <
        std::fabs(std::log2(kFxBeats[best]) - std::log2(std::max(cur, 0.001f))))
      best = i;
  return kFxBeats[std::max(0, std::min(kFxBeatCount - 1, best + (dir > 0 ? 1 : -1)))];
}

double loop_beats(const DeckView& d) {
  if (!d.loop_active || d.loop_out <= d.loop_in || d.bpm <= 0 || d.sample_rate <= 0) return -1;
  const double track_bpm = d.bpm / std::max(0.01, 1.0 + d.tempo);
  return (d.loop_out - d.loop_in) / d.sample_rate * track_bpm / 60.0;
}

// Parses one mapping line into m (blank and comment lines are fine). Returns false with a reason.
bool parse_line(Mapping& m, const std::string& line, std::string& why) {
  const std::vector<std::string> t = tokens(line);
  if (t.empty()) return true;
  const std::string& head = t[0];
  if (head == "name" && t.size() == 2) {
    m.name = t[1];
  } else if (head == "match" && t.size() == 2) {
    m.match = lower(t[1]);
  } else if (head == "audio") {
    Args a;
    if (!parse_args(t, 1, a, why)) return false;
    m.audio_match = lower(a.match);
    if (a.master >= 0) m.master_channel = a.master;
    m.phones_channel = a.phones;
  } else if (head == "jog") {
    Args a;
    if (!parse_args(t, 1, a, why)) return false;
    if (a.ticks_per_rev <= 0) { why = "jog needs ticks_per_rev=N"; return false; }
    m.ticks_per_rev = a.ticks_per_rev;
  } else if (head == "defaults") {
    Args a;
    if (!parse_args(t, 1, a, why)) return false;
    m.pickup_default = a.pickup == 1;
    if (a.deadzone >= 0) m.deadzone_default = a.deadzone;
  } else if (head == "init") {
    std::vector<std::uint8_t> msg;
    for (std::size_t i = 1; i < t.size(); ++i) {
      std::uint8_t b = 0;
      if (!hex_byte(t[i], b)) { why = "bad hex byte '" + t[i] + "'"; return false; }
      msg.push_back(b);
    }
    if (msg.empty()) { why = "init needs at least one byte"; return false; }
    m.init.push_back(std::move(msg));
  } else if (head == "in") {
    // in <kind> <status> <data1> [<data1 lsb>] <action> [args]
    InRule r;
    int kind = 0, action = 0;
    if (t.size() < 5 || !lookup(kInKinds, t[1], kind)) { why = "expected: in <kind> <status> <data1> <action>"; return false; }
    r.kind = static_cast<InKind>(kind);
    std::size_t i = 2;
    if (!hex_byte(t[i++], r.status) || !hex_byte(t[i++], r.data1)) { why = "bad status or data byte"; return false; }
    if (r.kind == InKind::Abs14 && (i >= t.size() || !hex_byte(t[i++], r.data1_lsb))) { why = "abs14 needs MSB and LSB"; return false; }
    if (i >= t.size() || !lookup(kActions, t[i], action)) { why = "unknown action"; return false; }
    r.action = static_cast<Action>(action);
    Args a;
    if (!parse_args(t, i + 1, a, why)) return false;
    r.deck = a.deck;
    r.slot = a.slot;
    r.mode = a.mode;
    r.top = a.top;
    r.invert = a.invert;
    r.amount = a.amount;
    r.has_deck = a.has_deck; r.has_slot = a.has_slot; r.has_beats = a.has_beats; r.has_dir = a.has_dir;
    r.has_value = a.has_value; r.has_range = a.has_range; r.master_unit = a.master_unit; r.hold = a.hold;
    r.value = a.value;
    r.lo = a.has_range ? a.lo : 0;
    r.hi = a.has_range ? a.hi : 1;
    r.deadzone = a.deadzone;
    r.pickup = a.pickup;
    r.pad = a.pad;
    r.offset = a.offset;
    if (r.action == Action::PadMode && r.mode < 0) { why = "pad_mode needs mode=<name>"; return false; }
    if (!a.when.empty()) {
      r.when_not = a.when[0] == '!';
      r.when = intern(m, r.when_not ? a.when.substr(1) : a.when);
    }
    if (r.action == Action::Modifier) {
      if (a.name.empty()) { why = "modifier needs name=<name>"; return false; }
      r.modifier = intern(m, a.name);
    }
    if (!resolve_effect(r.action, a.effect, r.effect)) { why = "unknown effect '" + a.effect + "'"; return false; }
    switch (r.action) {
      case Action::HotCue: case Action::HotCueClear:
        if (r.slot >= kHotCues) { why = "hot cue slot out of range"; return false; }
        break;
      case Action::FxSelect: case Action::FxOn: case Action::FxWet: case Action::FxParam: case Action::FxTime:
      case Action::PadFx:
        if (r.slot >= kFxSlots) { why = "effect slot out of range (1.." + std::to_string(kFxSlots) + ")"; return false; }
        break;
      case Action::Sampler: case Action::SamplerStop:
        if (r.slot + r.offset >= kSamplerSlots) { why = "sampler slot plus offset out of range"; return false; }
        break;
      default: break;
    }
    const std::size_t idx = m.in.size();
    m.in.push_back(r);
    m.index.emplace(static_cast<std::uint16_t>(r.status << 8 | r.data1), idx);
    if (r.kind == InKind::Abs14) m.index.emplace(static_cast<std::uint16_t>(r.status << 8 | r.data1_lsb), idx);
  } else if (head == "out") {
    // out <status> <data1> <state> [args]
    OutRule r;
    int state = 0;
    if (t.size() < 4 || !hex_byte(t[1], r.status) || !hex_byte(t[2], r.data1) || !lookup(kStates, t[3], state)) {
      why = "expected: out <status> <data1> <state>";
      return false;
    }
    r.state = static_cast<LedState>(state);
    Args a;
    if (!parse_args(t, 4, a, why)) return false;
    r.deck = a.deck;
    r.slot = a.slot;
    r.mode = a.mode;
    r.blink = a.blink;
    r.amount = a.amount;
    r.value = a.value;
    r.has_value = a.has_value;
    r.master_unit = a.master_unit;
    r.offset = a.offset;
    r.on = static_cast<std::uint8_t>(a.max >= 0 ? a.max : a.on);
    r.off = static_cast<std::uint8_t>(a.min >= 0 ? a.min : a.off);
    if (r.state == LedState::PadMode && r.mode < 0) { why = "pad_mode state needs mode=<name>"; return false; }
    if (r.state == LedState::Modifier) {
      if (a.name.empty()) { why = "modifier state needs name=<name>"; return false; }
      r.modifier = intern(m, a.name);
    }
    if (!resolve_effect(Action::FxOn, a.effect, r.effect)) { why = "unknown effect '" + a.effect + "'"; return false; }
    if (r.state == LedState::FxOn && r.slot >= kFxSlots) { why = "effect slot out of range"; return false; }
    if (r.state == LedState::SamplerPlaying && r.slot + r.offset >= kSamplerSlots) { why = "sampler slot plus offset out of range"; return false; }
    if (r.state == LedState::HotCue && r.slot >= kHotCues) { why = "hot cue slot out of range"; return false; }
    m.out.push_back(r);
  } else {
    why = "unknown directive '" + head + "'";
    return false;
  }
  return true;
}

// Identity of a mapping line, used to replace it when the same control or action is learned again.
void line_keys(const std::string& line, std::string& control, std::string& action) {
  const std::vector<std::string> t = tokens(line);
  control.clear();
  action.clear();
  if (t.size() < 5 || (t[0] != "in" && t[0] != "out")) return;
  std::size_t first = 4;  // first token after the address
  if (t[0] == "in") {
    control = "in " + upper(t[2]) + " " + upper(t[3]);
    first = t[1] == "abs14" ? 5 : 4;
  } else {
    control = "out " + upper(t[1]) + " " + upper(t[2]);
    first = 3;
  }
  for (std::size_t i = first; i < t.size(); ++i) {
    const std::string key = t[i].substr(0, t[i].find('='));
    const bool target = i == first || key == "deck" || key == "slot" || key == "unit" || key == "mode" ||
                        key == "when" || key == "pad" || key == "offset" || key == "effect" || key == "name";
    if (target) action += (action.empty() ? "" : " ") + t[i];
    if (t[i].rfind("when=", 0) == 0 || t[i].rfind("mode=", 0) == 0) control += " " + t[i];
  }
  action = t[0] + " " + action;
}

}  // namespace

bool parse_mapping(const std::string& text, Mapping& m, std::string& error) {
  m = Mapping{};
  std::istringstream in(text);
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    std::string why;
    if (!parse_line(m, line, why)) {
      error = "line " + std::to_string(number) + ": " + why;
      return false;
    }
  }
  if (m.name.empty() || m.match.empty()) {
    error = "mapping needs both a name and a match line";
    return false;
  }
  return true;
}

bool save_user_mapping(const std::string& device, const std::vector<std::string>& lines, const std::string& folder,
                       std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(folder, ec);
  std::string file;
  for (char c : device) file += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.') ? c : '_';
  if (file.empty()) return fail("empty device name");
  const fs::path path = fs::path(folder) / (file + ".dhmap");

  std::vector<std::string> existing;
  {
    std::ifstream f(path);
    std::string l;
    while (std::getline(f, l)) existing.push_back(l);
  }
  if (existing.empty()) {
    existing.push_back("# DeckHand controller mapping created by MIDI learn.");
    existing.push_back("name  \"" + device + "\"");
    existing.push_back("match \"" + device + "\"");
  }
  for (const std::string& add : lines) {
    Mapping check;
    std::string why;
    if (!parse_line(check, add, why)) return fail("bad line '" + add + "': " + why);
    std::string control, action;
    line_keys(add, control, action);
    if (!control.empty()) {
      existing.erase(std::remove_if(existing.begin(), existing.end(),
                                    [&](const std::string& old) {
                                      std::string c, a;
                                      line_keys(old, c, a);
                                      return !c.empty() && (c == control || a == action);
                                    }),
                     existing.end());
    }
    existing.push_back(add);
  }
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return fail("cannot write " + tmp.string());
    for (const std::string& l : existing) out << l << "\n";
    if (!out) return fail("write failed");
  }
  fs::rename(tmp, path, ec);
  if (ec) return fail("cannot replace " + path.string());
  return true;
}

std::vector<Mapping> load_mappings(const std::vector<std::string>& folders) {
  std::vector<Mapping> out;
  for (const std::string& folder : folders) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
      if (entry.path().extension() != ".dhmap") continue;
      std::ifstream f(entry.path());
      std::stringstream text;
      text << f.rdbuf();
      Mapping m;
      std::string error;
      if (!parse_mapping(text.str(), m, error)) {
        DH_LOG(Error, "controller mapping %s: %s", entry.path().string().c_str(), error.c_str());
        continue;
      }
      const auto same = std::find_if(out.begin(), out.end(), [&](const Mapping& o) { return o.name == m.name; });
      if (same != out.end()) *same = std::move(m);
      else out.push_back(std::move(m));
      DH_LOG(Debug, "loaded controller mapping %s", entry.path().filename().string().c_str());
    }
  }
  return out;
}

// ---- hub ------------------------------------------------------------------------------
struct ControllerHub::Learn {
  LearnState state = LearnState::Idle;
  std::string action_text, device, line, error;
  Action action = Action::None;
  std::uint8_t status = 0;
  std::vector<std::pair<int, int>> samples;  // (data1, value)
  std::chrono::steady_clock::time_point last;
};

ControllerHub::ControllerHub(ControlSink& sink, std::vector<Mapping> mappings)
    : sink_(sink), learn_(std::make_unique<Learn>()) {
  for (Mapping& m : mappings) mappings_.push_back(std::move(m));
}

ControllerHub::~ControllerHub() = default;

void ControllerHub::set_io(MidiIO* io) {
  io_ = io;
  devices_changed();
}

void ControllerHub::fit(Active& a) {
  const std::size_t n = a.map->in.size();
  a.msb.resize(n, 0);
  a.lsb.resize(n, 0);
  a.down.resize(n, 0);
  a.engaged.resize(n, 0);
  a.last_v.resize(n, -1.0f);
  a.aux.resize(n, -1);
  a.held.resize(a.map->modifiers.size(), std::array<bool, kMaxDecks + 1>{});
}

void ControllerHub::devices_changed() {
  if (!io_) return;
  const std::vector<std::string> names = io_->devices();
  std::lock_guard<std::mutex> lock(mutex_);
  // Drop controllers that went away.
  active_.erase(std::remove_if(active_.begin(), active_.end(),
                               [&](const Active& a) {
                                 const bool gone = std::find(names.begin(), names.end(), a.device) == names.end();
                                 if (gone) DH_LOG(Info, "controller disconnected: %s", a.device.c_str());
                                 return gone;
                               }),
                active_.end());
  for (const std::string& device : names) {
    if (std::any_of(active_.begin(), active_.end(), [&](const Active& a) { return a.device == device; })) continue;
    const std::string low = lower(device);
    Mapping* found = nullptr;
    for (Mapping& m : mappings_) {
      if (m.generated ? m.name != device : low.find(m.match) == std::string::npos) continue;
      found = &m;
      break;
    }
    if (!found) {  // unknown device: an empty mapping so MIDI learn works on it
      Mapping g;
      g.name = device;
      g.match = low;
      g.generated = true;
      mappings_.push_back(std::move(g));
      found = &mappings_.back();
      DH_LOG(Info, "MIDI device without a mapping: %s (MIDI learn available)", device.c_str());
    }
    Active a;
    a.device = device;
    a.map = found;
    fit(a);
    if (!found->generated) {
      for (const auto& msg : found->init) io_->send(device, msg.data(), msg.size());
      DH_LOG(Info, "controller connected: %s (mapping \"%s\")", device.c_str(), found->name.c_str());
    }
    active_.push_back(std::move(a));
  }
}

std::vector<std::string> ControllerHub::connected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  for (const Active& a : active_)
    if (!a.map->generated) out.push_back(a.map->name);
  return out;
}

bool ControllerHub::audio_hint(std::string& device_match, int& master_channel, int& phones_channel) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Active& a : active_) {
    if (a.map->audio_match.empty()) continue;
    device_match = a.map->audio_match;
    master_channel = a.map->master_channel;
    phones_channel = a.map->phones_channel;
    return true;
  }
  return false;
}

void ControllerHub::midi(const std::string& device, const std::uint8_t* data, std::size_t size) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = std::find_if(active_.begin(), active_.end(), [&](const Active& a) { return a.device == device; });
  std::size_t i = 0;
  while (i < size) {
    const std::uint8_t status = data[i];
    if (status < 0x80) {  // stray data byte
      ++i;
      continue;
    }
    const std::uint8_t type = status & 0xF0;
    if (status == 0xF0) {  // system exclusive: skip to its end
      while (i < size && data[i] != 0xF7) ++i;
      ++i;
      continue;
    }
    const std::size_t len = (type == 0xC0 || type == 0xD0) ? 2 : (type == 0xF0 ? 1 : 3);
    if (i + len > size) break;
    if (len == 3) {
      DH_LOG(Debug, "midi in  %s: %02X %02X %02X", device.c_str(), status, data[i + 1], data[i + 2]);
      const bool learning = learn_->state == LearnState::Waiting || learn_->state == LearnState::Observing;
      if (learning) learn_feed(device, status, data[i + 1], data[i + 2]);
      else if (it != active_.end()) handle(*it, status, data[i + 1], data[i + 2]);
    }
    i += len;
  }
}

bool ControllerHub::held(const Active& a, const std::string& name, int deck) const {
  for (std::size_t i = 0; i < a.map->modifiers.size(); ++i)
    if (a.map->modifiers[i] == name) return a.held[i][0] || a.held[i][1 + deck];
  return false;
}

bool ControllerHub::allowed(const Active& a, const InRule& r) const {
  if (r.mode >= 0 && r.action != Action::PadMode && a.pad_mode[r.deck] != r.mode) return false;
  if (r.when >= 0) {
    const bool on = a.held[r.when][0] || a.held[r.when][1 + r.deck];
    if (on == r.when_not) return false;
  }
  return true;
}

void ControllerHub::handle(Active& a, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
  if ((status & 0xF0) == 0x80) {  // note-off is a release of the same note
    status = static_cast<std::uint8_t>(0x90 | (status & 0x0F));
    d2 = 0;
  }
  const auto range = a.map->index.equal_range(static_cast<std::uint16_t>(status << 8 | d1));
  for (auto it = range.first; it != range.second; ++it) {
    const std::size_t idx = it->second;
    const InRule& r = a.map->in[idx];
    switch (r.kind) {
      case InKind::Button: {
        const bool pressed = d2 > 0;
        const bool filtered = r.when >= 0 || (r.mode >= 0 && r.action != Action::PadMode);
        if (pressed) {
          if (!allowed(a, r)) break;
          a.down[idx] = 1;
        } else {
          const bool was = a.down[idx] != 0;
          a.down[idx] = 0;
          if (filtered && !was) break;  // its press was routed elsewhere (other layer or mode)
        }
        button(a, idx, r, pressed);
        break;
      }
      case InKind::Abs:
        if (allowed(a, r)) value(a, idx, r, static_cast<float>(d2) / 127.0f);
        break;
      case InKind::Abs14: {
        (d1 == r.data1 ? a.msb : a.lsb)[idx] = d2;
        if (allowed(a, r)) value(a, idx, r, static_cast<float>((a.msb[idx] << 7) | a.lsb[idx]) / 16383.0f);
        break;
      }
      case InKind::Rel: if (allowed(a, r)) delta(a, r, static_cast<int>(d2) - 0x40); break;
      case InKind::Rel2: if (allowed(a, r)) delta(a, r, d2 < 0x40 ? d2 : static_cast<int>(d2) - 0x80); break;
    }
  }
}

FxSlotView& ControllerHub::fx_slot(bool master, int deck, int slot) {
  return master ? cache_.mixer.fx[slot] : cache_.deck[deck].fx[slot];
}

void ControllerHub::button(Active& a, std::size_t idx, const InRule& r, bool pressed) {
  Command c;
  c.deck = static_cast<std::uint8_t>(r.deck);
  const auto send = [&](Op op) {
    c.op = op;
    sink_.command(c);
  };
  const auto press = [&](Op op) {
    if (pressed) send(op);
  };
  DeckView& dv = cache_.deck[r.deck];
  // on/off flags: toggle by default, set with value=0|1, momentary with hold=1
  const auto flag = [&](Op op, bool& cur) {
    if (r.hold) {
      cur = pressed;
      c.i = pressed ? 1 : 0;
      send(op);
    } else if (pressed) {
      c.i = r.has_value ? (r.value > 0 ? 1 : 0) : -1;
      cur = c.i < 0 ? !cur : c.i == 1;
      send(op);
    }
  };
  const bool fx_master = r.master_unit;
  const std::uint8_t fx_deck = static_cast<std::uint8_t>(fx_master ? kFxMaster : r.deck);
  const int sampler_slot = r.slot + r.offset;
  switch (r.action) {
    case Action::Play: press(Op::PlayToggle); break;
    case Action::Cue: c.op = pressed ? Op::CueDown : Op::CueUp; sink_.command(c); break;
    case Action::Sync: press(Op::SyncToggle); break;
    case Action::SeekStart: c.d = 0; press(Op::Seek); break;
    case Action::TempoRange:
      if (pressed) {
        a.range_index[r.deck] = (a.range_index[r.deck] + 1) % 4;
        c.f = kTempoRanges[a.range_index[r.deck]];
        press(Op::TempoRange);
      }
      break;
    case Action::JogTouch:
      c.op = Op::JogTouch;
      c.i = pressed ? 1 : 0;
      sink_.command(c);
      if (!pressed && a.jog_ticks[r.deck] != 0) {
        DH_LOG(Debug, "jog deck %d: %.0f ticks while touched (one full turn should equal ticks_per_rev)",
               r.deck + 1, a.jog_ticks[r.deck]);
      }
      a.jog_ticks[r.deck] = 0;
      break;
    case Action::LoopIn: press(Op::LoopIn); break;
    case Action::LoopOut: press(Op::LoopOut); break;
    case Action::LoopToggle: c.f = r.amount; press(Op::LoopToggle); break;
    case Action::Reloop: press(Op::Reloop); break;
    case Action::CueCall: c.i = r.amount < 0 ? -1 : 1; press(Op::CueCall); break;
    case Action::BeatLoop:
      c.f = r.has_beats ? r.amount : 0.25f * std::pow(2.0f, static_cast<float>(r.slot));
      press(Op::BeatLoop);
      break;
    case Action::BeatJump:
      c.f = r.has_beats ? r.amount
                        : (r.slot % 2 == 0 ? -1.0f : 1.0f) * std::pow(2.0f, static_cast<float>(r.slot / 2));
      press(Op::BeatJump);
      break;
    case Action::HotCue:
      c.op = pressed ? Op::HotCueDown : Op::HotCueUp;
      c.i = r.slot;
      if (pressed) a.last_cue[r.deck] = r.slot;
      sink_.command(c);
      break;
    case Action::HotCueClear: c.i = r.slot; press(Op::HotCueClear); break;
    case Action::Pfl: c.i = -1; press(Op::Pfl); break;
    case Action::BrowsePress: if (pressed) sink_.browse_press(); break;
    case Action::Load: if (pressed) sink_.load_selected(r.deck); break;
    case Action::PadMode:
      if (pressed && a.pad_mode[r.deck] != r.mode) {
        a.pad_mode[r.deck] = r.mode;
        for (std::size_t i = 0; i < a.map->in.size(); ++i)  // soft takeover starts over on the new layer
          if (a.map->in[i].mode >= 0) a.engaged[i] = 0, a.last_v[i] = -1.0f;
      }
      break;
    case Action::Modifier: {
      a.held[r.modifier][r.has_deck ? 1 + r.deck : 0] = pressed;
      for (std::size_t i = 0; i < a.map->in.size(); ++i)
        if (a.map->in[i].when == r.modifier) a.engaged[i] = 0, a.last_v[i] = -1.0f;
      break;
    }
    case Action::KeyLock: flag(Op::KeyLock, dv.key_lock); break;
    case Action::Quantize: flag(Op::Quantize, dv.quantize); break;
    case Action::Slip: flag(Op::Slip, dv.slip); break;
    case Action::Reverse: flag(Op::Reverse, dv.reverse); break;
    case Action::LoopMove: c.f = r.amount; press(Op::LoopMove); break;
    case Action::LoopExit: press(Op::LoopExit); break;
    case Action::LoopHalve: c.f = 0.5f; press(Op::LoopScale); break;
    case Action::LoopDouble: c.f = 2.0f; press(Op::LoopScale); break;
    case Action::AutoGain: if (pressed) { c.f = r.value; send(Op::AutoGain); } break;
    case Action::ColorFx:
      if (pressed) {
        const int n = static_cast<int>(ColorFx::Count);
        c.i = r.effect >= 0 ? r.effect : (dv.color_fx + (r.amount < 0 ? n - 1 : 1)) % n;
        dv.color_fx = c.i;
        send(Op::ColorFx);
      }
      break;
    case Action::FxSelect:
      if (pressed) {
        FxSlotView& fx = fx_slot(fx_master, r.deck, r.slot);
        const int kind = r.effect >= 0 ? r.effect : next_effect(static_cast<int>(fx.kind), r.amount < 0 ? -1 : 1);
        fx.kind = static_cast<EffectKind>(kind);
        c.deck = fx_deck;
        c.i = r.slot;
        c.d = kind;
        send(Op::FxSelect);
      }
      break;
    case Action::FxOn: {
      FxSlotView& fx = fx_slot(fx_master, r.deck, r.slot);
      bool on = fx.on;
      if (!r.hold && !pressed) break;
      if (r.hold) on = pressed;
      else on = r.has_value ? r.value > 0 : !fx.on;
      fx.on = on;
      c.deck = fx_deck;
      c.i = r.slot;
      c.f = on ? 1.0f : 0.0f;
      send(Op::FxEnable);
      break;
    }
    case Action::FxTime:
      if (pressed) {
        FxSlotView& fx = fx_slot(fx_master, r.deck, r.slot);
        fx.beats = r.has_beats ? r.amount : step_beats(fx.beats, r.amount < 0 ? -1 : 1);
        c.deck = fx_deck;
        c.i = r.slot;
        c.f = fx.beats;
        send(Op::FxTime);
      }
      break;
    case Action::PadFx: {
      FxSlotView& fx = fx_slot(fx_master, r.deck, r.slot);
      c.deck = fx_deck;
      c.i = r.slot;
      if (pressed) {
        static const EffectKind kDefaults[8] = {EffectKind::Echo,      EffectKind::PingPong, EffectKind::Roll,
                                                EffectKind::Transform, EffectKind::Flanger,  EffectKind::Phaser,
                                                EffectKind::VinylBrake, EffectKind::Backspin};
        const int pad = r.pad >= 0 ? r.pad : (r.has_slot ? r.slot : 0);
        const EffectKind kind = r.effect >= 0 ? static_cast<EffectKind>(r.effect) : kDefaults[pad % 8];
        const EffectInfo* info = effect_info(kind);
        if (fx.kind != kind) {
          c.d = static_cast<double>(kind);
          send(Op::FxSelect);
          fx.kind = kind;
        }
        c.f = r.has_beats ? r.amount : (info ? info->default_beats : 1.0f);
        fx.beats = c.f;
        send(Op::FxTime);
        c.f = 1.0f;
        fx.on = true;
        send(Op::FxEnable);
      } else {
        c.f = 0.0f;
        fx.on = false;
        send(Op::FxEnable);
      }
      break;
    }
    case Action::Sampler:
      if (pressed) {
        c.deck = static_cast<std::uint8_t>(sampler_slot);
        if (held(a, "shift", r.deck)) {
          c.i = 0;
          send(Op::SamplerStop);
        } else {
          c.f = 0;  // full velocity
          send(Op::SamplerTrigger);
        }
      }
      break;
    case Action::SamplerStop:
      if (pressed) {
        c.deck = static_cast<std::uint8_t>(r.has_slot ? sampler_slot : 0);
        c.i = r.has_slot ? 0 : -1;
        send(Op::SamplerStop);
      }
      break;
    case Action::MasterBpm:
      if (pressed) {
        c.deck = 0;
        c.f = r.value;
        cache_.mixer.master_bpm = r.value;
        send(Op::MasterBpm);
      }
      break;
    case Action::KeyShift:
    case Action::KeyShiftPad:
    case Action::Keyboard: {
      if (r.action == Action::Keyboard) {
        if (!pressed) {  // let go of the cue the pad started
          if (a.aux[idx] >= 0) {
            c.op = Op::HotCueUp;
            c.i = a.aux[idx];
            sink_.command(c);
          }
          a.aux[idx] = -1;
          break;
        }
      } else if (!pressed) {
        break;
      }
      float semis = dv.key_shift;
      if (r.has_value) semis = r.value;
      else if (r.action == Action::Keyboard) semis = static_cast<float>(r.slot);
      else if (r.action == Action::KeyShiftPad) semis = static_cast<float>(r.slot - 4);
      else semis += r.amount < 0 ? -1.0f : 1.0f;
      semis = std::max(-12.0f, std::min(12.0f, semis));
      dv.key_shift = semis;
      c.op = Op::KeyShift;
      c.f = semis;
      sink_.command(c);
      if (r.action == Action::Keyboard && a.last_cue[r.deck] >= 0) {
        c.op = Op::HotCueDown;
        c.i = a.last_cue[r.deck];
        a.aux[idx] = c.i;
        sink_.command(c);
      }
      break;
    }
    default: break;
  }
}

bool ControllerHub::engine_value(const InRule& r, float& e) const {
  const DeckView& d = cache_.deck[r.deck];
  const bool hd = cache_.have_deck[r.deck], hm = cache_.have_mixer;
  const MixerView& m = cache_.mixer;
  const auto span = [&](float x) {
    if (r.hi == r.lo) return false;
    e = (x - r.lo) / (r.hi - r.lo);
    return true;
  };
  switch (r.action) {
    case Action::Tempo: if (!hd || d.tempo_range <= 0) return false; e = static_cast<float>(d.tempo / d.tempo_range) * 0.5f + 0.5f; break;
    case Action::Trim: if (!hd) return false; e = d.trim; break;
    case Action::EqHigh: if (!hd) return false; e = d.eq_high; break;
    case Action::EqMid: if (!hd) return false; e = d.eq_mid; break;
    case Action::EqLow: if (!hd) return false; e = d.eq_low; break;
    case Action::Filter: if (!hd) return false; e = d.filter; break;
    case Action::Fader: if (!hd) return false; e = d.fader; break;
    case Action::Crossfader: if (!hm) return false; e = m.crossfader; break;
    case Action::HeadMix: if (!hm) return false; e = m.head_mix; break;
    case Action::HeadLevel: if (!hm) return false; e = m.head_level; break;
    case Action::MasterLevel: if (!hm) return false; e = m.master_level; break;
    case Action::SamplerLevel: if (!hm) return false; e = m.sampler_level; break;
    case Action::FxWet:
      if (r.master_unit ? !hm : !hd) return false;
      e = (r.master_unit ? m.fx[r.slot] : d.fx[r.slot]).wet;
      break;
    case Action::FxParam:
      if (r.master_unit ? !hm : !hd) return false;
      e = (r.master_unit ? m.fx[r.slot] : d.fx[r.slot]).param;
      break;
    case Action::AutoGain: if (!hd) return false; if (!span(d.auto_gain_db)) return false; break;
    case Action::KeyShift: if (!hd) return false; if (!span(d.key_shift)) return false; break;
    case Action::MasterBpm: if (!hm || m.master_bpm <= 0) return false; if (!span(static_cast<float>(m.master_bpm))) return false; break;
    default: return false;
  }
  e = std::max(0.0f, std::min(1.0f, e));
  return true;
}

void ControllerHub::value(Active& a, std::size_t idx, const InRule& r, float v) {
  if (r.invert) v = 1.0f - v;
  const float dz = r.deadzone >= 0 ? r.deadzone : a.map->deadzone_default;
  if (dz > 0) {  // centre detent: a flat zone around 0.5, rescaled so the ends stay reachable
    const float off = v - 0.5f;
    v = std::fabs(off) <= dz ? 0.5f : 0.5f + (off > 0 ? 1.0f : -1.0f) * (std::fabs(off) - dz) / (0.5f - dz) * 0.5f;
  }
  const bool pickup = r.pickup >= 0 ? r.pickup == 1 : a.map->pickup_default;
  if (pickup && !a.engaged[idx]) {
    float e = 0;
    if (engine_value(r, e)) {
      const float prev = a.last_v[idx];
      a.last_v[idx] = v;
      const bool near = std::fabs(v - e) <= 0.02f;
      const bool crossed = prev >= 0 && (prev - e) * (v - e) <= 0;
      if (!near && !crossed) return;
      a.engaged[idx] = 1;
    }  // engine value unknown (no tick yet): pass the value through and stay unengaged
  }
  a.last_v[idx] = v;

  Command c;
  c.deck = static_cast<std::uint8_t>(r.deck);
  c.f = v;
  const auto ranged = [&](float lo, float hi) { return (r.has_range ? r.lo : lo) + v * ((r.has_range ? r.hi : hi) - (r.has_range ? r.lo : lo)); };
  switch (r.action) {
    case Action::Tempo: c.op = Op::Tempo; c.f = v * 2.0f - 1.0f; break;
    case Action::Trim: c.op = Op::Trim; break;
    case Action::EqHigh: c.op = Op::EqHigh; break;
    case Action::EqMid: c.op = Op::EqMid; break;
    case Action::EqLow: c.op = Op::EqLow; break;
    case Action::Filter: c.op = Op::Filter; break;
    case Action::Fader: c.op = Op::Fader; break;
    case Action::Crossfader: c.op = Op::Crossfader; break;
    case Action::HeadMix: c.op = Op::HeadMix; break;
    case Action::HeadLevel: c.op = Op::HeadLevel; break;
    case Action::MasterLevel: c.op = Op::MasterLevel; break;
    case Action::SamplerLevel: c.op = Op::SamplerLevel; break;
    case Action::AutoGain: c.op = Op::AutoGain; c.f = ranged(-24.0f, 12.0f); break;
    case Action::KeyShift: c.op = Op::KeyShift; c.f = std::round(ranged(-12.0f, 12.0f)); cache_.deck[r.deck].key_shift = c.f; break;
    case Action::MasterBpm: c.op = Op::MasterBpm; c.deck = 0; c.f = ranged(60.0f, 180.0f); cache_.mixer.master_bpm = c.f; break;
    case Action::FxWet: case Action::FxParam:
      c.op = r.action == Action::FxWet ? Op::FxWet : Op::FxParam;
      c.deck = static_cast<std::uint8_t>(r.master_unit ? kFxMaster : r.deck);
      c.i = r.slot;
      break;
    default: return;
  }
  sink_.command(c);
}

void ControllerHub::delta(Active& a, const InRule& r, int steps) {
  if (steps == 0) return;
  const int sign = steps > 0 ? 1 : -1;
  Command c;
  c.deck = static_cast<std::uint8_t>(r.deck);
  const std::uint8_t fx_deck = static_cast<std::uint8_t>(r.master_unit ? kFxMaster : r.deck);
  switch (r.action) {
    case Action::Browse: sink_.browse(static_cast<int>(std::lround(steps * r.amount))); break;
    case Action::Jog:
      a.jog_ticks[r.deck] += steps;
      c.op = Op::JogTurn;
      c.i = r.top ? 1 : 0;
      c.d = static_cast<double>(steps) * r.amount / a.map->ticks_per_rev;
      sink_.command(c);
      break;
    case Action::FxSelect: {
      FxSlotView& fx = fx_slot(r.master_unit, r.deck, r.slot);
      fx.kind = static_cast<EffectKind>(next_effect(static_cast<int>(fx.kind), sign));
      c.op = Op::FxSelect;
      c.deck = fx_deck;
      c.i = r.slot;
      c.d = static_cast<double>(fx.kind);
      sink_.command(c);
      break;
    }
    case Action::FxTime: {
      FxSlotView& fx = fx_slot(r.master_unit, r.deck, r.slot);
      fx.beats = step_beats(fx.beats, sign);
      c.op = Op::FxTime;
      c.deck = fx_deck;
      c.i = r.slot;
      c.f = fx.beats;
      sink_.command(c);
      break;
    }
    case Action::FxWet: case Action::FxParam: {  // 1 % per tick, times scale=
      FxSlotView& fx = fx_slot(r.master_unit, r.deck, r.slot);
      float& v = r.action == Action::FxWet ? fx.wet : fx.param;
      v = std::max(0.0f, std::min(1.0f, v + steps * r.amount * 0.01f));
      c.op = r.action == Action::FxWet ? Op::FxWet : Op::FxParam;
      c.deck = fx_deck;
      c.i = r.slot;
      c.f = v;
      sink_.command(c);
      break;
    }
    case Action::KeyShift: {  // one semitone per tick, times scale=
      float& k = cache_.deck[r.deck].key_shift;
      k = std::max(-12.0f, std::min(12.0f, k + steps * r.amount));
      c.op = Op::KeyShift;
      c.f = k;
      sink_.command(c);
      break;
    }
    case Action::MasterBpm: {  // one BPM per tick, times scale=
      double& b = cache_.mixer.master_bpm;
      b = std::max(20.0, std::min(300.0, (b > 0 ? b : 120.0) + steps * r.amount));
      c.op = Op::MasterBpm;
      c.deck = 0;
      c.f = static_cast<float>(b);
      sink_.command(c);
      break;
    }
    case Action::LoopMove:
      c.op = Op::LoopMove;
      c.f = static_cast<float>(sign) * r.amount;
      sink_.command(c);
      break;
    default: break;
  }
}

void ControllerHub::tick(const DeckView* decks, int deck_count, double now) {
  tick(decks, deck_count, nullptr, nullptr, 0, now);
}

void ControllerHub::tick(const DeckView* decks, int deck_count, const MixerView* mixer, const SamplerView* samplers,
                         int sampler_count, double now) {
  if (!io_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < deck_count && i < kMaxDecks; ++i) {
    cache_.deck[i] = decks[i];
    cache_.have_deck[i] = true;
  }
  if (mixer) {
    cache_.mixer = *mixer;
    cache_.have_mixer = true;
  }
  const bool blink_on = std::fmod(now, 0.5) < 0.25;
  const DeckView blank;
  std::map<std::uint16_t, int> want;
  for (Active& a : active_) {
    want.clear();
    for (const OutRule& r : a.map->out) {
      const bool deckless = r.master_unit || r.state == LedState::Modifier || r.state == LedState::SamplerPlaying;
      if (r.deck >= deck_count && !deckless) continue;
      const DeckView& d = r.deck < deck_count ? decks[r.deck] : blank;
      const std::uint16_t addr = static_cast<std::uint16_t>(r.status << 8 | r.data1);
      if (r.mode >= 0 && r.state != LedState::PadMode && a.pad_mode[r.deck] != r.mode) {
        want.emplace(addr, r.off);  // belongs to another pad mode: dark unless an active rule claims it
        continue;
      }
      int byte = 0;
      if (r.state == LedState::Level) {
        const float v = std::min(1.0f, d.level);
        byte = v < 0.02f ? 0 : r.off + static_cast<int>(std::lround((r.on - r.off) * v));
      } else {
        bool on = false;
        switch (r.state) {
          case LedState::Playing: on = d.playing || (r.blink && d.loaded && blink_on); break;
          case LedState::Cue: on = d.loaded && (d.at_cue || d.playing || !r.blink || blink_on); break;
          case LedState::Sync: on = d.sync; break;
          case LedState::Pfl: on = d.pfl; break;
          case LedState::Loop: on = d.loop_active; break;
          case LedState::LoopIn: on = d.loop_in >= 0; break;
          case LedState::HotCue: on = d.hot_cue[r.slot] >= 0; break;
          case LedState::Loaded: on = d.loaded; break;
          case LedState::PadMode: on = a.pad_mode[r.deck] == r.mode; break;
          case LedState::KeyLock: on = d.key_lock; break;
          case LedState::Quantize: on = d.quantize; break;
          case LedState::Slip: on = d.slip; break;
          case LedState::Reverse: on = d.reverse; break;
          case LedState::FxOn: {
            const FxSlotView* fx = r.master_unit ? (mixer ? &mixer->fx[r.slot] : nullptr) : &d.fx[r.slot];
            on = fx && fx->on && (r.effect < 0 || static_cast<int>(fx->kind) == r.effect);
            break;
          }
          case LedState::SamplerPlaying: {
            const int s = r.slot + r.offset;
            on = samplers && s < sampler_count && samplers[s].playing;
            break;
          }
          case LedState::LoopBeats: {
            const double b = loop_beats(d), want_b = r.has_value ? r.value : r.amount;
            on = b > 0 && std::fabs(b - want_b) <= 0.02 * want_b + 0.01;
            break;
          }
          case LedState::KeyShift: {
            const float target = r.has_value ? r.value : static_cast<float>(r.mode == 7 ? r.slot - 4 : r.slot);
            on = std::lround(d.key_shift) == std::lround(target);
            break;
          }
          case LedState::Modifier: on = r.modifier >= 0 && (a.held[r.modifier][0] || a.held[r.modifier][1 + r.deck]); break;
          default: break;
        }
        byte = on ? r.on : r.off;
      }
      want[addr] = byte;
    }
    for (const auto& [addr, byte] : want) {
      const auto last = a.last_out.find(addr);
      if (last != a.last_out.end() && last->second == byte) continue;
      a.last_out[addr] = byte;
      const std::uint8_t msg[3] = {static_cast<std::uint8_t>(addr >> 8), static_cast<std::uint8_t>(addr & 0xFF),
                                   static_cast<std::uint8_t>(byte)};
      io_->send(a.device, msg, 3);
    }
  }
}

// ---- MIDI learn -----------------------------------------------------------------------
bool ControllerHub::learn_begin(const std::string& action) {
  std::lock_guard<std::mutex> lock(mutex_);
  Learn& l = *learn_;
  l = Learn{};
  const std::vector<std::string> t = tokens(action);
  int act = 0;
  Mapping probe;
  std::string why;
  if (t.empty() || !lookup(kActions, t[0], act)) {
    l.state = LearnState::Failed;
    l.error = "unknown action";
    return false;
  }
  // The control part is a placeholder; this only checks the action and its arguments.
  if (!parse_line(probe, "in button 90 00 " + action, why)) {
    l.state = LearnState::Failed;
    l.error = why;
    return false;
  }
  l.action = static_cast<Action>(act);
  l.action_text = action;
  l.state = LearnState::Waiting;
  return true;
}

void ControllerHub::learn_cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  *learn_ = Learn{};
}

LearnState ControllerHub::learn_state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return learn_->state;
}

std::string ControllerHub::learn_device() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return learn_->device;
}

std::string ControllerHub::learn_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return learn_->error;
}

std::string ControllerHub::learn_result(bool finalize) {
  std::lock_guard<std::mutex> lock(mutex_);
  Learn& l = *learn_;
  if (l.state == LearnState::Observing &&
      (finalize || std::chrono::steady_clock::now() - l.last > std::chrono::milliseconds(400)))
    learn_finish();
  return l.state == LearnState::Done ? l.line : std::string();
}

bool ControllerHub::learn_feed(const std::string& device, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
  Learn& l = *learn_;
  const std::uint8_t type = status & 0xF0;
  const Class cls = action_class(l.action);
  const auto button_line = [&] {
    char buf[32];
    std::snprintf(buf, sizeof buf, "in button %02X %02X ", status, d1);
    l.device = device;
    l.line = buf + l.action_text;
    l.state = LearnState::Done;
    Mapping probe;
    std::string why;
    if (cls == Class::Value || cls == Class::Delta || !parse_line(probe, l.line, why)) {
      l.state = LearnState::Failed;
      l.error = cls == Class::Value ? "this action needs a fader or knob, not a button"
                : cls == Class::Delta ? "this action needs an encoder, not a button" : why;
      l.line.clear();
    }
  };
  if (type == 0x90 && d2 > 0 && l.state == LearnState::Waiting) {
    button_line();
  } else if (type == 0xB0) {
    if (l.state == LearnState::Waiting) {
      if (cls == Class::Button) {
        if (d2 > 0) button_line();  // a button that sends control changes
        return true;
      }
      l.device = device;
      l.status = status;
      l.state = LearnState::Observing;
    } else if (device != l.device || status != l.status) {
      return true;
    } else {
      bool known = false, partner = false;
      for (const auto& s : l.samples) {
        known = known || s.first == d1;
        partner = partner || (s.first + 32 == d1 && s.first < 32) || (d1 + 32 == s.first && d1 < 32);
      }
      if (!known && !partner) return true;  // some other control: not the one being learned
    }
    l.samples.emplace_back(d1, d2);
    l.last = std::chrono::steady_clock::now();
    if (l.samples.size() >= 12) learn_finish();
  }
  return true;
}

void ControllerHub::learn_finish() {
  Learn& l = *learn_;
  const Class cls = action_class(l.action);
  std::vector<int> ids;
  for (const auto& s : l.samples)
    if (std::find(ids.begin(), ids.end(), s.first) == ids.end()) ids.push_back(s.first);
  int msb = -1;
  if (cls != Class::Delta)
    for (int id : ids)
      if (id < 32 && std::find(ids.begin(), ids.end(), id + 32) != ids.end()) msb = id;
  int lo = 127, hi = 0;
  bool near_centre = true, near_ends = true;
  for (const auto& s : l.samples) {
    if (msb >= 0 && s.first != ids[0] && s.first != msb) continue;
    lo = std::min(lo, s.second);
    hi = std::max(hi, s.second);
    near_centre = near_centre && std::abs(s.second - 0x40) <= 16;
    near_ends = near_ends && (s.second <= 16 || s.second >= 112);
  }
  const char* kind = "abs";
  if (msb >= 0) kind = "abs14";
  else if (cls == Class::Value) kind = "abs";
  else if (cls == Class::Delta) kind = near_ends && !near_centre ? "rel2" : "rel";
  else if (hi - lo >= 24) kind = "abs";
  else if (near_centre) kind = "rel";
  else if (near_ends) kind = "rel2";
  char buf[48];
  if (msb >= 0) std::snprintf(buf, sizeof buf, "in abs14 %02X %02X %02X ", l.status, msb, msb + 32);
  else std::snprintf(buf, sizeof buf, "in %s %02X %02X ", kind, l.status, ids.empty() ? 0 : ids[0]);
  l.line = buf + l.action_text;
  Mapping probe;
  std::string why;
  if (!parse_line(probe, l.line, why)) {
    l.state = LearnState::Failed;
    l.error = why;
    l.line.clear();
  } else {
    l.state = LearnState::Done;
  }
}

bool ControllerHub::learn_commit() {
  std::lock_guard<std::mutex> lock(mutex_);
  Learn& l = *learn_;
  if (l.state != LearnState::Done) return false;
  const auto it = std::find_if(active_.begin(), active_.end(), [&](const Active& a) { return a.device == l.device; });
  if (it == active_.end()) return false;
  std::string why;
  if (!parse_line(*it->map, l.line, why)) return false;
  for (Active& a : active_)
    if (a.map == it->map) fit(a);
  *learn_ = Learn{};
  return true;
}

}  // namespace deckhand
