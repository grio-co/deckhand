// Controller layer: pad-mode pairs (repeated presses), the stems pad mode on the DDJ-FLX4 mapping, key actions, 16 hot
// cue slots, and that mappings written before these extensions still load and behave the same.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "check.hpp"
#include "deckhand/midi/controller.hpp"

using namespace deckhand;

namespace {

struct FakeIO : MidiIO {
  std::vector<std::string> names{"DDJ-FLX4"};
  std::vector<std::vector<std::uint8_t>> sent;
  std::map<int, int> known;  // the value each LED note holds (the hub sends only changes)
  std::vector<std::string> devices() override { return names; }
  void send(const std::string&, const std::uint8_t* d, std::size_t n) override {
    sent.emplace_back(d, d + n);
    if (n == 3) known[d[0] << 8 | d[1]] = d[2];
  }
  // what the LED shows now, -1 when never sent
  int val(std::uint8_t a, std::uint8_t b) const {
    const auto it = known.find(a << 8 | b);
    return it == known.end() ? -1 : it->second;
  }
  // value sent since the last clear, -1 when nothing was sent
  int last(std::uint8_t a, std::uint8_t b) const {
    int v = -1;
    for (const auto& m : sent)
      if (m.size() == 3 && m[0] == a && m[1] == b) v = m[2];
    return v;
  }
};

struct Sink : ControlSink {
  std::vector<Command> commands;
  void command(const Command& c) override { commands.push_back(c); }
  void browse(int) override {}
  void browse_press() override {}
  void load_selected(int) override {}
  const Command& last() const { return commands.back(); }
};

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  CHECK(f.good());
  std::stringstream t;
  t << f.rdbuf();
  return t.str();
}

struct Rig {
  Sink sink;
  FakeIO io;
  ControllerHub hub;
  DeckView decks[2];
  explicit Rig(const Mapping& m) : hub(sink, {m}) { hub.set_io(&io); }
  void send(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
    const std::uint8_t msg[3] = {a, b, c};
    hub.midi("DDJ-FLX4", msg, 3);
  }
  void tick(double now) {
    io.sent.clear();
    hub.tick(decks, 2, now);
  }
};

Mapping parse(const std::string& text) {
  Mapping m;
  std::string error;
  if (!parse_mapping(text, m, error)) std::fprintf(stderr, "%s\n", error.c_str());
  CHECK(error.empty());
  return m;
}

bool rejects(const std::string& text, const char* needle = nullptr) {
  Mapping m;
  std::string error;
  const bool ok = parse_mapping(text, m, error);
  return !ok && (!needle || error.find(needle) != std::string::npos);
}

const std::string kHead = "name \"T\"\nmatch \"DDJ-FLX4\"\n";

// ---- the format as it was: nothing new is needed to load or run an old mapping --------------------------------
void test_old_format() {
  const Mapping m = parse(kHead +
      "in button 90 1B pad_mode deck=1 mode=hotcue\n"
      "out 90 1B pad_mode deck=1 mode=hotcue\n"
      "in button 90 22 pad_mode deck=1 mode=sampler\n"
      "out 90 22 pad_mode deck=1 mode=sampler\n"
      "in button 90 10 hotcue deck=1 slot=1 mode=hotcue\n"
      "in button 90 10 sampler deck=1 slot=1 mode=sampler\n"
      "in button 90 11 beatjump deck=1 slot=1\n"
      "out 90 10 hotcue deck=1 slot=1 mode=hotcue\n");
  for (const InRule& r : m.in) CHECK(r.alt < 0 && !r.mode_not);
  Rig r(m);
  r.send(0x90, 0x10, 0x7F);  // default mode is hot cue
  CHECK(r.sink.last().op == Op::HotCueDown);
  r.send(0x90, 0x10, 0x00);
  r.send(0x90, 0x22, 0x7F);
  r.send(0x90, 0x22, 0x7F);  // a second press changes nothing without alt=
  r.send(0x90, 0x10, 0x7F);
  CHECK(r.sink.last().op == Op::SamplerTrigger);
  r.send(0x90, 0x10, 0x00);
  r.decks[0].loaded = true;
  r.tick(0.1);
  CHECK(r.io.val(0x90, 0x22) == 0x7F && r.io.val(0x90, 0x1B) == 0x00);
  // every shipped example still parses
  for (const char* f : {"controllers/generic-2deck.dhmap.example", "controllers/shift-layer.dhmap.example"}) {
    Mapping x;
    std::string e;
    CHECK(parse_mapping(slurp(f), x, e));
  }
  // an old file with a mode named like the new one in a comment or name is untouched
  CHECK(!rejects(kHead + "# stems\nin button 90 01 play deck=1\n"));
}

