// Keyboard shortcuts: parsing, layering, conflicts, contexts, hold/repeat, the shipped defaults, and docs/SHORTCUTS.md.
// `test_keymap --write-docs` regenerates the table in docs/SHORTCUTS.md from resources/keymaps/default.dhkeys.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include "check.hpp"
#include "deckhand/app/keymap.hpp"

using namespace deckhand;

static std::string slurp(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static KeyEvent ev(const char* key, std::uint32_t mods = 0, bool down = true, bool repeat = false) {
  KeyEvent e;
  e.physical = key_hid_from_name(key);
  e.mods = mods;
  e.down = down;
  e.repeat = repeat;
  return e;
}

static KeyChord chord(const char* t) {
  KeyChord c;
  std::string err;
  CHECK(parse_chord(t, c, err));
  return c;
}

static const char* kBegin = "<!-- BEGIN GENERATED -->";
static const char* kEnd = "<!-- END GENERATED -->";

int main(int argc, char** argv) {
  const std::string defaults_text = slurp("resources/keymaps/default.dhkeys");
  Keymap shipped;
  const auto shipped_warnings = shipped.load_defaults(defaults_text);

  if (argc > 1 && std::strcmp(argv[1], "--write-docs") == 0) {
    std::string doc = slurp("docs/SHORTCUTS.md");
    const auto a = doc.find(kBegin), b = doc.find(kEnd);
    CHECK(a != std::string::npos && b != std::string::npos && a < b);
    doc = doc.substr(0, a + std::strlen(kBegin)) + "\n\n" + shipped.cheat_sheet(Keymap::Format::Markdown) + doc.substr(b);
    std::ofstream("docs/SHORTCUTS.md", std::ios::binary | std::ios::trunc) << doc;
    std::printf("docs/SHORTCUTS.md written\n");
    return 0;
  }

  // ---- chords and key names
  {
    KeyChord c = chord("Shift+Mod+K");
    CHECK(c.key == "k" && (c.mods & kModShift) && (c.mods & kModMask) != kModShift);
    CHECK(chord_text(c) == "mod+shift+k");
    CHECK(chord("cmd+,").key == "comma" && chord("ctrl+alt+Left").mods == (kModControl | kModAlt));
    CHECK(chord("return").key == "enter" && chord("esc").key == "escape" && chord("f12").key == "f12");
    CHECK(chord("shift+/").key == "slash");
    KeyChord bad;
    std::string err;
    CHECK(!parse_chord("hyper+k", bad, err) && !err.empty());
    CHECK(!parse_chord("ctrl+", bad, err) && !parse_chord("blorp", bad, err));
    CHECK(key_name_from_hid(4) == "a" && key_name_from_hid(30) == "1" && key_name_from_hid(39) == "0" && key_name_from_hid(44) == "space");
    CHECK(key_name_from_hid(58) == "f1" && key_name_from_hid(82) == "up" && key_name_from_hid(54) == "comma" && key_name_from_hid(9999).empty());
    CHECK(key_hid_from_name("a") == 4 && key_hid_from_name("left") == 80);
    CHECK(key_name_from_char('Q') == "q" && key_name_from_char(',') == "comma" && key_name_from_char(0x4e2d).empty());
    KeyEvent by_char;  // no physical code: the character names the key
    by_char.character = 'j';
    CHECK(Keymap::chord_of(by_char).key == "j");
    KeyEvent both = ev("a");
    both.character = 'z';  // a layout that prints Z on the A position: the physical key wins
    CHECK(Keymap::chord_of(both).key == "a");
  }

  // ---- parsing, warnings, layering and unbinding
  {
    Keymap k;
    const auto w = k.load_defaults(
        "# comment\n"
        "bind a deck1.play\n"
        "bind shift+a deck1.cue mode=hold desc=\"Cue, held\"\n"
        "bind b deck1.beatjump -4 mode=repeat\n"
        "bind c deck9.play\n"       // unknown control
        "bind d deck1.play bogus\n"  // not a number
        "bind e\n"
        "frobnicate x\n"
        "bind f deck1.play context=nowhere\n"
        "bind g deck1.play\n"
        "bind g deck2.play\n");  // duplicate chord
    CHECK(w.size() == 6);
    CHECK(w[0].find("default.dhkeys:5") == 0 && w[0].find("deck9.play") != std::string::npos);
    CHECK(k.bindings().size() == 4);
    const KeyBinding* b = k.find(chord("shift+a"), KeyContext::Global);
    CHECK(b && b->action.mode == KeyMode::Hold && b->desc == "Cue, held");
    CHECK(k.find(chord("g"), KeyContext::Global)->action.control == "deck2.play");
    const KeyBinding* jump = k.find(chord("b"), KeyContext::Global);
    CHECK(jump && jump->action.has_value && jump->action.value == -4);
    CHECK(!k.user_changed());

    const auto uw = k.load_user("unbind a\nbind a deck2.sync\nbind shift+a deck1.sync\nunbind z\nbind q deck3.nothing\n");
    CHECK(uw.size() == 1);  // only the unknown control; unbinding a chord that is not there is fine
    CHECK(k.find(chord("a"), KeyContext::Global)->action.control == "deck2.sync");
    CHECK(k.find(chord("shift+a"), KeyContext::Global)->action.control == "deck1.sync");
    CHECK(k.user_changed());
    // a user layer over the same defaults again starts from the defaults
    k.load_user("unbind g\n");
    CHECK(!k.find(chord("g"), KeyContext::Global) && k.find(chord("a"), KeyContext::Global)->action.control == "deck1.play");

    // the saved file holds only the differences, and loads back to the same keymap
    k.load_user("unbind g\nbind a deck2.sync\nbind x deck1.reloop context=browser\n");
    const std::string saved = k.user_text();
    CHECK(saved.find("unbind g") != std::string::npos && saved.find("bind a deck2.sync") != std::string::npos);
    CHECK(saved.find("bind b ") == std::string::npos);  // untouched defaults are not copied
    Keymap again;
    again.load_defaults(
        "bind a deck1.play\nbind shift+a deck1.cue mode=hold desc=\"Cue, held\"\nbind b deck1.beatjump -4 mode=repeat\nbind g deck2.play\n");
    CHECK(again.load_user(saved).empty());
    CHECK(again.bindings().size() == k.bindings().size());
    CHECK(again.user_text() == saved);
    // an update that adds a default shortcut does not touch the user's changes
    again.load_defaults("bind a deck1.play\nbind g deck2.play\nbind h deck1.sync\n");
    again.load_user(saved);
    CHECK(again.find(chord("h"), KeyContext::Global) && !again.find(chord("g"), KeyContext::Global));

    k.reset_all();
    CHECK(!k.user_changed() && k.find(chord("g"), KeyContext::Global));
  }

  // ---- editing and conflicts
  {
    Keymap k;
    k.load_defaults("bind a deck1.play\nbind b deck1.sync\nbind c deck1.cue mode=hold\nbind shift+d deck1.sync context=browser\n");
    KeyAction play;
    play.control = "deck1.play";
    KeyAction sync;
    sync.control = "deck1.sync";
    const KeyBinding* c = k.conflict(chord("b"), KeyContext::Global, play);
    CHECK(c && c->action.control == "deck1.sync");
    CHECK(!k.conflict(chord("b"), KeyContext::Global, sync));  // its own chord is no conflict
    CHECK(!k.conflict(chord("z"), KeyContext::Global, play));
    CHECK(k.conflict(chord("shift+d"), KeyContext::Global, play));  // shadowed by the browser binding
    k.bind(chord("b"), KeyContext::Global, play);
    CHECK(k.find(chord("b"), KeyContext::Global)->action.control == "deck1.play" && k.find(chord("b"), KeyContext::Global)->user);
    CHECK(k.action_changed(play) && k.action_changed(sync));
    k.reset_action(play);
    CHECK(!k.action_changed(play) && k.find(chord("a"), KeyContext::Global));
    CHECK(!k.find(chord("b"), KeyContext::Global));  // reset restores play's own chords; b was taken from sync and stays free
    k.unbind_action(sync);
    CHECK(k.for_action(sync).empty());
    k.reset_action(sync);
    CHECK(k.find(chord("b"), KeyContext::Global)->action.control == "deck1.sync");  // the default chord wins back
    CHECK(k.unbind(chord("a"), KeyContext::Global) && !k.unbind(chord("a"), KeyContext::Global));
  }

  // ---- contexts: text focus suppresses everything but text bindings
  {
    Keymap k;
    k.load_defaults("bind a deck1.play\nbind b deck1.sync context=browser\nbind escape ui.close_panel context=text\nbind mod+f ui.focus_search\n");
    std::vector<KeyCall> out;
    CHECK(k.handle(ev("a"), KeyFocus::Browser, out) && out.size() == 1 && out[0].control == "deck1.play" && !out[0].has_value);
    out.clear();
    CHECK(!k.handle(ev("a"), KeyFocus::Text, out) && out.empty());       // typing in the search box
    CHECK(!k.handle(ev("f", kModControl | kModCommand), KeyFocus::Text, out) && out.empty());
    CHECK(k.handle(ev("a"), KeyFocus::Panel, out) && out.size() == 1);   // global shortcuts work with a panel open
    out.clear();
    CHECK(k.handle(ev("b"), KeyFocus::Browser, out) && out.size() == 1);
    out.clear();
    CHECK(!k.handle(ev("b"), KeyFocus::Panel, out) && !k.handle(ev("b"), KeyFocus::Text, out) && out.empty());  // browser-only
    CHECK(k.handle(ev("escape"), KeyFocus::Text, out) && out.size() == 1);
    out.clear();
    CHECK(!k.handle(ev("escape"), KeyFocus::Browser, out));  // text-only
    // modifiers must match exactly
    CHECK(!k.handle(ev("a", kModShift), KeyFocus::Browser, out) && !k.handle(ev("a", kModAlt), KeyFocus::Browser, out) && out.empty());
    // unknown keys are ignored, never crash
    KeyEvent none;
    CHECK(!k.handle(none, KeyFocus::Browser, out));
  }

  // ---- hold and release, repeat, press, toggle
  {
    Keymap k;
    k.load_defaults(
        "bind a deck1.cue mode=hold\n"
        "bind b deck1.hotcue.2 7 mode=hold\n"
        "bind c deck1.beatjump 4 mode=repeat\n"
        "bind d deck1.beatjump 4\n"
        "bind e deck1.play mode=toggle\n"
        "bind f deck1.tempo 0\n");
    std::vector<KeyCall> out;
    CHECK(k.handle(ev("a"), KeyFocus::Browser, out) && out.size() == 1 && out[0].has_value && out[0].value == 1);
    out.clear();
    CHECK(k.handle(ev("a", 0, true, true), KeyFocus::Browser, out) && out.empty());  // auto-repeat of a held key: nothing
    // the release comes even if a text field took the keyboard meanwhile, and whatever modifiers are down
    CHECK(k.handle(ev("a", kModShift, false), KeyFocus::Text, out) && out.size() == 1 && out[0].value == 0 && out[0].has_value);
    out.clear();
    CHECK(!k.handle(ev("a", 0, false), KeyFocus::Browser, out) && out.empty());  // nothing held any more
    CHECK(k.handle(ev("b"), KeyFocus::Browser, out) && out[0].value == 7);
    out.clear();
    k.release_all(out);  // window lost focus
    CHECK(out.size() == 1 && out[0].control == "deck1.hotcue.2" && out[0].value == 0);
    out.clear();
    k.release_all(out);
    CHECK(out.empty());
    CHECK(k.handle(ev("c"), KeyFocus::Browser, out) && k.handle(ev("c", 0, true, true), KeyFocus::Browser, out) && out.size() == 2 && out[1].value == 4);
    out.clear();
    CHECK(k.handle(ev("d"), KeyFocus::Browser, out) && k.handle(ev("d", 0, true, true), KeyFocus::Browser, out) && out.size() == 1);  // press ignores repeat
    out.clear();
    CHECK(k.handle(ev("e"), KeyFocus::Browser, out) && !out[0].has_value);
    out.clear();
    CHECK(k.handle(ev("f"), KeyFocus::Browser, out) && out[0].has_value && out[0].value == 0);
    out.clear();
    CHECK(!k.handle(ev("c", 0, false), KeyFocus::Browser, out) && out.empty());  // releasing a non-hold key does nothing
  }

  // ---- the shipped defaults
  {
    for (const std::string& w : shipped_warnings) std::fprintf(stderr, "default.dhkeys: %s\n", w.c_str());
    CHECK(shipped_warnings.empty());  // unknown controls, duplicate chords, reserved chords, syntax
    CHECK(shipped.bindings().size() > 200);
    std::set<std::string> names;
    for (const ControlInfo& c : list_controls()) names.insert(c.name);
    for (const UiActionInfo& u : list_ui_actions()) CHECK(names.insert(u.name).second);  // ui names do not clash with controls
    std::set<std::string> chords;
    for (const KeyBinding& b : shipped.bindings()) {
      CHECK(names.count(b.action.control));
      CHECK(check_key_control(b.action.control).empty());
      CHECK(chords.insert(chord_text(b.chord) + "|" + key_context_name(b.context)).second);  // no duplicate chord within a context
      CHECK(b.chord.key != "q" || (b.chord.mods & kModMask) != kModCommand);              // nothing the OS keeps for itself
    }
    // a chord bound in global and browser contexts would be ambiguous
    for (const KeyBinding& b : shipped.bindings())
      if (b.context == KeyContext::Browser) CHECK(!shipped.find(b.chord, KeyContext::Global));
    // the workflow the defaults promise
    for (const char* must : {"ui.load.1", "ui.load.2", "ui.load.3", "ui.load.4", "ui.browse_up", "ui.browse_down", "ui.browse_page_down", "ui.focus_search",
                             "ui.playlist_next", "ui.playlist_prev", "ui.preview_toggle", "ui.preview_seek", "ui.add_to_prepare", "ui.rate", "ui.settings",
                             "ui.shortcuts", "ui.undo_edit", "deck1.play", "deck2.play", "deck1.cue", "deck2.cue", "deck1.sync", "deck1.loop_in",
                             "deck1.loop_out", "deck1.loop_scale", "deck1.beatjump", "deck1.tempo", "deck1.wave_zoom_in", "deck2.hotcue.8",
                             "deck1.hotcue_clear.8", "deck1.cue_set", "deck1.memory_cue_set", "ui.nudge.1", "ui.nudge.2"}) {
      bool found = false;
      for (const KeyBinding& b : shipped.bindings()) found = found || b.action.control == must;
      if (!found) std::fprintf(stderr, "no default shortcut for %s\n", must);
      CHECK(found);
    }
    // 8 hot cues on each of 4 decks
    int pads = 0;
    for (const KeyBinding& b : shipped.bindings()) pads += b.action.control.find(".hotcue.") != std::string::npos;
    CHECK(pads == 32);
    // the cheat sheet renders every format
    CHECK(shipped.cheat_sheet(Keymap::Format::Html).find("<h2>Deck 1</h2>") != std::string::npos);
    CHECK(shipped.cheat_sheet(Keymap::Format::Text).find("Browser") != std::string::npos);
  }

  // ---- docs/SHORTCUTS.md is generated from the file
  {
    const std::string doc = slurp("docs/SHORTCUTS.md");
    const auto a = doc.find(kBegin), b = doc.find(kEnd);
    CHECK(a != std::string::npos && b != std::string::npos && a < b);
    const std::string have = doc.substr(a + std::strlen(kBegin), b - a - std::strlen(kBegin));
    const std::string want = "\n\n" + shipped.cheat_sheet(Keymap::Format::Markdown);
    if (have != want) std::fprintf(stderr, "docs/SHORTCUTS.md is out of date: run `test_keymap --write-docs` from the repository root\n");
    CHECK(have == want);
  }

  std::printf("test_keymap: ok\n");
  return 0;
}
