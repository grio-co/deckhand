// Controller options: deck side assignment (fixed 1/2, fixed 3/4, switchable at runtime), jog touch behaviour (vinyl or
// bend-only, finishing a spinning scratch), per-control response (crossfader curve, EQ kill, filter snap, inverts), the
// self-describing text form of DeviceOptions, and that every mapping file in controllers/ loads and is well formed.
// Fake MIDI only; nothing opens a device.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "check.hpp"
#include "deckhand/midi/controller.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {

struct FakeIO : MidiIO {
  std::vector<std::string> names{"FAKE-2DECK"};
  std::vector<std::vector<std::uint8_t>> sent;
  std::vector<std::string> devices() override { return names; }
  void send(const std::string&, const std::uint8_t* d, std::size_t n) override { sent.emplace_back(d, d + n); }
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
  int count(Op op, int deck = -1) const {
    int n = 0;
    for (const Command& c : commands)
      if (c.op == op && (deck < 0 || c.deck == deck)) ++n;
    return n;
  }
  const Command* last(Op op) const {
    for (auto it = commands.rbegin(); it != commands.rend(); ++it)
      if (it->op == op) return &*it;
    return nullptr;
  }
};

const char* kMap =
    "name \"Fake 2-deck\"\n"
    "match \"FAKE-2DECK\"\n"
    "jog ticks_per_rev=720\n"
    "in button 90 0B play deck=1\n"
    "in button 91 0B play deck=2\n"
    "in button 90 0C cue deck=1\n"
    "in button 90 60 hotcue deck=1 slot=1\n"
    "in button 90 36 jog_touch deck=1\n"
    "in button 91 36 jog_touch deck=2\n"
    "in rel B0 22 jog deck=1 surface=top\n"
    "in rel B0 21 jog deck=1 surface=side\n"
    "in button 90 50 deck_switch\n"
    "in button 90 51 modifier name=shift deck=1\n"
    "in abs B0 13 fader deck=1\n"
    "in abs B1 13 fader deck=2\n"
    "in abs B0 07 eq_high deck=1\n"
    "in abs B0 17 filter deck=1\n"
    "in abs B0 00 tempo deck=1\n"
    "in abs B6 1F crossfader\n"
    "out 90 0B playing deck=1\n"
    "out 91 0B playing deck=2\n";

constexpr const char* kName = "Fake 2-deck";

struct Rig {
  Sink sink;
  FakeIO io;
  ControllerHub hub;
  DeckView decks[4];
  Rig() : hub(sink, {parsed()}) { hub.set_io(&io); }
  static Mapping parsed() {
    Mapping m;
    std::string error;
    if (!parse_mapping(kMap, m, error)) std::fprintf(stderr, "%s\n", error.c_str());
    CHECK(error.empty());
    return m;
  }
  void send(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
    const std::uint8_t msg[3] = {a, b, c};
    hub.midi("FAKE-2DECK", msg, 3);
  }
  void tick(double now) {
    io.sent.clear();
    hub.tick(decks, 4, now);
  }
  void options(const DeviceOptions& o) { hub.set_device_options(kName, o); }
};

bool near(double a, double b, double tol = 0.01) { return std::fabs(a - b) <= tol; }

