#include "deckhand/app/keymap.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace deckhand {
namespace {

#if defined(__APPLE__)
constexpr bool kMac = true;
constexpr std::uint32_t kPrimaryMod = kModCommand;
#else
constexpr bool kMac = false;
constexpr std::uint32_t kPrimaryMod = kModControl;
#endif

struct HidKey {
  std::uint32_t hid;
  const char* name;
};

// USB HID keyboard usages (page 0x07): the physical keys, whatever the layout says is printed on them.
const std::vector<HidKey>& hid_table() {
  static const std::vector<HidKey> table = [] {
    std::vector<HidKey> t;
    static std::vector<std::string> names;  // storage for generated names
    names.reserve(128);
    const auto add = [&](std::uint32_t hid, std::string n) {
      names.push_back(std::move(n));
      t.push_back({hid, names.back().c_str()});
    };
    for (int i = 0; i < 26; ++i) add(4 + i, std::string(1, static_cast<char>('a' + i)));
    for (int i = 1; i <= 9; ++i) add(29 + i, std::to_string(i));
    add(39, "0");
    const std::pair<std::uint32_t, const char*> fixed[] = {
        {40, "enter"},     {41, "escape"},   {42, "backspace"}, {43, "tab"},      {44, "space"},     {45, "minus"},
        {46, "equals"},    {47, "lbracket"}, {48, "rbracket"},  {49, "backslash"}, {51, "semicolon"}, {52, "quote"},
        {53, "grave"},     {54, "comma"},    {55, "period"},    {56, "slash"},   {57, "capslock"},  {70, "printscreen"},
        {71, "scrolllock"}, {72, "pause"},   {73, "insert"},    {74, "home"},    {75, "pageup"},    {76, "delete"},
        {77, "end"},       {78, "pagedown"}, {79, "right"},     {80, "left"},    {81, "down"},      {82, "up"},
        {83, "numlock"},   {84, "kpdivide"}, {85, "kpmultiply"}, {86, "kpminus"}, {87, "kpplus"},   {88, "kpenter"},
        {98, "kp0"},       {99, "kpperiod"}};
    for (const auto& f : fixed) add(f.first, f.second);
    for (int i = 1; i <= 12; ++i) add(57 + i, "f" + std::to_string(i));
    for (int i = 1; i <= 9; ++i) add(88 + i, "kp" + std::to_string(i));
    for (int i = 13; i <= 24; ++i) add(104 + (i - 13), "f" + std::to_string(i));
    return t;
  }();
  return table;
}

// Single printable characters that name a key in files ("," for comma) and the character a key types.
const std::pair<char, const char*> kPunct[] = {{'-', "minus"},  {'=', "equals"},    {'[', "lbracket"}, {']', "rbracket"}, {'\\', "backslash"},
                                               {';', "semicolon"}, {'\'', "quote"}, {'`', "grave"},    {',', "comma"},    {'.', "period"},
                                               {'/', "slash"}};

std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

std::string canonical_key(std::string_view text) {
  std::string t = lower(text);
  if (t.size() == 1) {
    for (const auto& p : kPunct)
      if (p.first == t[0]) return p.second;
  }
  static const std::pair<const char*, const char*> aliases[] = {
      {"return", "enter"}, {"esc", "escape"}, {"del", "delete"}, {"pgup", "pageup"}, {"pgdn", "pagedown"}, {"bksp", "backspace"},
      {"ins", "insert"},   {"spacebar", "space"}, {"leftarrow", "left"}, {"rightarrow", "right"}, {"uparrow", "up"},
      {"downarrow", "down"}, {"backquote", "grave"}, {"backtick", "grave"}, {"leftbracket", "lbracket"},
      {"rightbracket", "rbracket"}, {"apostrophe", "quote"}};
  for (const auto& a : aliases)
    if (t == a.first) return a.second;
  return key_hid_from_name(t) ? t : std::string();
}

bool mod_from_word(const std::string& w, std::uint32_t& bit) {
  if (w == "shift") bit = kModShift;
  else if (w == "cmd" || w == "command" || w == "meta" || w == "super" || w == "win") bit = kModCommand;
  else if (w == "alt" || w == "option" || w == "opt") bit = kModAlt;
  else if (w == "ctrl" || w == "control") bit = kModControl;
  else if (w == "mod") bit = kPrimaryMod;
  else return false;
  return true;
}

// Chords the operating system keeps for itself (or that quit and hide the app): bindable, but warned about.
bool reserved_chord(const KeyChord& c) {
  const std::uint32_t m = c.mods & kModMask;
  if (m == kModCommand || m == kModControl) {  // Cmd on macOS; Ctrl-based ones are the application's own elsewhere
    if (m == kModCommand && (c.key == "q" || c.key == "w" || c.key == "h" || c.key == "m" || c.key == "tab" || c.key == "space" || c.key == "grave"))
      return true;
  }
  if (m == kModAlt && (c.key == "f4" || c.key == "tab" || c.key == "space")) return true;
  if (m == (kModCommand | kModAlt) && (c.key == "escape" || c.key == "d")) return true;
  if (m == (kModCommand | kModShift) && (c.key == "3" || c.key == "4" || c.key == "5")) return true;
  if (m == kModControl && kMac && (c.key == "up" || c.key == "down" || c.key == "left" || c.key == "right" || c.key == "space")) return true;
  return false;
}

std::string fmt_num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

// Splits a line into words; "double quoted" text (with \" and \\) is one word, key="a b" stays one word.
std::vector<std::string> split_words(const std::string& line, std::string& error) {
  std::vector<std::string> out;
  std::string cur;
  bool in_word = false, quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '\\' && i + 1 < line.size()) cur += line[++i];
      else if (c == '"') quoted = false;
      else cur += c;
    } else if (c == '"') {
      quoted = true;
      in_word = true;
    } else if (c == '#' && !in_word) {
      break;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (in_word) out.push_back(cur);
      cur.clear();
      in_word = false;
    } else {
      cur += c;
      in_word = true;
    }
  }
  if (quoted) error = "unterminated quote";
  if (in_word) out.push_back(cur);
  return out;
}

