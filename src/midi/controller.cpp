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

// The engine commands for the microphone channel (Op::MicOn, MicLevel, MicEqLow, MicEqHigh, MicTalkover) arrive
// with the microphone work; until engine.hpp defines the marker DECKHAND_ENGINE_HAS_MIC the mic actions parse but
// send nothing.
#if defined(DECKHAND_ENGINE_HAS_MIC)
#define DECKHAND_HAVE_MIC_OPS 1
#endif

namespace deckhand {

bool hid_usage_refused(int usage_page, int usage) {
  if (usage_page == 0x07) return true;  // keyboard / keypad page
  if (usage_page == 0x01) return usage == 0x01 || usage == 0x02 || usage == 0x06 || usage == 0x07;  // pointer, mouse, keyboard, keypad
  return false;
}

bool hid_matches(const HidMatch& m, const HidDeviceInfo& d) {
  return (m.vid < 0 || m.vid == d.vid) && (m.pid < 0 || m.pid == d.pid) &&
         (m.usage_page < 0 || m.usage_page == d.usage_page) && (m.usage < 0 || m.usage == d.usage) &&
         (m.interface_number < 0 || m.interface_number == d.interface_number);
}

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
    {"padfx", int(Action::PadFx)}, {"modifier", int(Action::Modifier)}, {"fx_assign", int(Action::FxAssign)},
    {"master_cue", int(Action::MasterCue)}, {"mic_on", int(Action::MicOn)}, {"mic_level", int(Action::MicLevel)},
    {"mic_eq_low", int(Action::MicEqLow)}, {"mic_eq_high", int(Action::MicEqHigh)},
    {"mic_talkover", int(Action::MicTalkover)},
};
const Named kStates[] = {
    {"playing", int(LedState::Playing)}, {"cue", int(LedState::Cue)}, {"sync", int(LedState::Sync)},
    {"pfl", int(LedState::Pfl)}, {"loop", int(LedState::Loop)}, {"loop_in", int(LedState::LoopIn)},
    {"hotcue", int(LedState::HotCue)}, {"loaded", int(LedState::Loaded)}, {"pad_mode", int(LedState::PadMode)},
    {"level", int(LedState::Level)}, {"key_lock", int(LedState::KeyLock)}, {"quantize", int(LedState::Quantize)},
    {"slip", int(LedState::Slip)}, {"reverse", int(LedState::Reverse)}, {"fx_on", int(LedState::FxOn)},
    {"sampler_playing", int(LedState::SamplerPlaying)}, {"loop_beats", int(LedState::LoopBeats)},
    {"key_shift", int(LedState::KeyShift)}, {"modifier", int(LedState::Modifier)}, {"fx_assign", int(LedState::FxAssign)},
    {"master_cue", int(LedState::MasterCue)}, {"color_fx", int(LedState::ColorFx)},
    {"mic_on", int(LedState::MicOn)}, {"mic_talkover", int(LedState::MicTalkover)},
};
const Named kModes[] = {{"hotcue", 0}, {"padfx1", 1}, {"beatjump", 2}, {"sampler", 3},
                        {"keyboard", 4}, {"padfx2", 5}, {"beatloop", 6}, {"keyshift", 7}};
const Named kInKinds[] = {{"button", int(InKind::Button)}, {"abs", int(InKind::Abs)}, {"abs14", int(InKind::Abs14)},
                          {"rel", int(InKind::Rel)}, {"rel2", int(InKind::Rel2)}};
const Named kHidKinds[] = {{"button", int(InKind::HidButton)}, {"touch", int(InKind::HidButton)},
                           {"abs", int(InKind::HidAbs)}, {"rel", int(InKind::HidRel)}, {"delta", int(InKind::HidDelta)}};
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
  int fx_unit = -1;
  float amount = 1, value = 0, lo = 0, hi = 1, deadzone = -1;
  int pickup = -1, pad = -1, offset = 0;
  int on = 0x7F, off = 0, min = -1, max = -1;
  bool has_on = false;
  std::string min_s, max_s;  // as written (MIDI out: hex byte; HID in: a number)
  std::string match, effect, when, name, target, init;
  // HID
  int report = -1, byte = -1, bit_lo = -1, bit_hi = -1, size = -1, endian = -1;
  bool is_signed = false;
  double interval = -1;
  long vid = -1, pid = -1, usage_page = -1, usage = -1, interface_number = -1;
  int master = -1, phones = -1;
  double ticks_per_rev = -1;
};

bool dec_int(const std::string& s, long& out) {  // decimal, or 0x-prefixed hex
  if (s.empty()) return false;
  char* end = nullptr;
  out = std::strtol(s.c_str(), &end, 0);
  return *end == '\0';
}

bool hex_int(const std::string& s, long& out) {  // hex, up to 8 digits
  if (s.empty() || s.size() > 8) return false;
  char* end = nullptr;
  out = std::strtol(s.c_str(), &end, 16);
  return *end == '\0' && out >= 0;
}