void test_text_form() {
  CHECK(DeviceOptions{}.to_text().empty());
  DeviceOptions o;
  o.jog_scale = 1.5;
  o.pickup = 1;
  o.leds = false;
  o.deck_pair = 2;
  o.jog_vinyl = false;
  o.jog_finish_scratch = true;
  o.crossfader_curve = 1;
  o.eq_kill = true;
  o.filter_snap = true;
  o.invert_fader = o.invert_crossfader = o.invert_tempo = true;
  o.ticks_per_rev = 1440;
  const DeviceOptions back = DeviceOptions::from_text(o.to_text());
  CHECK(back == o && back.deck_pair == 2 && !back.jog_vinyl && back.jog_finish_scratch && back.crossfader_curve == 1);
  CHECK(back.eq_kill && back.filter_snap && back.invert_fader && back.invert_crossfader && back.invert_tempo && !back.leds);
  CHECK(back.jog_scale == 1.5 && back.ticks_per_rev == 1440 && back.pickup == 1);
  // tolerant: empty, older data (only the first keys), unknown keys, junk values, other separators, out of range
  CHECK(DeviceOptions::from_text("") == DeviceOptions{});
  const DeviceOptions old = DeviceOptions::from_text("jog_scale=2 pickup=1 leds=0");
  CHECK(old.jog_scale == 2 && old.pickup == 1 && !old.leds && old.deck_pair == 0 && old.jog_vinyl && !old.eq_kill);
  const DeviceOptions newer = DeviceOptions::from_text("deck_pair=1;future_option=7,jog_vinyl=maybe banana eq_kill=1 =3 x=");
  CHECK(newer.deck_pair == 1 && newer.jog_vinyl && newer.eq_kill);
  const DeviceOptions wild = DeviceOptions::from_text("jog_scale=99 deck_pair=9 crossfader_curve=-4 ticks_per_rev=3 pickup=5");
  CHECK(wild.jog_scale == 8 && wild.deck_pair == 0 && wild.crossfader_curve == 0 && wild.ticks_per_rev == 16 && wild.pickup == 1);
  CHECK(DeviceOptions::from_text("jog_scale=nan").jog_scale == 1.0);
}

