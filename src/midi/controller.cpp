#include "deckhand/midi/controller.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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
};
const Named kStates[] = {
    {"playing", int(LedState::Playing)}, {"cue", int(LedState::Cue)}, {"sync", int(LedState::Sync)},
    {"pfl", int(LedState::Pfl)}, {"loop", int(LedState::Loop)}, {"loop_in", int(LedState::LoopIn)},
    {"hotcue", int(LedState::HotCue)}, {"loaded", int(LedState::Loaded)}, {"pad_mode", int(LedState::PadMode)},
    {"level", int(LedState::Level)},
};
const Named kModes[] = {{"hotcue", 0}, {"padfx1", 1}, {"beatjump", 2}, {"sampler", 3},
                        {"keyboard", 4}, {"padfx2", 5}, {"beatloop", 6}, {"keyshift", 7}};
const Named kInKinds[] = {{"button", int(InKind::Button)}, {"abs", int(InKind::Abs)}, {"abs14", int(InKind::Abs14)},
                          {"rel", int(InKind::Rel)}, {"rel2", int(InKind::Rel2)}};

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
  int deck = 0, slot = 0, mode = 0;
  bool top = false, invert = false, blink = false;
  float amount = 1;
  int on = 0x7F, off = 0, min = -1, max = -1;
  std::string match;
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
    std::uint8_t b = 0;
    if (k == "deck") a.deck = std::atoi(v.c_str()) - 1;
    else if (k == "slot") a.slot = std::atoi(v.c_str()) - 1;
    else if (k == "mode") { if (!lookup(kModes, v, a.mode)) { error = "unknown pad mode '" + v + "'"; return false; } }
    else if (k == "surface") a.top = v == "top";
    else if (k == "invert") a.invert = v == "1";
    else if (k == "blink") a.blink = v == "1";
    else if (k == "beats" || k == "dir" || k == "scale") a.amount = static_cast<float>(std::atof(v.c_str()));
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
  if (a.slot < 0 || a.slot >= kHotCues) { error = "slot out of range"; return false; }
  return true;
}

const float kTempoRanges[] = {0.06f, 0.10f, 0.16f, 1.0f};

}  // namespace

bool parse_mapping(const std::string& text, Mapping& m, std::string& error) {
  m = Mapping{};
  std::istringstream in(text);
  std::string line;
  int number = 0;
  const auto fail = [&](const std::string& why) {
    error = "line " + std::to_string(number) + ": " + why;
    return false;
  };
  while (std::getline(in, line)) {
    ++number;
    const std::vector<std::string> t = tokens(line);
    if (t.empty()) continue;
    const std::string& head = t[0];
    std::string why;
    if (head == "name" && t.size() == 2) {
      m.name = t[1];
    } else if (head == "match" && t.size() == 2) {
      m.match = lower(t[1]);
    } else if (head == "audio") {
      Args a;
      if (!parse_args(t, 1, a, why)) return fail(why);
      m.audio_match = lower(a.match);
      if (a.master >= 0) m.master_channel = a.master;
      m.phones_channel = a.phones;
    } else if (head == "jog") {
      Args a;
      if (!parse_args(t, 1, a, why)) return fail(why);
      if (a.ticks_per_rev <= 0) return fail("jog needs ticks_per_rev=N");
      m.ticks_per_rev = a.ticks_per_rev;
    } else if (head == "init") {
      std::vector<std::uint8_t> msg;
      for (std::size_t i = 1; i < t.size(); ++i) {
        std::uint8_t b = 0;
        if (!hex_byte(t[i], b)) return fail("bad hex byte '" + t[i] + "'");
        msg.push_back(b);
      }
      if (msg.empty()) return fail("init needs at least one byte");
      m.init.push_back(std::move(msg));
    } else if (head == "in") {
      // in <kind> <status> <data1> [<data1 lsb>] <action> [args]
      InRule r;
      int kind = 0, action = 0;
      if (t.size() < 5 || !lookup(kInKinds, t[1], kind)) return fail("expected: in <kind> <status> <data1> <action>");
      r.kind = static_cast<InKind>(kind);
      std::size_t i = 2;
      if (!hex_byte(t[i++], r.status) || !hex_byte(t[i++], r.data1)) return fail("bad status or data byte");
      if (r.kind == InKind::Abs14 && (i >= t.size() || !hex_byte(t[i++], r.data1_lsb))) return fail("abs14 needs MSB and LSB");
      if (i >= t.size() || !lookup(kActions, t[i], action)) return fail("unknown action");
      r.action = static_cast<Action>(action);
      Args a;
      if (!parse_args(t, i + 1, a, why)) return fail(why);
      r.deck = a.deck;
      r.slot = a.slot;
      r.mode = a.mode;
      r.top = a.top;
      r.invert = a.invert;
      r.amount = a.amount;
      const std::size_t idx = m.in.size();
      m.in.push_back(r);
      m.index.emplace(static_cast<std::uint16_t>(r.status << 8 | r.data1), idx);
      if (r.kind == InKind::Abs14) m.index.emplace(static_cast<std::uint16_t>(r.status << 8 | r.data1_lsb), idx);
    } else if (head == "out") {
      // out <status> <data1> <state> [args]
      OutRule r;
      int state = 0;
      if (t.size() < 4 || !hex_byte(t[1], r.status) || !hex_byte(t[2], r.data1) || !lookup(kStates, t[3], state))
        return fail("expected: out <status> <data1> <state>");
      r.state = static_cast<LedState>(state);
      Args a;
      if (!parse_args(t, 4, a, why)) return fail(why);
      r.deck = a.deck;
      r.slot = a.slot;
      r.mode = a.mode;
      r.blink = a.blink;
      r.on = static_cast<std::uint8_t>(a.max >= 0 ? a.max : a.on);
      r.off = static_cast<std::uint8_t>(a.min >= 0 ? a.min : a.off);
      m.out.push_back(r);
    } else {
      return fail("unknown directive '" + head + "'");
    }
  }
  if (m.name.empty() || m.match.empty()) {
    error = "mapping needs both a name and a match line";
    return false;
  }
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

ControllerHub::ControllerHub(ControlSink& sink, std::vector<Mapping> mappings)
    : sink_(sink), mappings_(std::move(mappings)) {}

void ControllerHub::set_io(MidiIO* io) {
  io_ = io;
  devices_changed();
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
    for (const Mapping& m : mappings_) {
      if (low.find(m.match) == std::string::npos) continue;
      Active a;
      a.device = device;
      a.map = &m;
      a.msb.assign(m.in.size(), 0);
      a.lsb.assign(m.in.size(), 0);
      a.last_out.assign(m.out.size(), -1);
      for (const auto& msg : m.init) io_->send(device, msg.data(), msg.size());
      DH_LOG(Info, "controller connected: %s (mapping \"%s\")", device.c_str(), m.name.c_str());
      active_.push_back(std::move(a));
      break;
    }
    if (!std::any_of(active_.begin(), active_.end(), [&](const Active& a) { return a.device == device; })) {
      DH_LOG(Info, "MIDI device without a mapping: %s", device.c_str());
    }
  }
}