bool parse_args(const std::vector<std::string>& t, std::size_t from, Args& a, std::string& error,
                std::size_t to = std::string::npos) {
  for (std::size_t i = from; i < t.size() && i < to; ++i) {
    const auto eq = t[i].find('=');
    if (eq == std::string::npos) {
      error = "expected key=value, got '" + t[i] + "'";
      return false;
    }
    const std::string k = t[i].substr(0, eq), v = t[i].substr(eq + 1);
    const float num = static_cast<float>(std::atof(v.c_str()));
    std::uint8_t b = 0;
    long n = 0;
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
      const int n = std::atoi(v.c_str());
      if (v == "assigned") { a.fx_unit = 0; a.master_unit = false; }
      else if (n >= 1 && n <= kFxUnits && v == std::to_string(n)) { a.fx_unit = n - 1; a.master_unit = false; }
      else if (v == "deck" || v == "master") { a.master_unit = v == "master"; a.fx_unit = -1; }
      else { error = "unit must be deck, master, assigned or 1.." + std::to_string(kFxUnits); return false; }
    }
    else if (k == "target") a.target = v;
    else if (k == "effect") a.effect = v;
    else if (k == "pad") a.pad = std::atoi(v.c_str()) - 1;
    else if (k == "offset") a.offset = std::atoi(v.c_str());
    else if (k == "when") a.when = v;
    else if (k == "name") a.name = v;
    else if (k == "on" && hex_int(v, n)) { a.on = static_cast<int>(n); a.has_on = true; }
    else if (k == "off" && hex_int(v, n)) a.off = static_cast<int>(n);
    else if (k == "min" && !v.empty()) { a.min_s = v; if (hex_byte(v, b)) a.min = b; }
    else if (k == "max" && !v.empty()) { a.max_s = v; if (hex_byte(v, b)) a.max = b; }
    else if (k == "report" && dec_int(v, n)) a.report = static_cast<int>(n);
    else if (k == "byte" && dec_int(v, n)) a.byte = static_cast<int>(n);
    else if (k == "bit" && dec_int(v, n)) a.bit_lo = a.bit_hi = static_cast<int>(n);
    else if (k == "bits") {
      const auto dash = v.find('-');
      long lo = 0, hi = 0;
      if (dash == std::string::npos || !dec_int(v.substr(0, dash), lo) || !dec_int(v.substr(dash + 1), hi)) { error = "bits must be <lo>-<hi>"; return false; }
      a.bit_lo = static_cast<int>(lo);
      a.bit_hi = static_cast<int>(hi);
    }
    else if (k == "size" && dec_int(v, n)) a.size = static_cast<int>(n);
    else if (k == "endian" && (v == "le" || v == "be")) a.endian = v == "be" ? 1 : 0;
    else if (k == "signed") a.is_signed = v == "1";
    else if (k == "interval" && dec_int(v, n) && n >= 0) a.interval = static_cast<double>(n) / 1000.0;
    else if (k == "init") a.init = v;
    else if (k == "vid" && dec_int(v, n)) a.vid = n;
    else if (k == "pid" && dec_int(v, n)) a.pid = n;
    else if (k == "usage_page" && dec_int(v, n)) a.usage_page = n;
    else if (k == "usage" && dec_int(v, n)) a.usage = n;
    else if (k == "interface" && dec_int(v, n)) a.interface_number = n;
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

bool resolve_fx_target(const std::string& name, int& out) {
  if (name == "master") { out = kFxMaster; return true; }
  if (name.size() == 5 && name.compare(0, 4, "deck") == 0 && name[4] >= '1' && name[4] <= '4') {
    out = name[4] - '1';
    return true;
  }
  return false;
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
    case Action::SamplerLevel: case Action::MicLevel: case Action::MicEqLow: case Action::MicEqHigh:
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

bool hex_string(const std::string& s, std::vector<std::uint8_t>& out) {  // "00 1F ff" or "001Fff"
  out.clear();
  int hi = -1;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    const int v = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                  : (std::isxdigit(static_cast<unsigned char>(c)) ? std::tolower(c) - 'a' + 10 : -1);
    if (v < 0) return false;
    if (hi < 0) hi = v;
    else { out.push_back(static_cast<std::uint8_t>(hi << 4 | v)); hi = -1; }
  }
  return hi < 0;
}

// The report field an "in hid" / "out hid" line addresses.
bool make_field(const Args& a, HidField& f, std::string& why, bool input = true) {
  if (a.report < 0 || a.report > 255) { why = "hid rules need report=0..255"; return false; }
  if (a.byte < 0 || a.byte > 4095) { why = "hid rules need byte=0..4095"; return false; }
  int lo = a.bit_lo, hi = a.bit_hi, size = a.size;
  if (size < 0) size = lo >= 0 ? hi / 8 + 1 : 1;
  if (size < 1 || size > 4) { why = "size must be 1..4 bytes"; return false; }
  if (lo < 0) { lo = 0; hi = size * 8 - 1; }
  if (lo > hi || hi >= size * 8) { why = "bit range outside the field"; return false; }
  f.report = a.report;
  f.byte = a.byte;
  f.size = size;
  f.lo = lo;
  f.hi = hi;
  f.big = a.endian == 1;
  f.is_signed = a.is_signed;
  if (input) {
    long n = 0;
    if (!a.min_s.empty()) { if (!dec_int(a.min_s, n)) { why = "bad min= for a HID field"; return false; } f.has_min = true; f.vmin = static_cast<double>(n); }
    if (!a.max_s.empty()) { if (!dec_int(a.max_s, n)) { why = "bad max= for a HID field"; return false; } f.has_max = true; f.vmax = static_cast<double>(n); }
    if (f.has_min && f.has_max && f.vmin == f.vmax) { why = "min and max are equal"; return false; }
  }
  return true;
}

// Reads a field out of a report payload. False when the report is too short.
bool hid_read(const HidField& f, const std::uint8_t* d, std::size_t n, std::int64_t& out) {
  if (!d || static_cast<std::size_t>(f.byte) + static_cast<std::size_t>(f.size) > n) return false;
  std::uint64_t v = 0;
  for (int i = 0; i < f.size; ++i) {
    const std::uint8_t b = d[f.byte + (f.big ? i : f.size - 1 - i)];
    v = v << 8 | b;
  }
  const int w = f.width();
  v = (v >> f.lo) & ((1ULL << w) - 1);
  out = (f.is_signed && ((v >> (w - 1)) & 1)) ? static_cast<std::int64_t>(v) - (1LL << w) : static_cast<std::int64_t>(v);
  return true;
}

// Writes the low bits of `value` into a field of a report payload (other bits are kept).
void hid_write(const HidField& f, std::vector<std::uint8_t>& d, std::uint32_t value) {
  if (static_cast<std::size_t>(f.byte) + static_cast<std::size_t>(f.size) > d.size()) return;
  std::uint64_t v = 0;
  for (int i = 0; i < f.size; ++i) v = v << 8 | d[f.byte + (f.big ? i : f.size - 1 - i)];
  const std::uint64_t mask = ((1ULL << f.width()) - 1) << f.lo;
  v = (v & ~mask) | ((static_cast<std::uint64_t>(value) << f.lo) & mask);
  for (int i = 0; i < f.size; ++i) {
    const int shift = 8 * (f.big ? f.size - 1 - i : i);
    d[f.byte + i] = static_cast<std::uint8_t>(v >> shift);
  }
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
  } else if (head == "hid") {
    Args a;
    if (!parse_args(t, 1, a, why)) return false;
    if (a.vid < 0 || a.vid > 0xFFFF || a.pid < 0 || a.pid > 0xFFFF) { why = "hid needs vid=0x.... and pid=0x...."; return false; }
    if (hid_usage_refused(static_cast<int>(a.usage_page), static_cast<int>(a.usage))) {
      why = "keyboards, keypads, mice and pointers are never opened";
      return false;
    }
    m.hid = true;
    m.hid_match = HidMatch{static_cast<int>(a.vid), static_cast<int>(a.pid), static_cast<int>(a.usage_page),
                           static_cast<int>(a.usage), static_cast<int>(a.interface_number)};
  } else if (head == "hid_out") {
    Args a;
    if (!parse_args(t, 1, a, why)) return false;
    if (a.report < 0 || a.report > 255 || a.size < 1 || a.size > 1024) { why = "hid_out needs report=0..255 and size=1..1024"; return false; }
    HidOutTemplate tpl;
    tpl.report = a.report;
    tpl.size = a.size;
    if (!hex_string(a.init, tpl.init)) { why = "bad init= hex bytes"; return false; }
    if (static_cast<int>(tpl.init.size()) > tpl.size) { why = "init is longer than size"; return false; }
    tpl.init.resize(static_cast<std::size_t>(tpl.size), 0);
    if (a.interval >= 0) tpl.interval = a.interval;
    for (const HidOutTemplate& o : m.hid_out)
      if (o.report == tpl.report) { why = "duplicate hid_out for this report"; return false; }
    m.hid_out.push_back(std::move(tpl));
  } else if (head == "hid_init") {
    // hid_init feature|output report=<id> <hex bytes...>
    HidInit h;
    if (t.size() < 4 || (t[1] != "feature" && t[1] != "output")) { why = "expected: hid_init feature|output report=<id> <bytes>"; return false; }
    h.feature = t[1] == "feature";
    long n = 0;
    if (t[2].compare(0, 7, "report=") != 0 || !dec_int(t[2].substr(7), n) || n < 0 || n > 255) { why = "hid_init needs report=0..255"; return false; }
    h.report = static_cast<int>(n);
    for (std::size_t i = 3; i < t.size(); ++i) {
      std::uint8_t b = 0;
      if (!hex_byte(t[i], b)) { why = "bad hex byte '" + t[i] + "'"; return false; }
      h.data.push_back(b);
    }
    m.hid_init.push_back(std::move(h));
  } else if (head == "in") {
    // in <kind> <status> <data1> [<data1 lsb>] <action> [args]
    // in hid <field args> <kind> <action> [args]
    InRule r;
    int kind = 0, action = 0;
    std::size_t i = 2;
    Args a;
    const bool is_hid = t.size() > 1 && t[1] == "hid";
    if (is_hid) {
      std::size_t k = 2;
      while (k < t.size() && t[k].find('=') != std::string::npos) ++k;
      if (k + 1 >= t.size() || !lookup(kHidKinds, t[k], kind)) {
        why = "expected: in hid report=<id> byte=<n> [bit=|bits=|size= endian=] <button|touch|abs|rel|delta> <action>";
        return false;
      }
      r.kind = static_cast<InKind>(kind);
      if (!parse_args(t, 2, a, why, k)) return false;
      i = k + 1;
      if (!lookup(kActions, t[i], action)) { why = "unknown action"; return false; }
      r.action = static_cast<Action>(action);
      if (!parse_args(t, i + 1, a, why)) return false;
      if (!make_field(a, r.hid, why)) return false;
    } else {
      if (t.size() < 5 || !lookup(kInKinds, t[1], kind)) { why = "expected: in <kind> <status> <data1> <action>"; return false; }
      r.kind = static_cast<InKind>(kind);
      if (!hex_byte(t[i++], r.status) || !hex_byte(t[i++], r.data1)) { why = "bad status or data byte"; return false; }
      if (r.kind == InKind::Abs14 && (i >= t.size() || !hex_byte(t[i++], r.data1_lsb))) { why = "abs14 needs MSB and LSB"; return false; }
      if (i >= t.size() || !lookup(kActions, t[i], action)) { why = "unknown action"; return false; }
      r.action = static_cast<Action>(action);
      if (!parse_args(t, i + 1, a, why)) return false;
    }
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
    if (a.ticks_per_rev > 0) r.ticks_per_rev = a.ticks_per_rev;
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
    r.fx_unit = a.fx_unit;
    if (r.action == Action::FxAssign) {
      if (r.fx_unit < 0) { why = "fx_assign needs unit=1.." + std::to_string(kFxUnits); return false; }
      if (!resolve_fx_target(a.target, r.fx_target)) { why = "fx_assign needs target=deck1..deck4|master"; return false; }
    }
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
    if (is_hid) {
      m.hid_index[r.hid.report].push_back(idx);
    } else {
      m.index.emplace(static_cast<std::uint16_t>(r.status << 8 | r.data1), idx);
      if (r.kind == InKind::Abs14) m.index.emplace(static_cast<std::uint16_t>(r.status << 8 | r.data1_lsb), idx);
    }
  } else if (head == "out") {
    // out <status> <data1> <state> [args]
    // out hid <field args> <state> [args]
    OutRule r;
    int state = 0;
    Args a;
    const bool is_hid = t.size() > 1 && t[1] == "hid";
    if (is_hid) {
      std::size_t k = 2;
      while (k < t.size() && t[k].find('=') != std::string::npos) ++k;
      if (k >= t.size() || !lookup(kStates, t[k], state)) {
        why = "expected: out hid report=<id> byte=<n> [bit=|bits=|size= endian=] <state>";
        return false;
      }
      if (!parse_args(t, 2, a, why, k) || !parse_args(t, k + 1, a, why)) return false;
      if (!make_field(a, r.hid_field, why, false)) return false;
      r.hid = true;
    } else {
      if (t.size() < 4 || !hex_byte(t[1], r.status) || !hex_byte(t[2], r.data1) || !lookup(kStates, t[3], state)) {
        why = "expected: out <status> <data1> <state>";
        return false;
      }
      if (!parse_args(t, 4, a, why)) return false;
    }
    r.state = static_cast<LedState>(state);
    r.deck = a.deck;
    r.slot = a.slot;
    r.mode = a.mode;
    r.blink = a.blink;
    r.amount = a.amount;
    r.value = a.value;
    r.has_value = a.has_value;
    r.master_unit = a.master_unit;
    r.fx_unit = a.fx_unit;
    r.offset = a.offset;
    long lim = 0;
    if (r.hid && !a.has_on) a.on = static_cast<int>((1LL << r.hid_field.width()) - 1);  // HID default: every bit set
    r.on = !a.max_s.empty() ? (hex_int(a.max_s, lim) ? static_cast<int>(lim) : -1) : a.on;
    r.off = !a.min_s.empty() ? (hex_int(a.min_s, lim) ? static_cast<int>(lim) : -1) : a.off;
    const int top = r.hid ? static_cast<int>((1LL << r.hid_field.width()) - 1) : 0xFF;
    if (r.on < 0 || r.off < 0 || r.on > top || r.off > top) { why = "on/off/min/max do not fit " + std::string(r.hid ? "the field" : "a byte"); return false; }
    if (r.state == LedState::PadMode && r.mode < 0) { why = "pad_mode state needs mode=<name>"; return false; }
    if (r.state == LedState::Modifier) {
      if (a.name.empty()) { why = "modifier state needs name=<name>"; return false; }
      r.modifier = intern(m, a.name);
    }
    if (!resolve_effect(r.state == LedState::ColorFx ? Action::ColorFx : Action::FxOn, a.effect, r.effect)) {
      why = "unknown effect '" + a.effect + "'";
      return false;
    }
    if (r.state == LedState::FxAssign) {
      if (r.fx_unit < 0) { why = "fx_assign state needs unit=1.." + std::to_string(kFxUnits); return false; }
      if (!resolve_fx_target(a.target, r.fx_target)) { why = "fx_assign state needs target=deck1..deck4|master"; return false; }
    }
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
  if (t[1] == "hid") {  // field tokens (key=value) identify the control; then kind (in) / state (out)
    std::size_t k = 2;
    control = t[0] + " hid";
    for (; k < t.size() && t[k].find('=') != std::string::npos; ++k) control += " " + t[k];
    first = t[0] == "in" ? k + 1 : k;
    if (first >= t.size()) { control.clear(); return; }
  } else if (t[0] == "in") {
    control = "in " + upper(t[2]) + " " + upper(t[3]);
    first = t[1] == "abs14" ? 5 : 4;
  } else {
    control = "out " + upper(t[1]) + " " + upper(t[2]);
    first = 3;
  }
  for (std::size_t i = first; i < t.size(); ++i) {
    const std::string key = t[i].substr(0, t[i].find('='));
    const bool target = i == first || key == "deck" || key == "slot" || key == "unit" || key == "mode" ||
                        key == "when" || key == "target" || key == "pad" || key == "offset" || key == "effect" || key == "name";
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
  if (m.name.empty() || (m.match.empty() && !m.hid)) {
    error = "mapping needs a name and a match line (or a hid line)";
    return false;
  }
  bool any_hid = false;
  for (const InRule& r : m.in) any_hid = any_hid || r.kind >= InKind::HidButton;
  for (const OutRule& r : m.out) any_hid = any_hid || r.hid;
  if ((any_hid || !m.hid_out.empty() || !m.hid_init.empty()) && !m.hid) {
    error = "HID rules need a 'hid vid=0x.... pid=0x....' line";
    return false;
  }
  // Every HID output report needs a template; one is made from the rules' extent when none was declared.
  const std::size_t declared = m.hid_out.size();
  for (const OutRule& r : m.out) {
    if (!r.hid) continue;
    const int end = r.hid_field.byte + r.hid_field.size;
    std::size_t at = m.hid_out.size();
    for (std::size_t i = 0; i < m.hid_out.size(); ++i)
      if (m.hid_out[i].report == r.hid_field.report) at = i;
    if (at == m.hid_out.size()) {
      HidOutTemplate n;
      n.report = r.hid_field.report;
      m.hid_out.push_back(std::move(n));
    }
    HidOutTemplate& tpl = m.hid_out[at];
    if (at < declared && end > tpl.size) {
      error = "out hid rule reaches byte " + std::to_string(end - 1) + ", outside hid_out report " +
              std::to_string(tpl.report) + " (size " + std::to_string(tpl.size) + ")";
      return false;
    }
    if (end > tpl.size) {
      tpl.size = end;
      tpl.init.resize(static_cast<std::size_t>(end), 0);
    }
  }
  return true;
}

bool save_user_mapping(const std::string& device, const std::vector<std::string>& lines, const std::string& folder,
                       std::string* error, const std::string& header) {
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
    if (header.empty()) existing.push_back("match \"" + device + "\"");
    else existing.push_back(header);
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
  // HID: the report observed, the frame before the first change, the frames after it, bytes to ignore
  bool hid = false;
  int report = 0;
  std::vector<std::uint8_t> base, noisy;
  std::vector<std::vector<std::uint8_t>> frames;
  std::string header;
};

struct ControllerHub::HidTrack {
  std::vector<std::uint8_t> last;
  std::vector<std::uint8_t> noise;  // grows when a byte changes, shrinks when it does not
  bool noisy(std::size_t i) const { return i < noise.size() && noise[i] >= 60; }
  void update(const std::uint8_t* data, std::size_t size) {
    const std::size_t n = std::min<std::size_t>(size, 512);
    if (last.size() != n) {
      last.assign(data, data + n);
      noise.assign(n, 0);
      return;
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (data[i] != last[i]) noise[i] = static_cast<std::uint8_t>(std::min(255, noise[i] + 10));
      else if (noise[i]) --noise[i];
      last[i] = data[i];
    }
  }
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

void ControllerHub::hid_set_matches() {
  std::vector<HidMatch> matches;
  for (const Mapping& m : mappings_)
    if (m.hid && !m.generated) matches.push_back(m.hid_match);
  if (learn_gamepads_)
    for (int usage : {0x04, 0x05, 0x08}) matches.push_back(HidMatch{-1, -1, 0x01, usage, -1});
  for (const auto& p : learn_allow_) matches.push_back(HidMatch{p.first, p.second, -1, -1, -1});
  hid_io_->set_matches(matches);
}

void ControllerHub::set_hid_io(HidIO* io) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hid_io_ = io;
    if (hid_io_) hid_set_matches();
  }
  devices_changed();
}

void ControllerHub::hid_learn_gamepads(bool on) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    learn_gamepads_ = on;
    if (hid_io_) hid_set_matches();
  }
  devices_changed();
}