void test_deck_assignment() {
  {  // fixed 3/4: both halves drive the other pair
    Rig r;
    DeviceOptions o;
    o.deck_pair = static_cast<int>(DeckPair::Decks34);
    r.options(o);
    r.send(0x90, 0x0B, 0x7F);
    r.send(0x91, 0x0B, 0x7F);
    CHECK(r.sink.commands.size() == 2 && r.sink.commands[0].deck == 2 && r.sink.commands[1].deck == 3);
    r.send(0xB6, 0x1F, 0x40);  // the crossfader is not a deck control
    CHECK(r.sink.last(Op::Crossfader) && r.sink.last(Op::Crossfader)->deck == 0);
    r.decks[2].playing = true;  // LEDs follow decks 3/4
    r.tick(1.0);
    CHECK(r.io.last(0x90, 0x0B) == 0x7F && r.io.last(0x91, 0x0B) == 0);
    // deck_switch does nothing when the device is not switchable
    r.send(0x90, 0x50, 0x7F);
    r.send(0x90, 0x50, 0);
    CHECK(r.hub.deck_layer(kName) == 1 && !r.hub.switch_decks(kName, 0));
  }
  {  // 1/2 with a switch, at runtime
    Rig r;
    r.send(0x90, 0x50, 0x7F);  // not switchable yet (default options): ignored
    r.send(0x90, 0x50, 0);
    CHECK(r.hub.deck_layer(kName) == 0);
    DeviceOptions o;
    o.deck_pair = static_cast<int>(DeckPair::Switchable);
    o.pickup = 1;
    r.options(o);
    CHECK(r.hub.deck_layer(kName) == 0);
    r.decks[0].playing = true;
    r.decks[0].loaded = r.decks[2].loaded = true;
    r.tick(1.0);
    CHECK(r.io.last(0x90, 0x0B) == 0x7F);
    // things held when the switch is pressed: cue, a pad, a platter touch and a modifier all get released on deck 1
    r.send(0x90, 0x0C, 0x7F);
    r.send(0x90, 0x60, 0x7F);
    r.send(0x90, 0x36, 0x7F);
    r.send(0x90, 0x51, 0x7F);
    CHECK(r.sink.count(Op::CueDown, 0) == 1 && r.sink.count(Op::HotCueDown, 0) == 1 && r.sink.count(Op::JogTouch, 0) == 1);
    // soft takeover: engage the deck-1 fader first
    r.decks[0].fader = 0.5f;
    r.tick(1.5);
    r.send(0xB0, 0x13, 64);  // near the engine value: engages
    CHECK(r.sink.count(Op::Fader, 0) == 1);
    r.sink.commands.clear();
    r.send(0x90, 0x50, 0x7F);  // deck switch pressed
    CHECK(r.hub.deck_layer(kName) == 1);
    CHECK(r.sink.count(Op::CueUp, 0) == 1 && r.sink.count(Op::HotCueUp, 0) == 1);
    CHECK(r.sink.count(Op::JogTouch, 0) == 1 && r.sink.last(Op::JogTouch)->i == 0);
    r.send(0x90, 0x50, 0);
    // the old buttons' releases arrive late: nothing more happens, nothing is sent twice
    const std::size_t n = r.sink.commands.size();
    r.send(0x90, 0x0C, 0);
    r.send(0x90, 0x60, 0);
    r.send(0x90, 0x36, 0);
    r.send(0x90, 0x51, 0);
    CHECK(r.sink.commands.size() == n);
    // new presses go to decks 3 and 4
    r.send(0x90, 0x0B, 0x7F);
    r.send(0x91, 0x0B, 0x7F);
    CHECK(r.sink.commands[n].deck == 2 && r.sink.commands[n + 1].deck == 3);
    // soft takeover is armed again: a fader far from deck 3's value is held back until it crosses it
    r.decks[2].fader = 0.2f;
    r.tick(1.9);
    // LEDs refreshed for the new decks: everything is sent again, now showing decks 3 and 4 (not playing)
    CHECK(r.io.last(0x90, 0x0B) == 0 && r.io.last(0x91, 0x0B) == 0);
    r.tick(1.95);
    CHECK(r.io.sent.empty());  // and only changes after that
    const int before = r.sink.count(Op::Fader);
    r.send(0xB0, 0x13, 120);
    CHECK(r.sink.count(Op::Fader) == before);
    r.send(0xB0, 0x13, 20);   // crosses 0.2 from above
    CHECK(r.sink.count(Op::Fader, 2) == 1);
    r.decks[3].playing = true;
    r.tick(4.0);
    CHECK(r.io.last(0x91, 0x0B) == 0x7F);
    // the mapped action toggles back, and the API can switch too
    r.send(0x90, 0x50, 0x7F);
    r.send(0x90, 0x50, 0);
    CHECK(r.hub.deck_layer(kName) == 0);
    r.tick(5.0);
    CHECK(r.io.last(0x90, 0x0B) == 0x7F);  // deck 1 plays: its LED is back on
    CHECK(r.hub.switch_decks(kName, 1) && r.hub.deck_layer(kName) == 1);
    CHECK(r.hub.switch_decks(kName, -1) && r.hub.deck_layer(kName) == 0);
    // changing an unrelated option keeps the layer; changing deck_pair moves it
    r.hub.switch_decks(kName, 1);
    o.leds = true;
    o.jog_scale = 2;
    r.options(o);
    CHECK(r.hub.deck_layer(kName) == 1);
    o.deck_pair = static_cast<int>(DeckPair::Decks12);
    r.options(o);
    CHECK(r.hub.deck_layer(kName) == 0);
  }
  {  // hold=1 style switch: the layer lasts while the button is down
    Mapping m;
    std::string error;
    CHECK(parse_mapping("name \"H\"\nmatch \"FAKE\"\nin button 90 50 deck_switch hold=1\nin button 90 0B play deck=1\n", m, error));
    Sink sink;
    ControllerHub hub(sink, {m});
    FakeIO io;
    io.names = {"FAKE"};
    hub.set_io(&io);
    DeviceOptions o;
    o.deck_pair = 2;
    hub.set_device_options("H", o);
    const std::uint8_t on[3] = {0x90, 0x50, 0x7F}, off[3] = {0x90, 0x50, 0}, play[3] = {0x90, 0x0B, 0x7F};
    hub.midi("FAKE", on, 3);
    hub.midi("FAKE", play, 3);
    hub.midi("FAKE", off, 3);
    hub.midi("FAKE", play, 3);
    CHECK(sink.commands.size() == 2 && sink.commands[0].deck == 2 && sink.commands[1].deck == 0);
  }
  {  // a mapping that addresses deck 3 or 4 itself is never shifted
    Mapping m;
    std::string error;
    CHECK(parse_mapping("name \"Four\"\nmatch \"FAKE\"\nin button 90 0B play deck=1\nin button 90 0C play deck=4\n", m, error));
    Sink sink;
    ControllerHub hub(sink, {m});
    FakeIO io;
    io.names = {"FAKE"};
    hub.set_io(&io);
    DeviceOptions o;
    o.deck_pair = 1;
    hub.set_device_options("Four", o);
    const std::uint8_t a[3] = {0x90, 0x0B, 0x7F}, b[3] = {0x90, 0x0C, 0x7F};
    hub.midi("FAKE", a, 3);
    hub.midi("FAKE", b, 3);
    CHECK(sink.commands[0].deck == 0 && sink.commands[1].deck == 3);
    CHECK(!hub.switch_decks("Four", 1));
  }
}