bool parse_context(const std::string& v, KeyContext& c) {
  const std::string l = lower(v);
  if (l == "global") c = KeyContext::Global;
  else if (l == "browser") c = KeyContext::Browser;
  else if (l == "text") c = KeyContext::Text;
  else return false;
  return true;
}

bool parse_mode(const std::string& v, KeyMode& m) {
  const std::string l = lower(v);
  if (l == "press") m = KeyMode::Press;
  else if (l == "hold") m = KeyMode::Hold;
  else if (l == "toggle") m = KeyMode::Toggle;
  else if (l == "repeat") m = KeyMode::Repeat;
  else return false;
  return true;
}

bool parse_number(const std::string& s, double& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return end && *end == 0;
}

std::string quote(const std::string& s) {
  std::string r = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') r += '\\';
    r += c;
  }
  return r + "\"";
}

std::string html_escape(const std::string& s) {
  std::string r;
  for (char c : s) {
    switch (c) {
      case '&': r += "&amp;"; break;
      case '<': r += "&lt;"; break;
      case '>': r += "&gt;"; break;
      case '"': r += "&quot;"; break;
      default: r += c;
    }
  }
  return r;
}

struct ControlIndex {
  std::vector<KeyControlInfo> all;
  std::unordered_map<std::string, std::size_t> by_name;
};

const ControlIndex& control_index() {
  static const ControlIndex idx = [] {
    ControlIndex i;
    for (const ControlInfo& c : list_controls()) i.all.push_back({c.name, c.kind, c.range, c.help});
    for (const UiActionInfo& u : list_ui_actions()) i.all.push_back({u.name, u.kind, u.range, u.help});
    for (std::size_t k = 0; k < i.all.size(); ++k) i.by_name.emplace(i.all[k].name, k);
    return i;
  }();
  return idx;
}

}  // namespace

// ---- keys and chords ------------------------------------------------------------------------------------------

std::string key_name_from_hid(std::uint32_t hid) {
  for (const HidKey& k : hid_table())
    if (k.hid == hid) return k.name;
  return {};
}

std::uint32_t key_hid_from_name(std::string_view name) {
  for (const HidKey& k : hid_table())
    if (name == k.name) return k.hid;
  return 0;
}

std::string key_name_from_char(std::uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
  if (cp >= 'a' && cp <= 'z') return std::string(1, static_cast<char>(cp));
  if (cp >= '0' && cp <= '9') return std::string(1, static_cast<char>(cp));
  if (cp == ' ') return "space";
  if (cp == '\r' || cp == '\n') return "enter";
  if (cp == '\t') return "tab";
  if (cp == 0x1b) return "escape";
  if (cp == 0x7f || cp == 8) return "backspace";
  for (const auto& p : kPunct)
    if (static_cast<std::uint32_t>(p.first) == cp) return p.second;
  return {};
}