void ControllerHub::hid_learn_allow(int vid, int pid) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    learn_allow_.emplace_back(vid, pid);
    if (hid_io_) hid_set_matches();
  }
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
  a.hid_prev.resize(n, 0);
  a.hid_seen.resize(n, 0);
  a.hid_sent.resize(a.map->hid_out.size());
  a.hid_time.resize(a.map->hid_out.size(), -1e9);
}

void ControllerHub::devices_changed() {
  if (!io_ && !hid_io_) return;
  const std::vector<std::string> names = io_ ? io_->devices() : std::vector<std::string>();
  const std::vector<HidDeviceInfo> infos = hid_io_ ? hid_io_->devices() : std::vector<HidDeviceInfo>();
  std::lock_guard<std::mutex> lock(mutex_);
  if (io_) midi_devices_changed(names);
  if (hid_io_) hid_devices_changed(infos);
}

void ControllerHub::hid_devices_changed(const std::vector<HidDeviceInfo>& infos) {
  hid_known_ = infos;
  active_.erase(std::remove_if(active_.begin(), active_.end(),
                               [&](const Active& a) {
                                 if (!a.hid) return false;
                                 const bool gone = std::none_of(infos.begin(), infos.end(), [&](const HidDeviceInfo& d) { return d.id == a.device; });
                                 if (gone) {
                                   DH_LOG(Info, "controller disconnected: %s", a.map->name.c_str());
                                   hid_io_->close(a.device);
                                   for (auto t = hid_tracks_.begin(); t != hid_tracks_.end();)
                                     t = t->first.first == a.device ? hid_tracks_.erase(t) : std::next(t);
                                 }
                                 return gone;
                               }),
                active_.end());
  for (const HidDeviceInfo& info : infos) {
    if (std::any_of(active_.begin(), active_.end(), [&](const Active& a) { return a.hid && a.device == info.id; })) continue;
    if (hid_usage_refused(info.usage_page, info.usage)) {  // never open keyboards, mice and pointers
      DH_LOG(Debug, "HID device %04X:%04X usage %04X/%04X refused", info.vid, info.pid, info.usage_page, info.usage);
      continue;
    }
    Mapping* found = nullptr;
    for (Mapping& m : mappings_)
      if (m.hid && !m.generated && hid_matches(m.hid_match, info)) { found = &m; break; }
    if (!found) {  // no mapping: only devices the app opted into (HID learn) get an empty one
      const bool game = learn_gamepads_ && info.usage_page == 0x01 && (info.usage == 0x04 || info.usage == 0x05 || info.usage == 0x08);
      const bool allowed = game || std::any_of(learn_allow_.begin(), learn_allow_.end(), [&](const std::pair<int, int>& p) {
                             return p.first == info.vid && (p.second < 0 || p.second == info.pid);
                           });
      if (!allowed) continue;
      char fallback[32];
      std::snprintf(fallback, sizeof fallback, "HID %04X:%04X", info.vid, info.pid);
      const std::string name = info.product.empty() ? std::string(fallback) : info.product;
      for (Mapping& m : mappings_)
        if (m.hid && m.generated && m.name == name && m.hid_match.vid == info.vid && m.hid_match.pid == info.pid) found = &m;
      if (!found) {
        Mapping g;
        g.name = name;
        g.match.clear();
        g.hid = true;
        g.generated = true;
        g.hid_match = HidMatch{info.vid, info.pid, -1, -1, -1};
        mappings_.push_back(std::move(g));
        found = &mappings_.back();
      }
      DH_LOG(Info, "HID device without a mapping: %s (learn available)", name.c_str());
    }
    if (!hid_io_->open(info.id)) {
      DH_LOG(Warn, "could not open HID device %04X:%04X", info.vid, info.pid);
      continue;
    }
    Active a;
    a.hid = true;
    a.device = info.id;
    a.map = found;
    fit(a);
    if (!found->generated) {
      for (const HidInit& h : found->hid_init) {
        if (h.feature) hid_io_->send_feature(info.id, h.report, h.data.data(), h.data.size());
        else hid_io_->send_output(info.id, h.report, h.data.data(), h.data.size());
      }
      DH_LOG(Info, "controller connected: %s (HID, mapping \"%s\")", info.product.c_str(), found->name.c_str());
    }
    active_.push_back(std::move(a));
  }
}