void test_jog() {
  {  // vinyl (default): touch scratches, top turns scratch, side turns bend
    Rig r;
    r.send(0x90, 0x36, 0x7F);
    r.send(0xB0, 0x22, 0x41);
    r.send(0xB0, 0x21, 0x41);
    r.send(0x90, 0x36, 0);
    CHECK(r.sink.count(Op::JogTouch) == 2 && r.sink.count(Op::JogTurn) == 2);
    CHECK(r.sink.commands[1].i == 1 && r.sink.commands[2].i == 0);
    CHECK(near(r.sink.commands[1].d, 1.0 / 720, 1e-6));
  }
  {  // bend only: touch ignored, every turn bends; calibration still counts
    Rig r;
    DeviceOptions o;
    o.jog_vinyl = false;
    r.options(o);
    r.send(0x90, 0x36, 0x7F);
    r.send(0xB0, 0x22, 0x41);
    r.send(0xB0, 0x22, 0x41);
    r.send(0x90, 0x36, 0);
    CHECK(r.sink.count(Op::JogTouch) == 0 && r.sink.count(Op::JogTurn) == 2);
    CHECK(r.sink.commands[0].i == 0 && r.sink.commands[1].i == 0);
    CHECK(r.hub.last_jog_ticks(kName) == 2);
    // switching to bend-only while a platter is touched lets go of it
    Rig q;
    q.send(0x90, 0x36, 0x7F);
    q.options(o);
    CHECK(q.sink.count(Op::JogTouch) == 2 && q.sink.last(Op::JogTouch)->i == 0);
  }
  {  // default: releasing the platter releases at once
    Rig r;
    r.send(0x90, 0x36, 0x7F);
    r.send(0xB0, 0x22, 0x41);
    r.tick(1.0);
    r.send(0x90, 0x36, 0);
    CHECK(r.sink.count(Op::JogTouch) == 2 && r.sink.last(Op::JogTouch)->i == 0);
  }
  {  // finish the scratch: released while spinning -> held until the platter has been still for a tick
    Rig r;
    DeviceOptions o;
    o.jog_finish_scratch = true;
    r.options(o);
    r.send(0x90, 0x36, 0x7F);
    r.send(0xB0, 0x22, 0x41);
    r.tick(1.0);
    r.send(0xB0, 0x22, 0x41);
    r.send(0x90, 0x36, 0);
    CHECK(r.sink.count(Op::JogTouch) == 1);  // only the press so far
    r.send(0xB0, 0x22, 0x41);  // the scratch goes on
    r.tick(1.1);
    CHECK(r.sink.count(Op::JogTouch) == 1);
    r.tick(1.2);  // still for one tick
    CHECK(r.sink.count(Op::JogTouch) == 2 && r.sink.last(Op::JogTouch)->i == 0);
    r.tick(1.3);
    CHECK(r.sink.count(Op::JogTouch) == 2);
    // released while still: immediate
    r.send(0x90, 0x36, 0x7F);
    r.tick(2.0);
    r.tick(2.1);
    r.send(0x90, 0x36, 0);
    CHECK(r.sink.count(Op::JogTouch) == 4);
    // grabbed again while it was finishing: no second touch, no release
    r.send(0x90, 0x36, 0x7F);
    r.send(0xB0, 0x22, 0x41);
    r.send(0x90, 0x36, 0);
    const int n = r.sink.count(Op::JogTouch);
    r.send(0x90, 0x36, 0x7F);
    CHECK(r.sink.count(Op::JogTouch) == n + 0);
    r.tick(3.0);
    r.tick(3.1);
    CHECK(r.sink.count(Op::JogTouch) == n);  // still held by the user
    r.send(0x90, 0x36, 0);
    CHECK(r.sink.count(Op::JogTouch) == n + 1 && r.sink.last(Op::JogTouch)->i == 0);
    // a platter that never stops is let go after 1.5 s
    r.tick(10.0);
    r.send(0x90, 0x36, 0x7F);
    r.send(0xB0, 0x22, 0x41);
    r.send(0x90, 0x36, 0);
    const int m = r.sink.count(Op::JogTouch);
    for (int i = 1; i <= 20; ++i) {
      r.send(0xB0, 0x22, 0x41);
      r.tick(10.0 + i * 0.1);
    }
    CHECK(r.sink.count(Op::JogTouch) == m + 1);
  }
}