bool parse_chord(std::string_view text, KeyChord& out, std::string& error) {
  out = {};
  std::vector<std::string> parts;
  std::string cur;
  for (char c : text) {
    if (c == '+' && !cur.empty()) {  // a leading or doubled '+' is a key of its own: not supported, use names
      parts.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  parts.push_back(cur);
  if (parts.empty() || parts.back().empty()) {
    error = "empty key in chord \"" + std::string(text) + "\"";
    return false;
  }
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    std::uint32_t bit = 0;
    if (!mod_from_word(lower(parts[i]), bit)) {
      error = "unknown modifier \"" + parts[i] + "\" (use shift, alt, ctrl, cmd or mod)";
      return false;
    }
    out.mods |= bit;
  }
  out.key = canonical_key(parts.back());
  if (out.key.empty()) {
    error = "unknown key \"" + parts.back() + "\"";
    return false;
  }
  return true;
}

std::string chord_text(const KeyChord& c) {
  std::string s;
  const auto add = [&](const char* w) { s += w; s += '+'; };
  std::uint32_t m = c.mods & kModMask;
  if (m & kPrimaryMod) {
    add("mod");
    m &= ~kPrimaryMod;
  }
  if (m & kModControl) add("ctrl");
  if (m & kModAlt) add("alt");
  if (m & kModShift) add("shift");
  if (m & kModCommand) add("cmd");
  return s + c.key;
}

std::string chord_label(const KeyChord& c, LabelStyle style) {
  std::string s;
  const bool native = style == LabelStyle::Native;
  const auto add = [&](const std::string& w) { s += w; s += '+'; };
  std::uint32_t m = c.mods & kModMask;
  if (!native && (m & kPrimaryMod)) {
    add("Mod");
    m &= ~kPrimaryMod;
  }
  if (m & kModControl) add("Ctrl");
  if (m & kModAlt) add(kMac && native ? "Option" : "Alt");
  if (m & kModShift) add("Shift");
  if (m & kModCommand) add(kMac || !native ? "Cmd" : "Win");
  std::string k = c.key;
  for (const auto& p : kPunct)
    if (k == p.second) {
      k = std::string(1, p.first);
      break;
    }
  if (k.size() == 1) k[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(k[0])));
  else if (k.compare(0, 2, "kp") == 0 && k.size() > 2) k = "Keypad " + k.substr(2);
  else if (!k.empty()) {
    k[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(k[0])));
    if (k == "Pageup") k = "Page Up";
    if (k == "Pagedown") k = "Page Down";
  }
  return s + k;
}

std::string key_control_group(const std::string& control) {
  const auto digit_after = [&](std::size_t at) { return control.size() > at && std::isdigit(static_cast<unsigned char>(control[at])); };
  if (control.compare(0, 4, "deck") == 0 && digit_after(4)) return "Deck " + std::string(1, control[4]);
  const std::string head = control.substr(0, control.find('.'));
  if (head == "mixer") return "Mixer";
  if (head == "sampler") return "Sampler";
  if (head == "preview") return "Pre-listen";
  if (head == "ui") {
    const std::string rest = control.substr(3);
    for (const char* deck_action : {"nudge.", "tempo_step.", "key_shift."})
      if (rest.compare(0, std::strlen(deck_action), deck_action) == 0) return "Deck " + std::string(1, control.back());
    for (const char* p : {"load", "browse", "playlist", "focus", "search", "add", "rate", "clear_search", "edit_track", "compatible", "undo"})
      if (rest.compare(0, std::strlen(p), p) == 0) return "Browser";
    if (rest.compare(0, 7, "preview") == 0) return "Pre-listen";
    return "Application";
  }
  return "Other";
}

const char* key_context_name(KeyContext c) {
  switch (c) {
    case KeyContext::Global: return "global";
    case KeyContext::Browser: return "browser";
    case KeyContext::Text: return "text";
  }
  return "global";
}

const char* key_mode_name(KeyMode m) {
  switch (m) {
    case KeyMode::Press: return "press";
    case KeyMode::Hold: return "hold";
    case KeyMode::Toggle: return "toggle";
    case KeyMode::Repeat: return "repeat";
  }
  return "press";
}

// ---- application actions -----------------------------------------------------------------------------------------

