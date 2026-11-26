#pragma once
// Keyboard shortcuts: key chords bound to named controls (controls.hpp) or application actions ("ui.*", below).
//
// The keymap is plain data plus a small state machine (held keys); it does not touch the engine or the UI, so
// it can be tested on its own. The application feeds it key events and carries out the calls it returns.
//
// Files (.dhkeys, one directive per line, '#' starts a comment):
//   bind   <chord> <control> [value] [mode=press|hold|toggle|repeat] [context=global|browser|text] [desc="text"]
//   unbind <chord> [context=...]
// A chord is modifiers and one key joined by '+': "mod+shift+k", "space", "f5", "ctrl+alt+left", "shift+slash".
// `mod` is the platform's main modifier (Command on macOS, Control elsewhere). The shipped defaults come first;
// the user's file is applied on top, so a user's `bind` replaces whatever the chord did and `unbind` removes it.
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "deckhand/app/controls.hpp"

namespace deckhand {

// Modifier bits: the same values as DH_MOD_* in capi.h.
enum KeyMod : std::uint32_t { kModShift = 1, kModCommand = 2, kModAlt = 4, kModControl = 8, kModMask = 15 };

enum class KeyContext : std::uint8_t {
  Global,   // any time no text field has the keyboard (also while a panel is open)
  Browser,  // only while the main window has the keyboard: no panel, no text field
  Text,     // only while a text field has the keyboard (search box, rename, settings fields)
};

// What a key does while it is down.
enum class KeyMode : std::uint8_t {
  Press,   // once on key down (auto-repeat ignored)
  Hold,    // value (default 1) on key down, 0 on key up
  Toggle,  // once on key down, without a value (toggles, or taps a button)
  Repeat,  // on key down and on every auto-repeat
};

struct KeyChord {
  std::string key;  // canonical key name: "a", "5", "space", "f5", "left", "comma", "kp1" ...
  std::uint32_t mods = 0;
  bool operator==(const KeyChord& o) const { return key == o.key && mods == o.mods; }
  bool operator<(const KeyChord& o) const { return key != o.key ? key < o.key : mods < o.mods; }
};

// What a key does: which control, with which value, how.
struct KeyAction {
  std::string control;  // "deck1.play", "deck2.hotcue.3", "ui.load.1" ...
  bool has_value = false;
  double value = 0;
  KeyMode mode = KeyMode::Press;
  bool operator==(const KeyAction& o) const {
    return control == o.control && has_value == o.has_value && (!has_value || value == o.value) && mode == o.mode;
  }
};

struct KeyBinding {
  KeyChord chord;
  KeyContext context = KeyContext::Global;
  KeyAction action;
  std::string desc;     // short description (English); empty = use the control's help
  bool user = false;    // set by the user's file or the shortcuts panel, not by the shipped defaults
};

// One keyboard event as the shell reports it.
struct KeyEvent {
  std::uint32_t physical = 0;   // USB HID keyboard usage (page 0x07), 0 = unknown
  std::uint32_t character = 0;  // Unicode code point the key produces without modifiers, 0 = none
  std::uint32_t mods = 0;
  bool down = true;
  bool repeat = false;
};

// Who has the keyboard right now.
enum class KeyFocus : std::uint8_t { Browser, Panel, Text };

// A control call the application should make.
struct KeyCall {
  std::string control;
  bool has_value = false;
  double value = 0;
};

// An application action ("ui.*"): the keymap names them, the application carries them out.
struct UiActionInfo {
  std::string name, kind, range, help;
};
std::vector<UiActionInfo> list_ui_actions();

// Every name a binding may use: list_controls() plus list_ui_actions().
struct KeyControlInfo {
  std::string name, kind, range, help;
};
std::vector<KeyControlInfo> list_key_controls();

// Names and parsing of keys and chords.
std::string key_name_from_hid(std::uint32_t hid);          // "" when unknown
std::string key_name_from_char(std::uint32_t codepoint);   // "" when no key has that character
std::uint32_t key_hid_from_name(std::string_view name);    // 0 when unknown
bool parse_chord(std::string_view text, KeyChord& out, std::string& error);
std::string chord_text(const KeyChord& c);                 // canonical, as written in files: "mod+shift+k"
// For people: "Cmd+Shift+K" in the platform's wording (Native), or "Mod+Shift+K" the same on every platform (Neutral, for docs).
enum class LabelStyle { Native, Neutral };
std::string chord_label(const KeyChord& c, LabelStyle style = LabelStyle::Native);
// Which area a control belongs to, for lists and cheat sheets: "Deck 1", "Browser", "Mixer", "Pre-listen", "Sampler", "Application".
std::string key_control_group(const std::string& control);
const char* key_context_name(KeyContext c);
const char* key_mode_name(KeyMode m);

class Keymap {
 public:
  // ---- loading ------------------------------------------------------------------------------------
  // Both return one message per problem ("default.dhkeys:12: unknown control deck9.play"); bad lines are skipped.
  // The user layer is parsed on top of whatever the defaults loaded; load_defaults() clears everything first.
  std::vector<std::string> load_defaults(const std::string& text, const std::string& source = "default.dhkeys");
  std::vector<std::string> load_user(const std::string& text, const std::string& source = "keymap.dhkeys");
  // Convenience: both from files; missing files are not an error (an empty keymap / no overrides).
  std::vector<std::string> load_files(const std::string& default_path, const std::string& user_path);