float sent_after(Rig& r, Op op, std::uint8_t status, std::uint8_t d1, std::uint8_t raw) {
  r.sink.commands.clear();
  r.send(status, d1, raw);
  const Command* c = r.sink.last(op);
  CHECK(c != nullptr);
  return c ? c->f : -99.0f;
}

void test_response() {
  {  // as before without options
    Rig r;
    CHECK(near(sent_after(r, Op::Crossfader, 0xB6, 0x1F, 127), 1.0));
    CHECK(near(sent_after(r, Op::Fader, 0xB0, 0x13, 127), 1.0));
    CHECK(near(sent_after(r, Op::EqHigh, 0xB0, 0x07, 3), 3.0 / 127));
  }
  {  // crossfader curves
    Rig r;
    DeviceOptions o;
    o.crossfader_curve = static_cast<int>(CrossfaderCurve::Cut);
    r.options(o);
    CHECK(near(sent_after(r, Op::Crossfader, 0xB6, 0x1F, 127), 1.0) && near(sent_after(r, Op::Crossfader, 0xB6, 0x1F, 0), 0.0));
    CHECK(near(sent_after(r, Op::Crossfader, 0xB6, 0x1F, 114), 1.0, 0.01));  // 90 % of the travel is already full
    CHECK(near(sent_after(r, Op::Crossfader, 0xB6, 0x1F, 64), 0.504, 0.01));
    CHECK(near(sent_after(r, Op::Crossfader, 0xB6, 0x1F, 80), 0.5 + (80 / 127.0 - 0.5) / 0.8, 0.01));
    o.crossfader_curve = static_cast<int>(CrossfaderCurve::Smooth);
    r.options(o);
    CHECK(near(sent_after(r, Op::Crossfader, 0xB6, 0x1F, 127), 1.0) && near(sent_after(r, Op::Crossfader, 0xB6, 0x1F, 0), 0.0));
    const float q = sent_after(r, Op::Crossfader, 0xB6, 0x1F, 32);  // a quarter of the travel
    CHECK(near(q, 0.5 - 0.5 * std::pow(0.4961, 1.5), 0.01));
    CHECK(sent_after(r, Op::Crossfader, 0xB6, 0x1F, 100) < 100 / 127.0 + 0.2);
  }
  {  // EQ kill and filter snap
    Rig r;
    DeviceOptions o;
    o.eq_kill = true;
    o.filter_snap = true;
    r.options(o);
    CHECK(sent_after(r, Op::EqHigh, 0xB0, 0x07, 0) == 0.0f && sent_after(r, Op::EqHigh, 0xB0, 0x07, 3) == 0.0f);
    CHECK(near(sent_after(r, Op::EqHigh, 0xB0, 0x07, 10), (10 / 127.0 - 0.04) / 0.46 * 0.5, 0.005));
    CHECK(near(sent_after(r, Op::EqHigh, 0xB0, 0x07, 64), 0.504, 0.005) && near(sent_after(r, Op::EqHigh, 0xB0, 0x07, 127), 1.0));
    CHECK(near(sent_after(r, Op::Filter, 0xB0, 0x17, 64), 0.5, 0.0001) && near(sent_after(r, Op::Filter, 0xB0, 0x17, 66), 0.5, 0.0001));
    CHECK(near(sent_after(r, Op::Filter, 0xB0, 0x17, 127), 1.0) && near(sent_after(r, Op::Filter, 0xB0, 0x17, 0), 0.0));
    CHECK(sent_after(r, Op::Filter, 0xB0, 0x17, 80) > 0.5f);
  }
  {  // inverts, per class
    Rig r;
    DeviceOptions o;
    o.invert_fader = true;
    r.options(o);
    CHECK(near(sent_after(r, Op::Fader, 0xB0, 0x13, 127), 0.0) && near(sent_after(r, Op::Fader, 0xB1, 0x13, 0), 1.0));
    CHECK(near(sent_after(r, Op::Crossfader, 0xB6, 0x1F, 127), 1.0) && near(sent_after(r, Op::Tempo, 0xB0, 0x00, 127), 1.0));
    o = DeviceOptions{};
    o.invert_crossfader = o.invert_tempo = true;
    r.options(o);
    CHECK(near(sent_after(r, Op::Crossfader, 0xB6, 0x1F, 127), 0.0) && near(sent_after(r, Op::Tempo, 0xB0, 0x00, 127), -1.0));
    CHECK(near(sent_after(r, Op::Fader, 0xB0, 0x13, 127), 1.0));
  }
  {  // shaping happens before soft takeover: the held-back test is in the engine's terms
    Rig r;
    DeviceOptions o;
    o.pickup = 1;
    o.invert_fader = true;
    r.options(o);
    r.decks[0].fader = 0.0f;
    r.tick(1.0);
    r.sink.commands.clear();
    r.send(0xB0, 0x13, 127);  // inverted -> 0, which is where the engine is: engages at once
    CHECK(r.sink.count(Op::Fader) == 1);
  }
}