std::vector<UiActionInfo> list_ui_actions() {
  std::vector<UiActionInfo> o;
  const auto add = [&](std::string n, const char* kind, const char* range, std::string help) {
    o.push_back({std::move(n), kind, range, std::move(help)});
  };
  for (int d = 1; d <= kMaxDecks; ++d) add("ui.load." + std::to_string(d), "trigger", "none", "Load the selected track into deck " + std::to_string(d));
  add("ui.load_free", "trigger", "none", "Load the selected track into deck 1, or into deck 2 while deck 1 plays");
  add("ui.browse_up", "trigger", "none", "Select the previous track (or playlist while the tree has focus)");
  add("ui.browse_down", "trigger", "none", "Select the next track (or playlist while the tree has focus)");
  add("ui.browse_page_up", "trigger", "none", "Move the selection up one page");
  add("ui.browse_page_down", "trigger", "none", "Move the selection down one page");
  add("ui.browse_top", "trigger", "none", "Select the first track");
  add("ui.browse_bottom", "trigger", "none", "Select the last track");
  add("ui.browse_switch", "trigger", "none", "Move the keyboard between the playlist tree and the track list");
  add("ui.playlist_next", "trigger", "none", "Show the next playlist");
  add("ui.playlist_prev", "trigger", "none", "Show the previous playlist");
  add("ui.focus_search", "trigger", "none", "Start typing in the search box");
  add("ui.clear_search", "trigger", "none", "Clear the search box");
  add("ui.preview_toggle", "trigger", "none", "Pre-listen to the selected track (or pause it), in the headphones");
  add("ui.preview_stop", "trigger", "none", "Stop pre-listening");
  add("ui.preview_seek", "value", "seconds, signed, relative to the pre-listen position", "Skip around in the pre-listened track");
  add("ui.queue_add", "trigger", "none", "Add the selected tracks to the end of the play queue");
  add("ui.queue_play_next", "trigger", "none", "Play the selected tracks next (behind earlier play-next entries)");
  add("ui.suggestions", "trigger", "none", "Open or close the suggestions: what goes well after the track on air");
  add("ui.queue_suggestion", "trigger", "none", "Play the top suggestion next (adds it to the play queue)");
  add("ui.prep", "trigger", "none", "Open or close preparation mode: audition two tracks against each other in the headphones");
  add("ui.add_to_prepare", "trigger", "none", "Add the selected tracks to the playlist named Prepare (if there is one)");
  add("ui.add_to", "trigger", "none", "Open the add-to-playlist menu for the selected tracks");
  add("ui.rate", "value", "0..5 stars", "Rate the selected track");
  add("ui.rate_up", "trigger", "none", "Give the selected track one more star");
  add("ui.rate_down", "trigger", "none", "Give the selected track one star less");
  add("ui.undo_edit", "trigger", "none", "Undo the last tag edit (title, artist, rating ...)");
  add("ui.edit_track", "trigger", "none", "Edit the selected track's tags");
  add("ui.compatible", "trigger", "none", "Show only tracks that mix with the loaded one");
  add("ui.settings", "trigger", "none", "Open or close the settings");
  add("ui.shortcuts", "trigger", "none", "Open or close the keyboard shortcuts");
  add("ui.sampler", "trigger", "none", "Open or close the sampler");
  add("ui.learn", "trigger", "none", "Open or close MIDI learn");
  add("ui.controller", "trigger", "none", "Open or close the panel of the connected controller (mapping and per-controller settings)");
  add("ui.controller.switch_decks", "toggle", "0 / 1 = the deck pair, none = switch", "Switch a controller with switchable deck sides to its other pair of decks (1/2 <-> 3/4)");
  add("ui.columns", "trigger", "none", "Open or close the browser columns");
  add("ui.trash", "trigger", "none", "Open or close the trash and edit history");
  add("ui.chain", "trigger", "none", "Open or close the effect chain editor (master chain; again on the open panel closes it)");
  for (const char* t : {"deck1", "deck2", "deck3", "deck4", "master", "mic", "sampler"})
    add(std::string("ui.chain.") + t, "trigger", "none", std::string("Open the effect chain editor on the chain of ") + t);
  add("ui.layouts", "trigger", "none", "Open or close the browser layouts (saved column sets)");
  add("ui.layout_next", "trigger", "none", "Switch to the next browser layout");
  add("ui.layout_prev", "trigger", "none", "Switch to the previous browser layout");
  add("ui.locate", "trigger", "none", "Open or close the search for moved files");
  add("ui.capture", "trigger", "none", "Open or close recording from an analogue input (record a side, split it into tracks, export)");
  add("ui.split", "trigger", "none", "Split the browser into two track tables (side by side, stacked, off)");
  add("ui.focus_pane", "trigger", "none", "Move the keyboard focus to the other track table"); 
  add("ui.close_panel", "trigger", "none", "Close the open panel, or clear the search");
  add("ui.window.browser", "trigger", "none", "Show or hide the browser window");
  add("ui.window.sampler", "trigger", "none", "Show or hide the sampler window");
  add("ui.layout", "trigger", "none", "Switch between the two- and four-deck layouts");
  add("ui.skin_next", "trigger", "none", "Switch to the next skin");
  add("ui.cheat_sheet", "trigger", "none", "Write the shortcut cheat sheet (HTML) into the data folder");
  for (int a = 1; a <= kMaxDecks; ++a)
    for (int b = 1; b <= kMaxDecks; ++b)
      if (a != b)
        o.push_back({"ui.link." + std::to_string(a) + "." + std::to_string(b), "trigger", "none",
                     "Link the playhead of deck " + std::to_string(a) + " to the playhead of deck " + std::to_string(b) + " (a mix point between the two tracks)"});
  for (int d = 1; d <= kMaxDecks; ++d) {
    o.push_back({"ui.nudge." + std::to_string(d), "value", "jog revolutions per event, signed (+ = forward); bind it as mode=repeat",
                 "Nudge the deck's pitch while the key is down (jog side bend)"});
    o.push_back({"ui.key_shift." + std::to_string(d), "value", "semitones, signed (+1 / -1)", "Move the deck's key shift by a step (turns key lock on)"});
    o.push_back({"ui.tempo_step." + std::to_string(d), "value", "tempo fader steps, signed (fader range -1..1)", "Move the tempo fader by a step"});
  }
  return o;
}