std::vector<std::string> ControllerHub::connected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  for (const Active& a : active_) out.push_back(a.map->name);
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
      if (it != active_.end()) handle(*it, status, data[i + 1], data[i + 2]);
    }
    i += len;
  }
}

void ControllerHub::handle(Active& a, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
  if ((status & 0xF0) == 0x80) {  // note-off is a release of the same note
    status = static_cast<std::uint8_t>(0x90 | (status & 0x0F));
    d2 = 0;
  }
  const auto range = a.map->index.equal_range(static_cast<std::uint16_t>(status << 8 | d1));
  for (auto it = range.first; it != range.second; ++it) {
    const InRule& r = a.map->in[it->second];
    switch (r.kind) {
      case InKind::Button: button(a, r, d2 > 0); break;
      case InKind::Abs: value(r, static_cast<float>(d2) / 127.0f); break;
      case InKind::Abs14: {
        (d1 == r.data1 ? a.msb : a.lsb)[it->second] = d2;
        value(r, static_cast<float>((a.msb[it->second] << 7) | a.lsb[it->second]) / 16383.0f);
        break;
      }
      case InKind::Rel: delta(a, r, static_cast<int>(d2) - 0x40); break;
      case InKind::Rel2: delta(a, r, d2 < 0x40 ? d2 : static_cast<int>(d2) - 0x80); break;
    }
  }
}

void ControllerHub::button(Active& a, const InRule& r, bool pressed) {
  Command c;
  c.deck = static_cast<std::uint8_t>(r.deck);
  const auto press = [&](Op op) {
    if (!pressed) return;
    c.op = op;
    sink_.command(c);
  };
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
    case Action::BeatLoop: c.f = r.amount; press(Op::BeatLoop); break;
    case Action::BeatJump: c.f = r.amount; press(Op::BeatJump); break;
    case Action::HotCue:
      c.op = pressed ? Op::HotCueDown : Op::HotCueUp;
      c.i = r.slot;
      sink_.command(c);
      break;
    case Action::HotCueClear: c.i = r.slot; press(Op::HotCueClear); break;
    case Action::Pfl: c.i = -1; press(Op::Pfl); break;
    case Action::BrowsePress: if (pressed) sink_.browse_press(); break;
    case Action::Load: if (pressed) sink_.load_selected(r.deck); break;
    case Action::PadMode: if (pressed) a.pad_mode[r.deck] = r.mode; break;
    default: break;
  }
}

void ControllerHub::value(const InRule& r, float v) {
  if (r.invert) v = 1.0f - v;
  Command c;
  c.deck = static_cast<std::uint8_t>(r.deck);
  c.f = v;
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
    default: return;
  }
  sink_.command(c);
}

void ControllerHub::delta(Active& a, const InRule& r, int steps) {
  if (steps == 0) return;
  if (r.action == Action::Browse) {
    sink_.browse(static_cast<int>(std::lround(steps * r.amount)));
  } else if (r.action == Action::Jog) {
    a.jog_ticks[r.deck] += steps;
    Command c;
    c.op = Op::JogTurn;
    c.deck = static_cast<std::uint8_t>(r.deck);
    c.i = r.top ? 1 : 0;
    c.d = static_cast<double>(steps) * r.amount / a.map->ticks_per_rev;
    sink_.command(c);
  }
}

void ControllerHub::tick(const DeckView* decks, int deck_count, double now) {
  if (!io_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const bool blink_on = std::fmod(now, 0.5) < 0.25;
  for (Active& a : active_) {
    for (std::size_t i = 0; i < a.map->out.size(); ++i) {
      const OutRule& r = a.map->out[i];
      if (r.deck >= deck_count) continue;
      const DeckView& d = decks[r.deck];
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
          default: break;
        }
        byte = on ? r.on : r.off;
      }
      if (byte == a.last_out[i]) continue;
      a.last_out[i] = byte;
      const std::uint8_t msg[3] = {r.status, r.data1, static_cast<std::uint8_t>(byte)};
      io_->send(a.device, msg, 3);
    }
  }
}

}  // namespace deckhand