// ---- every mapping shipped in controllers/ ---------------------------------------------------------------
std::string read(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream t;
  t << f.rdbuf();
  return t.str();
}

void test_shipped_mappings() {
  std::error_code ec;
  fs::path dir = "controllers";
  if (!fs::is_directory(dir, ec)) dir = fs::path("..") / "controllers";
  CHECK(fs::is_directory(dir, ec));
  int files = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const std::string ext = e.path().extension().string();
    const bool stock = ext == ".dhmap", example = e.path().filename().string().find(".dhmap.example") != std::string::npos;
    if (!stock && !example) continue;
    ++files;
    const std::string text = read(e.path());
    Mapping m;
    std::string error;
    const bool ok = parse_mapping(text, m, error);
    if (!ok) std::fprintf(stderr, "%s: %s\n", e.path().string().c_str(), error.c_str());
    CHECK(ok);
    CHECK(!m.name.empty() && (m.hid || !m.match.empty()));
    CHECK(!m.in.empty());
    for (const InRule& r : m.in) {
      if (r.kind >= InKind::HidButton) continue;
      CHECK(r.status >= 0x80 && r.status < 0xF0 && r.data1 < 0x80 && r.deck >= 0 && r.deck < kMaxDecks);
      CHECK(r.slot >= 0);
    }
    for (const OutRule& r : m.out)
      if (!r.hid) CHECK(r.status >= 0x80 && r.status < 0xF0 && r.data1 < 0x80 && r.deck >= 0 && r.deck < kMaxDecks);
    if (!stock) continue;
    // a stock mapping starts with a comment block that says where its numbers come from
    std::string head;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line) && !line.empty() && line[0] == '#';) head += line + "\n";
    CHECK(head.find("MIDI") != std::string::npos);
    const bool third_party = e.path().filename().string() != "ddj-flx4.dhmap";
    if (third_party) {
      CHECK(head.find("Source:") != std::string::npos);
      CHECK(head.find("Untested on hardware") != std::string::npos || head.find("untested on hardware") != std::string::npos);
    }
    // a stock mapping must work with the new options: switching its decks releases whatever its rules hold
    if (m.hid) continue;
    Sink sink;
    ControllerHub hub(sink, {m});
    FakeIO io;
    io.names = {m.match};
    hub.set_io(&io);
    DeviceOptions o;
    o.deck_pair = 2;
    hub.set_device_options(m.name, o);
    for (const InRule& r : m.in) {
      if (r.kind != InKind::Button || r.status >= 0xB0) continue;
      const std::uint8_t on[3] = {r.status, r.data1, 0x7F};
      hub.midi(m.match, on, 3);
    }
    hub.switch_decks(m.name, 1);
    hub.switch_decks(m.name, 0);
  }
  CHECK(files >= 2);
}

}  // namespace