std::vector<KeyControlInfo> list_key_controls() { return control_index().all; }

std::string check_key_control(std::string_view name) {
  const ControlIndex& idx = control_index();
  if (idx.by_name.count(std::string(name))) return {};
  return "unknown control " + std::string(name);
}

// ---- Keymap -------------------------------------------------------------------------------------------------------

void Keymap::parse_into(std::vector<KeyBinding>& target, const std::string& text, const std::string& source, bool user,
                        std::vector<std::string>& warnings) {
  std::istringstream in(text);
  std::string line;
  int n = 0;
  const auto warn = [&](const std::string& why) { warnings.push_back(source + ":" + std::to_string(n) + ": " + why); };
  const auto find_at = [&](const KeyChord& c, KeyContext ctx) {
    return std::find_if(target.begin(), target.end(), [&](const KeyBinding& b) { return b.chord == c && b.context == ctx; });
  };
  while (std::getline(in, line)) {
    ++n;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string err;
    const std::vector<std::string> w = split_words(line, err);
    if (!err.empty()) {
      warn(err);
      continue;
    }
    if (w.empty()) continue;
    const std::string verb = lower(w[0]);
    if (verb != "bind" && verb != "unbind") {
      warn("unknown directive \"" + w[0] + "\"");
      continue;
    }
    if (w.size() < 2) {
      warn(verb + " needs a key chord");
      continue;
    }
    KeyChord chord;
    if (!parse_chord(w[1], chord, err)) {
      warn(err);
      continue;
    }
    KeyContext ctx = KeyContext::Global;
    KeyAction act;
    std::string desc;
    std::size_t i = 2;
    bool bad = false;
    if (verb == "bind") {
      if (w.size() < 3) {
        warn("bind needs a control name");
        continue;
      }
      act.control = w[2];
      i = 3;
      if (i < w.size() && w[i].find('=') == std::string::npos) {  // optional value
        if (!parse_number(w[i], act.value)) {
          warn("value \"" + w[i] + "\" is not a number");
          continue;
        }
        act.has_value = true;
        ++i;
      }
    }
    for (; i < w.size() && !bad; ++i) {
      const std::size_t eq = w[i].find('=');
      if (eq == std::string::npos) {
        warn("unexpected \"" + w[i] + "\"");
        bad = true;
        break;
      }
      const std::string k = lower(w[i].substr(0, eq)), v = w[i].substr(eq + 1);
      if (k == "context" && parse_context(v, ctx)) continue;
      if (k == "mode" && verb == "bind" && parse_mode(v, act.mode)) continue;
      if (k == "desc" && verb == "bind") {
        desc = v;
        continue;
      }
      warn("bad option \"" + w[i] + "\"");
      bad = true;
    }
    if (bad) continue;
    if (verb == "unbind") {
      const auto it = find_at(chord, ctx);
      if (it != target.end()) target.erase(it);
      continue;
    }
    if (const std::string why = check_key_control(act.control); !why.empty()) {
      warn(why);
      continue;
    }
    if (reserved_chord(chord)) warn(chord_label(chord, LabelStyle::Neutral) + " is reserved by the operating system (bound anyway)");
    KeyBinding b;
    b.chord = chord;
    b.context = ctx;
    b.action = act;
    b.desc = desc;
    b.user = user;
    const auto it = find_at(chord, ctx);
    if (it != target.end()) {
      if (!user) warn("duplicate chord " + chord_text(chord) + " (the later line wins)");
      *it = b;
    } else {
      target.push_back(std::move(b));
    }
  }
}