void ControllerHub::midi_devices_changed(const std::vector<std::string>& names) {
  // Drop controllers that went away.
  active_.erase(std::remove_if(active_.begin(), active_.end(),
                               [&](const Active& a) {
                                 if (a.hid) return false;
                                 const bool gone = std::find(names.begin(), names.end(), a.device) == names.end();
                                 if (gone) DH_LOG(Info, "controller disconnected: %s", a.device.c_str());
                                 return gone;
                               }),
                active_.end());
  for (const std::string& device : names) {
    if (std::any_of(active_.begin(), active_.end(), [&](const Active& a) { return !a.hid && a.device == device; })) continue;
    const std::string low = lower(device);
    Mapping* found = nullptr;
    for (Mapping& m : mappings_) {
      if (m.match.empty() || (m.hid && m.generated)) continue;  // HID-only mappings and generated HID ones are not MIDI
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
  const auto it = std::find_if(active_.begin(), active_.end(), [&](const Active& a) { return !a.hid && a.device == device; });
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
      case InKind::Button: dispatch_button(a, idx, r, d2 > 0); break;
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
      default: break;
    }
  }
}

void ControllerHub::dispatch_button(Active& a, std::size_t idx, const InRule& r, bool pressed) {
  const bool filtered = r.when >= 0 || (r.mode >= 0 && r.action != Action::PadMode);
  if (pressed) {
    if (!allowed(a, r)) return;
    a.down[idx] = 1;
  } else {
    const bool was = a.down[idx] != 0;
    a.down[idx] = 0;
    if (filtered && !was) return;  // its press was routed elsewhere (other layer or mode)
  }
  button(a, idx, r, pressed);
}