// Pads that play slices of a sampler slot and drive the step sequencer.
void test_sampler_actions() {
  const char* text =
      "name \"Fake pads\"\nmatch \"FAKE-2DECK\"\n"
      "in button 90 70 slice slot=3 value=2\n"
      "in button 90 71 seq_run\n"
      "in button 90 72 seq_record value=1\n"
      "in button 90 73 seq_pattern slot=5\n"
      "in button 90 51 modifier name=shift deck=1\n";
  Mapping m;
  std::string error;
  CHECK(parse_mapping(text, m, error) && error.empty());
  for (const char* bad : {"in button 90 70 slice slot=17\n", "in button 90 70 seq_pattern slot=9\n"}) {
    Mapping x;
    std::string e;
    CHECK(!parse_mapping(std::string("name \"x\"\nmatch \"FAKE\"\n") + bad, x, e) && !e.empty());
  }
  Sink sink;
  FakeIO io;
  ControllerHub hub(sink, {m});
  hub.set_io(&io);
  DeckView decks[4];
  hub.tick(decks, 4, 0.0);
  const auto press = [&](std::uint8_t note, std::uint8_t vel = 127) {
    const std::uint8_t msg[3] = {0x90, note, vel};
    hub.midi("FAKE-2DECK", msg, 3);
  };
  press(0x70);
  const Command* c = sink.last(Op::SamplerSliceTrigger);
  CHECK(c && c->deck == 1 && c->i == 2);
  press(0x71);
  CHECK(sink.last(Op::SeqRun) && sink.last(Op::SeqRun)->i == -1);
  press(0x72);
  CHECK(sink.last(Op::SeqRecord) && sink.last(Op::SeqRecord)->i == 1);
  press(0x73);
  CHECK(sink.last(Op::SeqSelect) && sink.last(Op::SeqSelect)->i == 4 && sink.last(Op::SeqSelect)->d == 0);
  press(0x51);  // shift held: the pattern waits for the next bar, the slice pad cuts the slot
  press(0x73);
  CHECK(sink.last(Op::SeqSelect)->d == 1);
  press(0x70);
  CHECK(sink.last(Op::SamplerSlice) && sink.last(Op::SamplerSlice)->deck == 1 && sink.last(Op::SamplerSlice)->i == 8);
}

int main() {
  test_text_form();
  test_deck_assignment();
  test_jog();
  test_response();
  test_shipped_mappings();
  test_sampler_actions();
  std::puts("test_controller_options OK");
  return 0;
}