// ---- pad-mode pairs ------------------------------------------------------------------------------------------
void test_pairs() {
  CHECK(rejects(kHead + "in button 90 22 pad_mode deck=1 mode=sampler alt=sampler\n", "alt="));
  CHECK(rejects(kHead + "in button 90 22 pad_mode deck=1 mode=sampler alt=nope\n", "pad mode"));
  CHECK(rejects(kHead + "in button 90 22 sampler deck=1 slot=1 alt=stems\n", "alt="));
  CHECK(rejects(kHead + "in button 90 22 pad_mode deck=1 mode=!sampler\n"));
  CHECK(rejects(kHead + "out 90 22 pad_mode deck=1 mode=sampler alt=sampler\n", "alt="));
  CHECK(rejects(kHead + "in button 90 22 stem_preset deck=1\n", "preset"));
  CHECK(rejects(kHead + "in button 90 22 stem_mute deck=1 stem=guitar\n", "stem"));

  const Mapping m = parse(kHead +
      "in button 90 1B pad_mode deck=1 mode=hotcue\n"
      "in button 90 22 pad_mode deck=1 mode=sampler alt=stems\n"
      "out 90 22 pad_mode deck=1 mode=sampler alt=stems\n"
      "out 90 23 pad_mode deck=1 mode=stems\n"
      "out 90 24 pad_mode deck=1 mode=sampler alt=stems alt_on=20\n"
      "in button 90 30 sampler deck=1 slot=1 mode=!stems\n"
      "in button 90 30 stem_mute deck=1 stem=vocals mode=stems\n"
      "in button 90 31 stem_mute deck=1 slot=2 mode=stems\n"
      "in button 90 32 stem_mute deck=1 slot=7 mode=stems\n"
      "in button 98 30 stem_preset deck=1 preset=acapella mode=stems\n"
      "in button 98 30 sampler_stop deck=1 slot=1 mode=!stems\n");
  Rig r(m);
  const auto press = [&](std::uint8_t a, std::uint8_t b) {
    r.send(a, b, 0x7F);
    r.send(a, b, 0x00);
  };
  r.decks[0].loaded = true;
  // hot cue is the default; pad 30 belongs to nothing in it... but mode=!stems rules work in every mode but stems
  r.sink.commands.clear();
  press(0x90, 0x30);
  CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::SamplerTrigger);
  r.sink.commands.clear();
  press(0x90, 0x22);  // sampler
  r.tick(0.1);
  CHECK(r.io.val(0x90, 0x22) == 0x7F && r.io.val(0x90, 0x24) == 0x7F);
  press(0x90, 0x30);
  CHECK(r.sink.last().op == Op::SamplerTrigger);
  r.sink.commands.clear();
  press(0x90, 0x22);  // the second press: stems
  r.tick(0.3);  // blink off
  CHECK(r.io.val(0x90, 0x22) == 0x00);
  CHECK(r.io.val(0x90, 0x24) == 0x20);  // alt_on: its own level, not blinking
  CHECK(r.io.val(0x90, 0x23) == 0x7F);  // the plain stems-mode LED is lit
  r.tick(0.6);  // blink on
  CHECK(r.io.val(0x90, 0x22) == 0x7F);
  r.sink.commands.clear();
  press(0x90, 0x30);  // now a stem mute, not a sampler trigger
  CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::StemParam && r.sink.last().i == ((0 << 8) | static_cast<int>(StemField::Mute)) &&
        r.sink.last().f < 0);
  press(0x90, 0x31);  // slot=2 -> stem 2 (drums)
  CHECK(r.sink.last().i == ((1 << 8) | static_cast<int>(StemField::Mute)));
  press(0x90, 0x32);  // slot=7 -> stem 7 % 4 = bass (index 2)
  CHECK(r.sink.last().i == ((2 << 8) | static_cast<int>(StemField::Mute)));
  r.sink.commands.clear();
  press(0x98, 0x30);  // SHIFT + pad: the preset, and the sampler's stop is not sent
  CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::StemPreset && r.sink.last().i == static_cast<int>(StemPresetId::Acapella));
  press(0x90, 0x22);  // third press: back to the sampler
  r.sink.commands.clear();
  press(0x90, 0x30);
  CHECK(r.sink.last().op == Op::SamplerTrigger && r.sink.commands.size() == 1);
  press(0x98, 0x30);
  CHECK(r.sink.last().op == Op::SamplerStop);
  // another mode's button leaves the pair; a first press of the pair's button starts at its main mode
  press(0x90, 0x22);  // stems
  press(0x90, 0x1B);  // hot cue
  r.sink.commands.clear();
  press(0x90, 0x30);
  CHECK(r.sink.last().op == Op::SamplerTrigger);  // !stems: any mode but stems
  press(0x90, 0x22);  // from hot cue the pair starts at its main mode
  r.sink.commands.clear();
  press(0x90, 0x30);
  CHECK(r.sink.last().op == Op::SamplerTrigger);  // sampler, not stems
}