std::vector<std::string> Keymap::load_defaults(const std::string& text, const std::string& source) {
  std::vector<std::string> warnings;
  held_.clear();
  defaults_.clear();
  parse_into(defaults_, text, source, false, warnings);
  bindings_ = defaults_;
  return warnings;
}

std::vector<std::string> Keymap::load_user(const std::string& text, const std::string& source) {
  std::vector<std::string> warnings;
  bindings_ = defaults_;
  parse_into(bindings_, text, source, true, warnings);
  for (KeyBinding& b : bindings_) {  // a binding identical to the default one is not a user change
    const auto d = std::find_if(defaults_.begin(), defaults_.end(), [&](const KeyBinding& x) {
      return x.chord == b.chord && x.context == b.context && x.action == b.action;
    });
    b.user = d == defaults_.end();
  }
  return warnings;
}

namespace {
bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}
}  // namespace

std::vector<std::string> Keymap::load_files(const std::string& default_path, const std::string& user_path) {
  std::string text;
  std::vector<std::string> warnings;
  if (!default_path.empty() && read_file(default_path, text)) warnings = load_defaults(text, "default.dhkeys");
  else load_defaults("", "default.dhkeys");
  if (!user_path.empty() && read_file(user_path, text)) {
    const auto more = load_user(text, "keymap.dhkeys");
    warnings.insert(warnings.end(), more.begin(), more.end());
  }
  return warnings;
}

namespace {
std::string binding_line(const KeyBinding& b) {
  std::string s = "bind " + chord_text(b.chord) + " " + b.action.control;
  if (b.action.has_value) s += " " + fmt_num(b.action.value);
  if (b.action.mode != KeyMode::Press) s += std::string(" mode=") + key_mode_name(b.action.mode);
  if (b.context != KeyContext::Global) s += std::string(" context=") + key_context_name(b.context);
  if (!b.desc.empty()) s += " desc=" + quote(b.desc);
  return s;
}
}  // namespace

std::string Keymap::user_text() const {
  std::string out =
      "# DeckHand keyboard shortcuts: your changes on top of the shipped defaults (resources/keymaps/default.dhkeys).\n"
      "# Only differences are listed, so new default shortcuts from updates still appear.\n"
      "#   bind   <chord> <control> [value] [mode=press|hold|toggle|repeat] [context=global|browser|text]\n"
      "#   unbind <chord> [context=...]      removes a default shortcut\n";
  std::string unbinds, binds;
  for (const KeyBinding& d : defaults_) {
    const KeyBinding* e = find(d.chord, d.context);
    if (!e) unbinds += std::string("unbind ") + chord_text(d.chord) + (d.context != KeyContext::Global ? std::string(" context=") + key_context_name(d.context) : "") + "\n";
  }
  for (const KeyBinding& b : bindings_) {
    const auto d = std::find_if(defaults_.begin(), defaults_.end(), [&](const KeyBinding& x) {
      return x.chord == b.chord && x.context == b.context && x.action == b.action;
    });
    if (d == defaults_.end()) binds += binding_line(b) + "\n";
  }
  return out + (unbinds.empty() && binds.empty() ? "" : "\n") + unbinds + binds;
}

bool Keymap::user_changed() const {
  if (bindings_.size() != defaults_.size()) return true;
  for (const KeyBinding& b : bindings_)
    if (b.user) return true;
  return false;
}

bool Keymap::save_user(const std::string& path, std::string& error) const {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    error = "cannot write " + path;
    return false;
  }
  f << user_text();
  f.flush();
  if (!f) {
    error = "cannot write " + path;
    return false;
  }
  return true;
}

const KeyBinding* Keymap::find(const KeyChord& chord, KeyContext context) const {
  for (const KeyBinding& b : bindings_)
    if (b.context == context && b.chord == chord) return &b;
  return nullptr;
}

std::vector<const KeyBinding*> Keymap::for_action(const KeyAction& a) const {
  std::vector<const KeyBinding*> r;
  for (const KeyBinding& b : bindings_)
    if (b.action == a) r.push_back(&b);
  return r;
}

const KeyBinding* Keymap::conflict(const KeyChord& chord, KeyContext context, const KeyAction& a) const {
  const KeyBinding* b = find(chord, context);
  if (b && !(b->action == a)) return b;
  // A global binding is shadowed by (and shadows) a context-specific one on the same chord.
  if (context == KeyContext::Global) {
    const KeyBinding* o = find(chord, KeyContext::Browser);
    if (o && !(o->action == a)) return o;
  } else if (context == KeyContext::Browser) {
    const KeyBinding* g = find(chord, KeyContext::Global);
    if (g && !(g->action == a)) return g;
  }
  return nullptr;
}