void ControllerHub::hid(const std::string& device, int report_id, const std::uint8_t* data, std::size_t size) {
  if (!data && size) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = std::find_if(active_.begin(), active_.end(), [&](const Active& a) { return a.hid && a.device == device; });
  if (it == active_.end()) return;
  std::shared_ptr<HidTrack>& track = hid_tracks_[{device, report_id}];
  if (!track) track = std::make_shared<HidTrack>();
  if (learn_->state == LearnState::Waiting || learn_->state == LearnState::Observing) learn_feed_hid(device, report_id, data, size, *track);
  else handle_hid(*it, report_id, data, size);
  track->update(data, size);
}

namespace {
float hid_normalise(const HidField& f, std::int64_t raw) {
  const int w = f.width();
  const double lo = f.has_min ? f.vmin : (f.is_signed ? -static_cast<double>(1LL << (w - 1)) : 0.0);
  const double hi = f.has_max ? f.vmax : (f.is_signed ? static_cast<double>((1LL << (w - 1)) - 1) : static_cast<double>((1LL << w) - 1));
  if (hi == lo) return 0;
  return static_cast<float>(std::max(0.0, std::min(1.0, (static_cast<double>(raw) - lo) / (hi - lo))));
}
}  // namespace

// Reports repeat every field, so a rule acts only when its own field changed since the last report
// (the first report only records a baseline: nothing moves just because a device connected).
void ControllerHub::handle_hid(Active& a, int report_id, const std::uint8_t* d, std::size_t n) {
  const auto rules = a.map->hid_index.find(report_id);
  if (rules == a.map->hid_index.end()) return;
  for (const std::size_t idx : rules->second) {
    const InRule& r = a.map->in[idx];
    std::int64_t raw = 0;
    if (!hid_read(r.hid, d, n, raw)) continue;  // short report: ignore
    const bool had = a.hid_seen[idx] != 0;
    const std::int64_t prev = a.hid_prev[idx];
    a.hid_seen[idx] = 1;
    a.hid_prev[idx] = raw;
    if (r.kind != InKind::HidDelta && (!had || raw == prev)) continue;
    switch (r.kind) {
      case InKind::HidButton: dispatch_button(a, idx, r, (raw != 0) != r.invert); break;
      case InKind::HidAbs: if (allowed(a, r)) value(a, idx, r, hid_normalise(r.hid, raw)); break;
      case InKind::HidRel: {
        const std::int64_t mod = 1LL << r.hid.width();
        std::int64_t diff = ((raw - prev) % mod + mod) % mod;  // running position: delta with wrap
        if (diff >= mod / 2) diff -= mod;
        if (r.invert) diff = -diff;
        if (allowed(a, r)) delta(a, r, static_cast<int>(diff));
        break;
      }
      case InKind::HidDelta:
        if (raw != 0 && allowed(a, r)) delta(a, r, static_cast<int>(std::max<std::int64_t>(-100000, std::min<std::int64_t>(100000, r.invert ? -raw : raw))));
        break;
      default: break;
    }
  }
}

FxSlotView& ControllerHub::fx_slot(int target, int slot) {
  return target == kFxMaster ? cache_.mixer.fx[slot] : cache_.deck[target].fx[slot];
}

int ControllerHub::fx_target(const InRule& r) const {
  if (r.fx_unit >= 0) return fx_assigned_[r.fx_unit];
  return r.master_unit ? kFxMaster : r.deck;
}