// ---- the stems pad mode on the shipped DDJ-FLX4 mapping ---------------------------------------------------------
void test_flx4() {
  const Mapping m = parse(slurp("controllers/ddj-flx4.dhmap"));
  Rig r(m);
  const auto press = [&](std::uint8_t a, std::uint8_t b) {
    r.send(a, b, 0x7F);
    r.send(a, b, 0x00);
  };
  r.decks[0].loaded = r.decks[1].loaded = true;
  r.decks[0].stems_state = 2;
  r.decks[0].stems_on = 1;

  // sampler as before
  press(0x90, 0x22);
  r.sink.commands.clear();
  press(0x97, 0x31);
  CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::SamplerTrigger && r.sink.last().deck == 1);
  r.sink.commands.clear();
  press(0x98, 0x31);
  CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::SamplerStop);
  r.tick(0.1);
  CHECK(r.io.val(0x90, 0x22) == 0x7F);

  // the second press of SAMPLER: STEMS (the button blinks), same pad notes
  press(0x90, 0x22);
  r.tick(0.3);  // the button blinks: off, then on
  CHECK(r.io.val(0x90, 0x22) == 0x00);
  // pad LEDs: pads 1..4 and 5..8 both show the four stems; all audible with stems playing
  for (int i = 0; i < 8; ++i) CHECK(r.io.val(0x97, static_cast<std::uint8_t>(0x30 + i)) == 0x7F);
  r.tick(0.6);
  CHECK(r.io.val(0x90, 0x22) == 0x7F);
  r.sink.commands.clear();
  press(0x97, 0x30);  // pad 1: mute vocals
  CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::StemParam && r.sink.last().deck == 0 &&
        r.sink.last().i == static_cast<int>(StemField::Mute) && r.sink.last().f < 0);
  r.decks[0].stem_mute = 1;  // the engine reports the mute
  r.tick(0.4);
  CHECK(r.io.val(0x97, 0x30) == 0x00 && r.io.val(0x97, 0x31) == 0x7F);  // vocals dark, drums still lit
  r.sink.commands.clear();
  r.send(0x97, 0x33, 0x7F);  // pad 4: mute other
  CHECK(r.sink.last().i == ((3 << 8) | static_cast<int>(StemField::Mute)));
  r.send(0x97, 0x33, 0x00);
  CHECK(r.sink.commands.size() == 1);  // a toggle: release does nothing
  r.sink.commands.clear();
  r.send(0x97, 0x35, 0x7F);  // pad 6: isolate drums while held
  CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::StemParam && r.sink.last().i == ((1 << 8) | static_cast<int>(StemField::Isolate)) &&
        r.sink.last().f == 1.0f);
  r.send(0x97, 0x35, 0x00);
  CHECK(r.sink.commands.size() == 2 && r.sink.last().f == 0.0f);
  // shift + pad 1..5 -> presets
  const StemPresetId want[5] = {StemPresetId::Reset, StemPresetId::Acapella, StemPresetId::Instrumental, StemPresetId::DrumsOnly, StemPresetId::NoDrums};
  for (int i = 0; i < 5; ++i) {
    r.sink.commands.clear();
    press(0x98, static_cast<std::uint8_t>(0x30 + i));
    CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::StemPreset && r.sink.last().i == static_cast<int>(want[i]));
  }
  r.sink.commands.clear();
  press(0x98, 0x35);  // SHIFT + pad 6 has no stem meaning and must not stop a sample
  CHECK(r.sink.commands.empty());
  // lit = audible: solo on drums, vocals / bass / other are not
  r.decks[0].stem_solo = 2;
  r.tick(0.5);
  CHECK(r.io.val(0x97, 0x31) == 0x7F && r.io.val(0x97, 0x30) == 0x00 && r.io.val(0x97, 0x32) == 0x00 && r.io.val(0x97, 0x33) == 0x00);
  r.decks[0].stem_solo = 0;
  r.decks[0].stem_mute = 4;
  r.tick(0.55);
  CHECK(r.io.val(0x97, 0x32) == 0x00 && r.io.val(0x97, 0x30) == 0x7F);
  // no stems on the track: every stem pad is dark
  r.decks[0].stems_state = 0;
  r.decks[0].stem_mute = 0;
  r.tick(0.6);
  for (int i = 0; i < 8; ++i) CHECK(r.io.val(0x97, static_cast<std::uint8_t>(0x30 + i)) == 0x00);
  // stems attached but the deck plays the original: dark too
  r.decks[0].stems_state = 2;
  r.decks[0].stems_on = 0;
  r.tick(0.65);
  CHECK(r.io.val(0x97, 0x30) == 0x00);
  // a stem press while the stems are off turns them on first
  r.sink.commands.clear();
  press(0x97, 0x31);
  CHECK(r.sink.commands.size() == 2 && r.sink.commands[0].op == Op::StemEnable && r.sink.commands[0].i == 1 && r.sink.commands[1].op == Op::StemParam);
  // deck 2 has its own notes and its own mode
  press(0x91, 0x22);
  press(0x91, 0x22);
  r.sink.commands.clear();
  press(0x99, 0x32);
  CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::StemParam && r.sink.last().deck == 1 && (r.sink.last().i >> 8) == 2);
  r.sink.commands.clear();
  press(0x97, 0x36);  // deck 1 is still in stems: isolate bass
  CHECK(r.sink.commands.size() == 2 && r.sink.last().i == ((2 << 8) | static_cast<int>(StemField::Isolate)));
  // back to the sampler on the third press
  press(0x90, 0x22);
  r.sink.commands.clear();
  press(0x97, 0x30);
  CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::SamplerTrigger);
  // the other modes keep their own notes and are untouched
  press(0x90, 0x1B);
  r.sink.commands.clear();
  r.send(0x97, 0x02, 0x7F);
  CHECK(r.sink.last().op == Op::HotCueDown && r.sink.last().i == 2);
  r.send(0x97, 0x02, 0x00);
  press(0x90, 0x6F);  // key shift mode still sets -4..+3
  r.sink.commands.clear();
  press(0x97, 0x77);
  CHECK(r.sink.last().op == Op::KeyShift && r.sink.last().f == 3.0f);
}