void Keymap::bind(const KeyChord& chord, KeyContext context, const KeyAction& a, const std::string& desc) {
  KeyBinding nb;
  nb.chord = chord;
  nb.context = context;
  nb.action = a;
  nb.desc = desc;
  const auto d = std::find_if(defaults_.begin(), defaults_.end(), [&](const KeyBinding& x) {
    return x.chord == chord && x.context == context && x.action == a;
  });
  nb.user = d == defaults_.end();
  if (d != defaults_.end() && desc.empty()) nb.desc = d->desc;
  for (KeyBinding& b : bindings_)
    if (b.chord == chord && b.context == context) {
      if (nb.desc.empty() && b.action == a) nb.desc = b.desc;
      b = nb;
      return;
    }
  bindings_.push_back(nb);
}

bool Keymap::unbind(const KeyChord& chord, KeyContext context) {
  const auto it = std::find_if(bindings_.begin(), bindings_.end(), [&](const KeyBinding& b) { return b.chord == chord && b.context == context; });
  if (it == bindings_.end()) return false;
  bindings_.erase(it);
  return true;
}

void Keymap::unbind_action(const KeyAction& a) {
  bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(), [&](const KeyBinding& b) { return b.action == a; }), bindings_.end());
}

void Keymap::reset_action(const KeyAction& a) {
  unbind_action(a);
  for (const KeyBinding& d : defaults_)
    if (d.action == a) {
      unbind(d.chord, d.context);  // the default chord wins back from whatever took it
      bindings_.push_back(d);
    }
}

bool Keymap::action_changed(const KeyAction& a) const {
  std::set<std::string> now, def;
  for (const KeyBinding& b : bindings_)
    if (b.action == a) now.insert(chord_text(b.chord) + "|" + key_context_name(b.context));
  for (const KeyBinding& b : defaults_)
    if (b.action == a) def.insert(chord_text(b.chord) + "|" + key_context_name(b.context));
  return now != def;
}

void Keymap::reset_all() { bindings_ = defaults_; }

// ---- running ----------------------------------------------------------------------------------------------------------

KeyChord Keymap::chord_of(const KeyEvent& ev) {
  KeyChord c;
  c.mods = ev.mods & kModMask;
  c.key = key_name_from_hid(ev.physical);
  if (c.key.empty()) c.key = key_name_from_char(ev.character);
  return c;
}

bool Keymap::handle(const KeyEvent& ev, KeyFocus focus, std::vector<KeyCall>& out) {
  const KeyChord chord = chord_of(ev);
  if (!ev.down) {  // releasing a held key: always, whatever has focus now
    for (std::size_t i = 0; i < held_.size(); ++i) {
      if (held_[i].key == chord.key) {
        out.push_back(held_[i].release);
        held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
      }
    }
    return false;
  }
  if (chord.key.empty()) return false;
  const KeyBinding* b = nullptr;
  switch (focus) {
    case KeyFocus::Text: b = find(chord, KeyContext::Text); break;
    case KeyFocus::Panel: b = find(chord, KeyContext::Global); break;
    case KeyFocus::Browser:
      b = find(chord, KeyContext::Browser);
      if (!b) b = find(chord, KeyContext::Global);
      break;
  }
  if (!b) return false;
  const KeyAction& a = b->action;
  switch (a.mode) {
    case KeyMode::Press:
    case KeyMode::Toggle:
      if (!ev.repeat) out.push_back({a.control, a.has_value && a.mode == KeyMode::Press, a.value});
      break;
    case KeyMode::Repeat: out.push_back({a.control, a.has_value, a.value}); break;
    case KeyMode::Hold:
      if (ev.repeat) break;
      for (const Held& h : held_)
        if (h.key == chord.key) return true;
      out.push_back({a.control, true, a.has_value ? a.value : 1.0});
      held_.push_back({chord.key, {a.control, true, 0.0}});
      break;
  }
  return true;
}

void Keymap::release_all(std::vector<KeyCall>& out) {
  for (const Held& h : held_) out.push_back(h.release);
  held_.clear();
}

// ---- cheat sheet ---------------------------------------------------------------------------------------------------------