  // The user's file as it should be saved: only the differences from the defaults.
  std::string user_text() const;
  bool save_user(const std::string& path, std::string& error) const;
  bool user_changed() const;  // differs from the defaults

  // ---- reading --------------------------------------------------------------------------------------
  const std::vector<KeyBinding>& bindings() const { return bindings_; }
  const KeyBinding* find(const KeyChord& chord, KeyContext context) const;
  std::vector<const KeyBinding*> for_action(const KeyAction& a) const;  // same control, value and mode
  // The binding this chord would collide with when bound to `a` in `context` (null = free).
  bool action_changed(const KeyAction& a) const;  // its chords differ from the defaults
  const KeyBinding* conflict(const KeyChord& chord, KeyContext context, const KeyAction& a) const;

  // ---- editing (the shortcuts panel and the API) -------------------------------------------------------
  void bind(const KeyChord& chord, KeyContext context, const KeyAction& a, const std::string& desc = "");  // replaces the chord's binding
  bool unbind(const KeyChord& chord, KeyContext context);
  void unbind_action(const KeyAction& a);  // every chord of one action
  void reset_action(const KeyAction& a);   // back to the defaults for that action
  void reset_all();                        // back to the defaults

  // ---- running --------------------------------------------------------------------------------------
  // Handles one key event. Returns true when a binding took it (the shell must not also type its character).
  // The calls to carry out are appended to `out`. Key-up events release held (mode=hold) keys even if focus
  // changed in between.
  bool handle(const KeyEvent& ev, KeyFocus focus, std::vector<KeyCall>& out);
  // Releases everything still held (window lost focus, panel opened).
  void release_all(std::vector<KeyCall>& out);

  // The chord of an event, by physical key when known, else by character; "" key = not bindable.
  static KeyChord chord_of(const KeyEvent& ev);

  // Printable cheat sheet of the effective bindings, grouped by area. Markdown is the neutral table used by
  // docs/SHORTCUTS.md (decks 3 and 4 are left out: they repeat decks 1 and 2 with Alt held).
  enum class Format { Text, Html, Markdown };
  std::string cheat_sheet(Format format) const;

 private:
  struct Held {
    std::string key;  // the physical key that is down
    KeyCall release;
  };
  void parse_into(std::vector<KeyBinding>& target, const std::string& text, const std::string& source, bool user,
                  std::vector<std::string>& warnings);
  std::vector<KeyBinding> defaults_, bindings_;
  std::vector<Held> held_;
};

// Checks a binding's control name; returns "" when valid, else why not.
std::string check_key_control(std::string_view name);

}  // namespace deckhand