// ---- key actions, hot cue slots 9..16, learn choices ------------------------------------------------------------
void test_key_and_cues() {
  const Mapping m = parse(kHead +
      "in button 90 01 key_sync deck=1\n"
      "in button 90 02 key_sync deck=1 value=0\n"
      "in button 90 03 key_match deck=2\n"
      "in button 90 04 key_reset deck=1\n"
      "in button 90 05 key_shift deck=1 dir=1\n"
      "in button 90 06 key_shift deck=1 dir=-1\n"
      "in button 90 07 key_sync deck=1 hold=1\n"
      "out 90 01 key_sync deck=1\n"
      "in button 90 20 hotcue deck=1 slot=9\n"
      "in button 90 21 hotcue deck=1 slot=16\n"
      "in button 90 22 hotcue_clear deck=2 slot=12\n"
      "out 90 20 hotcue deck=1 slot=9\n"
      "out 90 21 hotcue deck=1 slot=16\n");
  CHECK(rejects(kHead + "in button 90 20 hotcue deck=1 slot=17\n", "slot"));
  Rig r(m);
  const auto press = [&](std::uint8_t b) {
    r.send(0x90, b, 0x7F);
    r.send(0x90, b, 0x00);
  };
  press(0x01);
  CHECK(r.sink.last().op == Op::KeyFollow && r.sink.last().i == -1 && r.sink.last().deck == 0);
  press(0x02);
  CHECK(r.sink.last().op == Op::KeyFollow && r.sink.last().i == 0);
  r.sink.commands.clear();
  r.send(0x90, 0x07, 0x7F);
  r.send(0x90, 0x07, 0x00);
  CHECK(r.sink.commands.size() == 2 && r.sink.commands[0].i == 1 && r.sink.commands[1].i == 0);
  press(0x03);
  CHECK(r.sink.last().op == Op::KeyMatch && r.sink.last().deck == 1);
  press(0x05);
  CHECK(r.sink.last().op == Op::KeyShift && r.sink.last().deck == 0);
  r.decks[0].loaded = true;
  r.decks[0].key_sync_follow = 1;
  r.tick(0.1);
  CHECK(r.io.val(0x90, 0x01) == 0x7F);
  r.decks[0].key_sync_follow = 0;
  r.tick(0.2);
  CHECK(r.io.val(0x90, 0x01) == 0x00);
  r.decks[0].key_shift = 2;
  press(0x04);
  CHECK(r.sink.last().op == Op::KeyReset && r.sink.last().deck == 0);

  press(0x20);
  CHECK(r.sink.commands[r.sink.commands.size() - 2].op == Op::HotCueDown && r.sink.commands[r.sink.commands.size() - 2].i == 8);
  press(0x21);
  CHECK(r.sink.commands[r.sink.commands.size() - 2].op == Op::HotCueDown && r.sink.commands[r.sink.commands.size() - 2].i == 15);
  press(0x22);
  CHECK(r.sink.last().op == Op::HotCueClear && r.sink.last().i == 11 && r.sink.last().deck == 1);
  r.decks[0].hot_cue[15] = 10.0;
  r.tick(0.3);
  CHECK(r.io.val(0x90, 0x21) == 0x7F);

  // learn: the key and stems groups, and cues 9..16
  const auto choices = learn_choices(2);
  int key = 0, stems = 0, cues = 0;
  for (const LearnChoice& c : choices) {
    if (c.group == "Key") ++key;
    if (c.group == "Stems") ++stems;
    if (c.label.rfind("Hot cue ", 0) == 0) ++cues;
    Mapping probe;
    std::string why;
    CHECK(parse_mapping("name \"x\"\nmatch \"x\"\nin button 90 00 " + c.action + "\n", probe, why));
  }
  CHECK(key == 10 && stems == 2 * 13 && cues == 2 * kHotCues && kHotCues >= 16);
  ControllerHub learner(r.sink, {});
  CHECK(learner.learn_begin("key_sync deck=1") && learner.learn_begin("key_match deck=1") && learner.learn_begin("key_reset deck=2"));
  CHECK(learner.learn_begin("hotcue slot=16 deck=1") && !learner.learn_begin("hotcue slot=17 deck=1"));
  CHECK(learner.learn_begin("stem_mute stem=vocals deck=1") && learner.learn_begin("stem_preset preset=drums deck=1"));
  CHECK(learner.learn_begin("pad_mode mode=stems deck=1"));
}

}  // namespace

int main() {
  test_old_format();
  test_pairs();
  test_flx4();
  test_key_and_cues();
  std::puts("controller pads ok");
  return 0;
}