std::string Keymap::cheat_sheet(Format format) const {
  struct Row {
    std::string shortcuts, what, control;
  };
  std::vector<std::pair<std::string, std::vector<Row>>> groups;
  std::vector<std::pair<std::string, std::vector<const KeyBinding*>>> by_action;  // keyed by control+value+mode
  std::map<std::string, std::size_t> seen;
  const LabelStyle style = format == Format::Markdown ? LabelStyle::Neutral : LabelStyle::Native;
  const auto info = [&](const std::string& n) -> const KeyControlInfo* {
    const ControlIndex& idx = control_index();
    const auto it = idx.by_name.find(n);
    return it == idx.by_name.end() ? nullptr : &idx.all[it->second];
  };
  for (const KeyBinding& b : bindings_) {
    if (format == Format::Markdown && b.action.control.compare(0, 4, "deck") == 0 && b.action.control.size() > 4 &&
        (b.action.control[4] == '3' || b.action.control[4] == '4'))
      continue;
    if (format == Format::Markdown && (b.action.control.compare(0, 8, "ui.load.") == 0 || b.action.control.compare(0, 9, "ui.nudge.") == 0 ||
                                       b.action.control.compare(0, 14, "ui.tempo_step.") == 0 || b.action.control.compare(0, 13, "ui.key_shift.") == 0) &&
        (b.action.control.back() == '3' || b.action.control.back() == '4'))
      continue;
    const std::string key = b.action.control + "|" + (b.action.has_value ? fmt_num(b.action.value) : "-") + "|" + key_mode_name(b.action.mode) +
                            "|" + key_context_name(b.context);
    const auto it = seen.find(key);
    if (it == seen.end()) {
      seen[key] = by_action.size();
      by_action.push_back({key, {&b}});
    } else {
      by_action[it->second].second.push_back(&b);
    }
  }
  for (const auto& entry : by_action) {
    const KeyBinding& first = *entry.second.front();
    const std::string group = key_control_group(first.action.control);
    std::string chords;
    for (const KeyBinding* b : entry.second) {
      if (!chords.empty()) chords += ", ";
      chords += chord_label(b->chord, style);
      if (b->context == KeyContext::Text) chords += " (text field)";
    }
    std::string what = first.desc;
    if (what.empty()) {
      const KeyControlInfo* ci = info(first.action.control);
      what = ci ? ci->help : first.action.control;
    }
    std::string control = first.action.control;
    if (first.action.has_value) control += " " + fmt_num(first.action.value);
    if (first.action.mode != KeyMode::Press) control += std::string(" (") + key_mode_name(first.action.mode) + ")";
    auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& p) { return p.first == group; });
    if (g == groups.end()) {
      groups.push_back({group, {}});
      g = groups.end() - 1;
    }
    g->second.push_back({chords, what, control});
  }
  std::stable_sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  // Deck groups first, then the rest alphabetically: "Application" and "Browser" read better before "Deck 1" in print,
  // so only the order of the numbered decks is fixed.
  std::string out;
  if (format == Format::Html) {
    out += "<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>DeckHand keyboard shortcuts</title>\n<style>\n"
           "body{font:14px/1.4 -apple-system,Segoe UI,sans-serif;margin:24px;color:#111}\n"
           "h1{font-size:20px}h2{font-size:15px;margin:18px 0 4px;border-bottom:1px solid #999}\n"
           "table{border-collapse:collapse;width:100%}td{padding:2px 8px 2px 0;vertical-align:top}\n"
           "td.k{white-space:nowrap;font-family:ui-monospace,Menlo,Consolas,monospace;width:34%}\n"
           "@media print{body{margin:8mm;font-size:10pt}h2{break-after:avoid}tr{break-inside:avoid}}\n"
           "</style></head><body>\n<h1>DeckHand keyboard shortcuts</h1>\n";
  } else if (format == Format::Text) {
    out += "DeckHand keyboard shortcuts\n===========================\n";
  }
  for (const auto& g : groups) {
    if (format == Format::Html) out += "<h2>" + html_escape(g.first) + "</h2>\n<table>\n";
    else if (format == Format::Markdown) out += "### " + g.first + "\n\n| Shortcut | What it does | Control |\n|---|---|---|\n";
    else out += "\n" + g.first + "\n";
    for (const Row& r : g.second) {
      if (format == Format::Html)
        out += "<tr><td class=\"k\">" + html_escape(r.shortcuts) + "</td><td>" + html_escape(r.what) + "</td></tr>\n";
      else if (format == Format::Markdown) {
        std::string k = r.shortcuts, c = r.control;
        std::string esc_k;
        for (char ch : k) {
          if (ch == '|') esc_k += "\\|";
          else esc_k += ch;
        }
        out += "| `" + esc_k + "` | " + r.what + " | `" + c + "` |\n";
      } else {
        std::string pad = r.shortcuts;
        if (pad.size() < 26) pad.append(26 - pad.size(), ' ');
        out += "  " + pad + r.what + "\n";
      }
    }
    if (format == Format::Html) out += "</table>\n";
    else if (format == Format::Markdown) out += "\n";
  }
  if (format == Format::Html) out += "</body></html>\n";
  return out;
}

}  // namespace deckhand