void ControllerHub::fx_assign(int unit, int target) {
  const int old = fx_assigned_[unit];
  if (old == target) return;
  Command c;
  for (int slot = 0; slot < kFxSlots; ++slot) {
    FxSlotView& from = fx_slot(old, slot);
    FxSlotView& to = fx_slot(target, slot);
    c.i = slot;
    if (from.on) {  // never leave the effect running on the channel it was taken away from
      c.deck = static_cast<std::uint8_t>(old);
      c.op = Op::FxEnable;
      c.f = 0.0f;
      sink_.command(c);
      from.on = false;
    }
    if (from.kind == EffectKind::None) continue;  // nothing selected on this slot: leave the target's own
    to.kind = from.kind;
    to.wet = from.wet;
    to.beats = from.beats;
    to.param = from.param;
    c.deck = static_cast<std::uint8_t>(target);
    c.op = Op::FxSelect; c.d = static_cast<double>(to.kind); sink_.command(c);
    c.d = 0;
    c.op = Op::FxWet; c.f = to.wet; sink_.command(c);
    c.op = Op::FxParam; c.f = to.param; sink_.command(c);
    c.op = Op::FxTime; c.f = to.beats; sink_.command(c);
  }
  fx_assigned_[unit] = target;
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
  const int fx_tgt = fx_target(r);
  const std::uint8_t fx_deck = static_cast<std::uint8_t>(fx_tgt);
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
    case Action::FxAssign:
      if (pressed) fx_assign(r.fx_unit, r.fx_target);
      break;
    case Action::MasterCue: {
      if (!r.hold && !pressed) break;
      const bool want = r.hold ? pressed : (r.has_value ? r.value > 0 : !master_cue_);
      if (want == master_cue_) break;
      if (want) master_cue_saved_ = cache_.mixer.head_mix;
      master_cue_ = want;
      cache_.mixer.head_mix = want ? 1.0f : master_cue_saved_;
      c.deck = 0;
      c.f = cache_.mixer.head_mix;
      send(Op::HeadMix);
      break;
    }
#ifdef DECKHAND_HAVE_MIC_OPS
    case Action::MicOn: case Action::MicTalkover: {
      bool& cur = r.action == Action::MicOn ? mic_on_ : mic_talk_;
      c.deck = 0;
      const Op op = r.action == Action::MicOn ? Op::MicOn : Op::MicTalkover;
      if (r.hold) {
        cur = pressed;
        c.i = pressed ? 1 : 0;
        send(op);
      } else if (pressed) {
        c.i = r.has_value ? (r.value > 0 ? 1 : 0) : -1;
        cur = c.i < 0 ? !cur : c.i == 1;
        send(op);
      }
      break;
    }
#endif
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
        FxSlotView& fx = fx_slot(fx_tgt, r.slot);
        const int kind = r.effect >= 0 ? r.effect : next_effect(static_cast<int>(fx.kind), r.amount < 0 ? -1 : 1);
        fx.kind = static_cast<EffectKind>(kind);
        c.deck = fx_deck;
        c.i = r.slot;
        c.d = kind;
        send(Op::FxSelect);
      }
      break;
    case Action::FxOn: {
      FxSlotView& fx = fx_slot(fx_tgt, r.slot);
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
        FxSlotView& fx = fx_slot(fx_tgt, r.slot);
        fx.beats = r.has_beats ? r.amount : step_beats(fx.beats, r.amount < 0 ? -1 : 1);
        c.deck = fx_deck;
        c.i = r.slot;
        c.f = fx.beats;
        send(Op::FxTime);
      }
      break;
    case Action::PadFx: {
      FxSlotView& fx = fx_slot(fx_tgt, r.slot);
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
      if (fx_target(r) == kFxMaster ? !hm : !cache_.have_deck[fx_target(r)]) return false;
      e = (fx_target(r) == kFxMaster ? m.fx[r.slot] : cache_.deck[fx_target(r)].fx[r.slot]).wet;
      break;
    case Action::FxParam:
      if (fx_target(r) == kFxMaster ? !hm : !cache_.have_deck[fx_target(r)]) return false;
      e = (fx_target(r) == kFxMaster ? m.fx[r.slot] : cache_.deck[fx_target(r)].fx[r.slot]).param;
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
    case Action::HeadMix: c.op = Op::HeadMix; master_cue_ = false; break;  // the knob takes over from master cue
    case Action::HeadLevel: c.op = Op::HeadLevel; break;
    case Action::MasterLevel: c.op = Op::MasterLevel; break;
    case Action::SamplerLevel: c.op = Op::SamplerLevel; break;
#ifdef DECKHAND_HAVE_MIC_OPS
    case Action::MicLevel: c.op = Op::MicLevel; c.deck = 0; break;
    case Action::MicEqLow: c.op = Op::MicEqLow; c.deck = 0; break;
    case Action::MicEqHigh: c.op = Op::MicEqHigh; c.deck = 0; break;
#endif
    case Action::AutoGain: c.op = Op::AutoGain; c.f = ranged(-24.0f, 12.0f); break;
    case Action::KeyShift: c.op = Op::KeyShift; c.f = std::round(ranged(-12.0f, 12.0f)); cache_.deck[r.deck].key_shift = c.f; break;
    case Action::MasterBpm: c.op = Op::MasterBpm; c.deck = 0; c.f = ranged(60.0f, 180.0f); cache_.mixer.master_bpm = c.f; break;
    case Action::FxWet: case Action::FxParam:
      c.op = r.action == Action::FxWet ? Op::FxWet : Op::FxParam;
      c.deck = static_cast<std::uint8_t>(fx_target(r));
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
  const int fx_tgt = fx_target(r);
  const std::uint8_t fx_deck = static_cast<std::uint8_t>(fx_tgt);
  switch (r.action) {
    case Action::Browse: sink_.browse(static_cast<int>(std::lround(steps * r.amount))); break;
    case Action::Jog:
      a.jog_ticks[r.deck] += steps;
      c.op = Op::JogTurn;
      c.i = r.top ? 1 : 0;
      c.d = static_cast<double>(steps) * r.amount / (r.ticks_per_rev > 0 ? r.ticks_per_rev : a.map->ticks_per_rev);
      sink_.command(c);
      break;
    case Action::FxSelect: {
      FxSlotView& fx = fx_slot(fx_tgt, r.slot);
      fx.kind = static_cast<EffectKind>(next_effect(static_cast<int>(fx.kind), sign));
      c.op = Op::FxSelect;
      c.deck = fx_deck;
      c.i = r.slot;
      c.d = static_cast<double>(fx.kind);
      sink_.command(c);
      break;
    }
    case Action::FxTime: {
      FxSlotView& fx = fx_slot(fx_tgt, r.slot);
      fx.beats = step_beats(fx.beats, sign);
      c.op = Op::FxTime;
      c.deck = fx_deck;
      c.i = r.slot;
      c.f = fx.beats;
      sink_.command(c);
      break;
    }
    case Action::FxWet: case Action::FxParam: {  // 1 % per tick, times scale=
      FxSlotView& fx = fx_slot(fx_tgt, r.slot);
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
  if (!io_ && !hid_io_) return;
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
  std::map<std::array<int, 6>, int> hwant;
  for (Active& a : active_) {
    want.clear();
    hwant.clear();
    for (const OutRule& r : a.map->out) {
      if (r.hid != a.hid) continue;
      const std::array<int, 6> hkey = {r.hid_field.report, r.hid_field.byte, r.hid_field.size, r.hid_field.lo,
                                       r.hid_field.hi, r.hid_field.big ? 1 : 0};
      const bool deckless = r.state == LedState::MicOn || r.state == LedState::MicTalkover || r.master_unit || r.fx_unit >= 0 || r.state == LedState::MasterCue || r.state == LedState::Modifier || r.state == LedState::SamplerPlaying;
      if (r.deck >= deck_count && !deckless) continue;
      const DeckView& d = r.deck < deck_count ? decks[r.deck] : blank;
      const std::uint16_t addr = static_cast<std::uint16_t>(r.status << 8 | r.data1);
      if (r.mode >= 0 && r.state != LedState::PadMode && a.pad_mode[r.deck] != r.mode) {
        if (r.hid) hwant.emplace(hkey, r.off);  // belongs to another pad mode: dark unless an active rule claims it
        else want.emplace(addr, r.off);
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
            const int tgt = r.fx_unit >= 0 ? fx_assigned_[r.fx_unit] : (r.master_unit ? kFxMaster : r.deck);
            const FxSlotView* fx = tgt == kFxMaster ? (mixer ? &mixer->fx[r.slot] : nullptr)
                                   : (tgt < deck_count ? &decks[tgt].fx[r.slot] : nullptr);
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
          case LedState::FxAssign: on = fx_assigned_[r.fx_unit] == r.fx_target; break;
          case LedState::MasterCue: on = master_cue_; break;
          case LedState::ColorFx: on = r.effect >= 0 ? d.color_fx == r.effect : d.color_fx != static_cast<int>(ColorFx::Filter); break;
          case LedState::MicOn: on = mic_on_; break;
          case LedState::MicTalkover: on = mic_talk_; break;
          case LedState::Modifier: on = r.modifier >= 0 && (a.held[r.modifier][0] || a.held[r.modifier][1 + r.deck]); break;
          default: break;
        }
        byte = on ? r.on : r.off;
      }
      if (r.hid) hwant[hkey] = byte;
      else want[addr] = byte;
    }
    if (a.hid) {
      hid_send_out(a, hwant, now);
      continue;
    }
    if (!io_) continue;
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

// Composes each output report from its template and the claimed fields; sends it when it changed and the
// report's interval has passed (a report held back by the rate limit goes out on a later tick).
void ControllerHub::hid_send_out(Active& a, const std::map<std::array<int, 6>, int>& want, double now) {
  if (!hid_io_) return;
  for (std::size_t ti = 0; ti < a.map->hid_out.size(); ++ti) {
    const HidOutTemplate& tpl = a.map->hid_out[ti];
    std::vector<std::uint8_t> buf = tpl.init;
    buf.resize(static_cast<std::size_t>(tpl.size), 0);
    for (const auto& [key, val] : want) {
      if (key[0] != tpl.report) continue;
      HidField f;
      f.report = key[0]; f.byte = key[1]; f.size = key[2]; f.lo = key[3]; f.hi = key[4]; f.big = key[5] != 0;
      hid_write(f, buf, static_cast<std::uint32_t>(val));
    }
    if (a.hid_sent[ti] == buf) continue;
    if (now - a.hid_time[ti] < tpl.interval) continue;
    a.hid_time[ti] = now;  // also paces retries after a failed send
    if (hid_io_->send_output(a.device, tpl.report, buf.data(), buf.size())) a.hid_sent[ti] = std::move(buf);
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

// ---- MIDI learn choices ---------------------------------------------------------------
std::vector<LearnChoice> learn_choices(int decks) {
  std::vector<LearnChoice> v;
  const auto add = [&](const std::string& group, const std::string& label, const std::string& action) {
    v.push_back({label, action, group});
  };
  const auto num = [](double x) {
    std::string t = std::to_string(x);
    t.erase(t.find_last_not_of('0') + 1);
    if (!t.empty() && t.back() == '.') t.pop_back();
    return t;
  };
  decks = std::max(1, std::min(kMaxDecks, decks));
  static const char* kPadModes[] = {"hotcue", "padfx1", "beatjump", "sampler", "keyboard", "padfx2", "beatloop", "keyshift"};
  static const char* kPadNames[] = {"Hot cue", "Pad FX 1", "Beat jump", "Sampler", "Keyboard", "Pad FX 2", "Beat loop", "Key shift"};
  static const double kLoops[] = {0.25, 0.5, 1, 2, 4, 8, 16, 32};
  static const double kJumps[] = {1, 2, 4, 8, 16};
  for (int d = 1; d <= decks; ++d) {
    const std::string g = "Deck " + std::to_string(d), dk = " deck=" + std::to_string(d);
    const struct { const char* label; const char* act; } simple[] = {
        {"Play", "play"}, {"Cue", "cue"}, {"Sync", "sync"}, {"Tempo range", "tempo_range"},
        {"Jump to start", "seek_start"}, {"Jog touch", "jog_touch"}, {"Tempo fader", "tempo"},
        {"Loop in", "loop_in"}, {"Loop out", "loop_out"}, {"Loop on/off", "loop_toggle"}, {"Reloop", "reloop"},
        {"Loop exit", "loop_exit"}, {"Loop halve", "loop_halve"}, {"Loop double", "loop_double"},
        {"Next cue point", "cue_call dir=1"}, {"Previous cue point", "cue_call dir=-1"},
        {"Delete hot cue 1", "hotcue_clear slot=1"},
        {"Trim", "trim"}, {"EQ high", "eq_high"}, {"EQ mid", "eq_mid"}, {"EQ low", "eq_low"},
        {"Colour FX knob", "filter"}, {"Channel fader", "fader"}, {"Headphone cue", "pfl"},
        {"Key lock", "key_lock"}, {"Quantize", "quantize"}, {"Slip", "slip"}, {"Reverse", "reverse"},
        {"Key shift", "key_shift"}, {"Auto gain", "auto_gain"}, {"Colour FX type", "color_fx"},
        {"Load selected track", "load"},
    };
    for (const auto& s : simple) add(g, s.label, std::string(s.act) + dk);
    add(g, "Jog wheel (top)", "jog surface=top" + dk);
    add(g, "Jog wheel (side)", "jog surface=side" + dk);
    for (int i = 1; i <= kHotCues; ++i) add(g, "Hot cue " + std::to_string(i), "hotcue slot=" + std::to_string(i) + dk);
    for (double b : kLoops) add(g, "Beat loop " + num(b), "beatloop beats=" + num(b) + dk);
    for (double b : kJumps) {
      add(g, "Beat jump back " + num(b), "beatjump beats=-" + num(b) + dk);
      add(g, "Beat jump forward " + num(b), "beatjump beats=" + num(b) + dk);
    }
    for (int m = 0; m < 8; ++m) add(g, std::string("Pad mode: ") + kPadNames[m], std::string("pad_mode mode=") + kPadModes[m] + dk);
  }
  const struct { const char* label; const char* act; } mixer[] = {
      {"Crossfader", "crossfader"}, {"Headphone mix", "head_mix"}, {"Headphone level", "head_level"},
      {"Master level", "master_level"}, {"Master cue", "master_cue"}, {"Browse (encoder)", "browse"},
      {"Browse press", "browse_press"}, {"Sampler level", "sampler_level"}, {"Master BPM", "master_bpm"},
      {"Shift (modifier)", "modifier name=shift"},
      {"Mic on/off", "mic_on"}, {"Mic level", "mic_level"}, {"Mic EQ low", "mic_eq_low"},
      {"Mic EQ high", "mic_eq_high"}, {"Mic talkover", "mic_talkover"},
  };
  for (const auto& s : mixer) add("Mixer", s.label, s.act);
  for (int u = 1; u <= 2; ++u) {
    const std::string g = "Effect unit " + std::to_string(u), us = " unit=" + std::to_string(u) + " slot=1";
    add(g, "Select effect (next)", "fx_select dir=1" + us);
    add(g, "Select effect (previous)", "fx_select dir=-1" + us);
    add(g, "Select effect (encoder)", "fx_select" + us);
    add(g, "On/off", "fx_on" + us);
    add(g, "Wet/dry", "fx_wet" + us);
    add(g, "Parameter", "fx_param" + us);
    add(g, "Time (shorter)", "fx_time dir=-1" + us);
    add(g, "Time (longer)", "fx_time dir=1" + us);
    for (int d = 1; d <= decks; ++d)
      add(g, "Assign to deck " + std::to_string(d), "fx_assign unit=" + std::to_string(u) + " target=deck" + std::to_string(d));
    add(g, "Assign to master", "fx_assign unit=" + std::to_string(u) + " target=master");
  }
  add("Master effect", "Select effect (next)", "fx_select dir=1 unit=master slot=1");
  add("Master effect", "On/off", "fx_on unit=master slot=1");
  add("Master effect", "Wet/dry", "fx_wet unit=master slot=1");
  for (int s = 1; s <= 16; ++s) add("Sampler", "Sampler pad " + std::to_string(s), "sampler slot=" + std::to_string(s));
  add("Sampler", "Stop all samples", "sampler_stop");
  return v;
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

std::string ControllerHub::learn_header() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return learn_->hid ? learn_->header : std::string();
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

// HID learn. Waiting: the first report with a changed (non-noisy) byte. A button action is captured at once
// (one changed bit, or one byte going to or from zero); other actions observe up to 12 changing reports
// (0.4 s after the last, or finalize) and then classify the byte that changed most: a toggling bit is a
// button, a counter that moves in one direction (wrapping at its width) is `rel`, a field that returns to
// zero is `delta`, anything else is `abs`. Two adjacent bytes become one 16-bit field when the high byte
// only ever steps by one as the low byte wraps.
bool ControllerHub::learn_feed_hid(const std::string& device, int report_id, const std::uint8_t* data, std::size_t size,
                                   HidTrack& tr) {
  Learn& l = *learn_;
  const std::size_t n = std::min<std::size_t>(size, 512);
  if (tr.last.size() != n) return true;  // no baseline for this report yet
  const Class cls = action_class(l.action);
  const auto header = [&] {
    char buf[96] = "";
    for (const HidDeviceInfo& d : hid_known_)
      if (d.id == device) {
        if (d.usage_page) std::snprintf(buf, sizeof buf, "hid vid=0x%04X pid=0x%04X usage_page=0x%X usage=0x%X", d.vid, d.pid, d.usage_page, d.usage);
        else std::snprintf(buf, sizeof buf, "hid vid=0x%04X pid=0x%04X", d.vid, d.pid);
      }
    l.header = buf;
  };
  if (l.state == LearnState::Waiting) {
    std::vector<std::size_t> changed;
    int bits = 0;
    for (std::size_t i = 0; i < n; ++i)
      if (data[i] != tr.last[i] && !tr.noisy(i)) {
        changed.push_back(i);
        for (unsigned x = data[i] ^ tr.last[i]; x; x &= x - 1) ++bits;
      }
    if (changed.empty()) return true;
    l.hid = true;
    l.device = device;
    l.report = report_id;
    l.base = tr.last;
    l.noisy.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) l.noisy[i] = tr.noisy(i) ? 1 : 0;
    header();
    if (cls == Class::Button) {
      char buf[96];
      const std::size_t b = changed[0];
      if (bits == 1) {
        int bit = 0;
        while (!(((data[b] ^ tr.last[b]) >> bit) & 1)) ++bit;
        std::snprintf(buf, sizeof buf, "in hid report=%d byte=%zu bit=%d button ", report_id, b, bit);
        l.line = buf + l.action_text + (((data[b] >> bit) & 1) ? "" : " invert=1");
      } else if (changed.size() == 1 && (data[b] == 0 || tr.last[b] == 0)) {
        std::snprintf(buf, sizeof buf, "in hid report=%d byte=%zu button ", report_id, b);
        l.line = buf + l.action_text + (data[b] ? "" : " invert=1");
      } else {
        return true;  // several controls at once: wait for a clear press
      }
      Mapping probe;
      std::string why;
      if (!parse_line(probe, l.line, why)) {
        l.state = LearnState::Failed;
        l.error = why;
        l.line.clear();
      } else {
        l.state = LearnState::Done;
      }
      return true;
    }
    l.state = LearnState::Observing;
    l.frames.assign(1, std::vector<std::uint8_t>(data, data + n));
    l.last = std::chrono::steady_clock::now();
    return true;
  }
  if (device != l.device || report_id != l.report || l.frames.empty() || l.frames.back().size() != n) return true;
  bool differs = false;
  for (std::size_t i = 0; i < n; ++i) differs = differs || (data[i] != l.frames.back()[i] && !l.noisy[i]);
  if (!differs) return true;
  l.frames.emplace_back(data, data + n);
  l.last = std::chrono::steady_clock::now();
  if (l.frames.size() >= 12) learn_finish();
  return true;
}

void ControllerHub::learn_finish_hid() {
  Learn& l = *learn_;
  const Class cls = action_class(l.action);
  const std::size_t n = l.base.size();
  std::vector<const std::vector<std::uint8_t>*> seq{&l.base};
  for (const auto& f : l.frames) seq.push_back(&f);
  const auto fail = [&](const std::string& why) {
    l.state = LearnState::Failed;
    l.error = why;
    l.line.clear();
  };
  std::vector<int> counts(n, 0);
  for (std::size_t j = 1; j < seq.size(); ++j)
    for (std::size_t i = 0; i < n; ++i)
      if ((*seq[j])[i] != (*seq[j - 1])[i] && !l.noisy[i]) ++counts[i];
  std::size_t best = 0;
  for (std::size_t i = 0; i < n; ++i)
    if (counts[i] > counts[best]) best = i;
  if (n == 0 || counts[best] == 0) return fail("no control movement was seen");
  // Widen to two bytes when the neighbour only steps by one as this byte wraps (carry).
  const auto carries = [&](std::size_t nb) {
    if (nb >= n || l.noisy[nb] || counts[nb] == 0 || counts[nb] > counts[best]) return false;
    for (std::size_t j = 1; j < seq.size(); ++j) {
      const int dn = (*seq[j])[nb] - (*seq[j - 1])[nb], dl = (*seq[j])[best] - (*seq[j - 1])[best];
      if (dn == 0) continue;
      if (!((dn == 1 && dl < 0) || (dn == -1 && dl > 0))) return false;
    }
    return true;
  };
  const bool le = carries(best + 1), be = best > 0 && carries(best - 1);
  HidField f;
  f.report = l.report;
  f.size = 1;
  f.big = false;
  f.byte = static_cast<int>(best);
  if (le != be) {
    f.size = 2;
    f.big = be;
    f.byte = static_cast<int>(be ? best - 1 : best);
  }
  f.hi = f.size * 8 - 1;
  std::vector<std::int64_t> x;
  for (const auto* fr : seq) {
    std::int64_t v = 0;
    hid_read(f, fr->data(), fr->size(), v);
    x.push_back(v);
  }
  const std::int64_t mod = 1LL << f.width();
  int nonzero = 0, pos = 0, neg = 0, wraps = 0;
  std::int64_t maxabs = 0, lo = x[0], hi = x[0];
  for (std::size_t j = 1; j < x.size(); ++j) {
    lo = std::min(lo, x[j]);
    hi = std::max(hi, x[j]);
    const std::int64_t d = x[j] - x[j - 1];
    if (d == 0) continue;
    std::int64_t circ = ((d % mod) + mod) % mod;
    if (circ >= mod / 2) circ -= mod;
    ++nonzero;
    (circ > 0 ? pos : neg)++;
    maxabs = std::max<std::int64_t>(maxabs, std::llabs(circ));
    if (std::llabs(d) > mod / 2 && std::llabs(circ) < mod / 4) ++wraps;
  }
  const bool counter = wraps > 0 || (nonzero >= 2 && std::max(pos, neg) >= 0.85 * nonzero && maxabs <= mod / 4);
  int returns = 0;  // a per-report delta keeps coming back to zero; a running counter does not
  for (std::size_t j = 1; j < x.size(); ++j) returns += x[j] == 0;
  const bool rests_at_zero = x.front() == 0 && x.back() == 0 && returns >= 2;
  const bool toggle = f.size == 1 && lo != hi && [&] {
    for (std::int64_t v : x) if (v != lo && v != hi) return false;
    const std::uint64_t diff = static_cast<std::uint64_t>(lo ^ hi);
    return (diff & (diff - 1)) == 0;
  }();
  char buf[128];
  std::string kind = "abs", extra;
  if (toggle) {
    if (cls == Class::Value || cls == Class::Delta) return fail(cls == Class::Value ? "this action needs a fader or knob, not a button" : "this action needs an encoder, not a button");
    int bit = 0;
    while (!(((lo ^ hi) >> bit) & 1)) ++bit;
    kind = "button";
    extra = " bit=" + std::to_string(bit) + (((x.back() >> bit) & 1) ? "" : " invert=1");
  } else if (cls == Class::Value) {
    kind = "abs";
  } else if (rests_at_zero) {
    kind = "delta";
  } else if (counter) {
    kind = "rel";
  } else if (cls == Class::Delta) {
    kind = "rel";
  } else {
    kind = (hi - lo >= mod / 4) ? "abs" : "rel";
  }
  std::snprintf(buf, sizeof buf, "in hid report=%d byte=%d", f.report, f.byte);
  l.line = buf;
  if (f.size > 1 && kind != "button") l.line += std::string(" size=2 endian=") + (f.big ? "be" : "le");
  if (kind == "delta") l.line += " signed=1";
  l.line += extra + " " + kind + " " + l.action_text;
  Mapping probe;
  std::string why;
  if (!parse_line(probe, l.line, why)) return fail(why);
  l.state = LearnState::Done;
}

void ControllerHub::learn_finish() {
  if (learn_->hid) return learn_finish_hid();
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
  const InRule& added = it->map->in.back();
  for (Active& a : active_) {
    if (a.map != it->map) continue;
    fit(a);
    if (a.hid && added.kind >= InKind::HidButton) {  // the new rule starts from the control's present value
      const auto track = hid_tracks_.find({a.device, added.hid.report});
      std::int64_t raw = 0;
      if (track != hid_tracks_.end() && hid_read(added.hid, track->second->last.data(), track->second->last.size(), raw)) {
        a.hid_prev[it->map->in.size() - 1] = raw;
        a.hid_seen[it->map->in.size() - 1] = 1;
      }
    }
  }
  *learn_ = Learn{};
  return true;
}

}  // namespace deckhand
