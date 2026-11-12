// Controller mapping tests: parse the shipped mapping and drive it with fake MIDI.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "check.hpp"
#include "deckhand/midi/controller.hpp"

using namespace deckhand;

namespace {

struct FakeIO : MidiIO {
  std::vector<std::string> names{"DDJ-FLX4"};
  std::vector<std::vector<std::uint8_t>> sent;
  std::vector<std::string> devices() override { return names; }
  void send(const std::string&, const std::uint8_t* d, std::size_t n) override { sent.emplace_back(d, d + n); }
  bool has(std::uint8_t a, std::uint8_t b, std::uint8_t c) const {
    for (const auto& m : sent)
      if (m.size() == 3 && m[0] == a && m[1] == b && m[2] == c) return true;
    return false;
  }
};

struct Sink : ControlSink {
  std::vector<Command> commands;
  int browsed = 0, presses = 0, loaded = -1;
  void command(const Command& c) override { commands.push_back(c); }
  void browse(int steps) override { browsed += steps; }
  void browse_press() override { ++presses; }
  void load_selected(int deck) override { loaded = deck; }
  const Command& last() const { return commands.back(); }
};

}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : "controllers/ddj-flx4.dhmap";
  std::ifstream f(path);
  CHECK(f.good());
  std::stringstream text;
  text << f.rdbuf();
  Mapping m;
  std::string error;
  if (!parse_mapping(text.str(), m, error)) std::fprintf(stderr, "%s\n", error.c_str());
  CHECK(error.empty());
  CHECK(m.name == "DDJ-FLX4" && m.master_channel == 0 && m.phones_channel == 2 && m.init.size() == 2);

  Mapping bad;
  CHECK(!parse_mapping("name \"x\"\nmatch \"x\"\nin button 90 ZZ play\n", bad, error));
  CHECK(error.find("line 3") != std::string::npos);

  Sink sink;
  FakeIO io;
  ControllerHub hub(sink, {m});
  hub.set_io(&io);
  CHECK(hub.connected().size() == 1);
  CHECK(io.has(0x90, 0x17, 0x7F) && io.has(0x91, 0x17, 0x7F));  // vinyl mode sent on connect
  std::string match;
  int master = -1, phones = -1;
  CHECK(hub.audio_hint(match, master, phones) && match == "ddj-flx4" && master == 0 && phones == 2);

  const auto send = [&](std::uint8_t a, std::uint8_t b, std::uint8_t c) {
    const std::uint8_t msg[3] = {a, b, c};
    hub.midi("DDJ-FLX4", msg, 3);
  };

  send(0x90, 0x0B, 0x7F);  // PLAY deck 1
  CHECK(sink.last().op == Op::PlayToggle && sink.last().deck == 0);
  const std::size_t n = sink.commands.size();
  send(0x90, 0x0B, 0x00);  // release does nothing
  CHECK(sink.commands.size() == n);

  send(0x91, 0x0C, 0x7F);  // CUE deck 2 press / release
  CHECK(sink.last().op == Op::CueDown && sink.last().deck == 1);
  send(0x91, 0x0C, 0x00);
  CHECK(sink.last().op == Op::CueUp);

  send(0xB0, 0x00, 0x7F);  // tempo deck 1: maximum is the "+" end
  send(0xB0, 0x20, 0x7F);
  CHECK(sink.last().op == Op::Tempo && sink.last().f > 0.999f);
  send(0xB0, 0x00, 0x40);
  send(0xB0, 0x20, 0x00);
  CHECK(std::fabs(sink.last().f) < 0.001f);

  send(0xB6, 0x1F, 0x00);  // crossfader hard left
  send(0xB6, 0x3F, 0x00);
  CHECK(sink.last().op == Op::Crossfader && sink.last().f == 0.0f);
  send(0xB6, 0x18, 0x7F);  // filter knob, deck 2
  CHECK(sink.last().op == Op::Filter && sink.last().deck == 1);
  send(0xB1, 0x0F, 0x00);  // EQ low deck 2
  CHECK(sink.last().op == Op::EqLow && sink.last().deck == 1);

  send(0x90, 0x36, 0x7F);  // jog touch + turn clockwise on the platter
  CHECK(sink.last().op == Op::JogTouch && sink.last().i == 1);
  send(0xB0, 0x22, 0x43);
  CHECK(sink.last().op == Op::JogTurn && sink.last().i == 1 && std::fabs(sink.last().d - 3.0 / 720.0) < 1e-9);
  send(0xB0, 0x21, 0x3E);  // wheel side, counter-clockwise
  CHECK(sink.last().i == 0 && sink.last().d < 0);

  send(0x97, 0x02, 0x7F);  // hot cue pad 3, deck 1
  CHECK(sink.last().op == Op::HotCueDown && sink.last().i == 2 && sink.last().deck == 0);
  send(0x9A, 0x07, 0x7F);  // SHIFT + pad 8, deck 2
  CHECK(sink.last().op == Op::HotCueClear && sink.last().i == 7 && sink.last().deck == 1);
  send(0x99, 0x64, 0x7F);  // beat loop pad 5, deck 2 = 4 beats
  CHECK(sink.last().op == Op::BeatLoop && sink.last().f == 4.0f && sink.last().deck == 1);
  send(0x97, 0x20, 0x7F);  // beat jump pad 1 = back one beat
  CHECK(sink.last().op == Op::BeatJump && sink.last().f == -1.0f);
  send(0x90, 0x4D, 0x7F);  // 4 BEAT / EXIT
  CHECK(sink.last().op == Op::LoopToggle && sink.last().f == 4.0f);
  send(0x80, 0x0C, 0x40);  // a real note-off also releases CUE
  CHECK(sink.last().op == Op::CueUp && sink.last().deck == 0);

  send(0xB6, 0x40, 0x02);  // browse two steps down, then one up
  send(0xB6, 0x40, 0x7F);
  CHECK(sink.browsed == 1);
  send(0x96, 0x47, 0x7F);  // LOAD deck 2
  CHECK(sink.loaded == 1);
  send(0x96, 0x41, 0x7F);
  CHECK(sink.presses == 1);

  // LED feedback follows deck state and only sends changes.
  DeckView decks[kMaxDecks];
  decks[0].loaded = decks[0].playing = true;
  decks[0].hot_cue[1] = 1000;
  decks[0].level = 1.0f;
  decks[1].sync = true;
  io.sent.clear();
  hub.tick(decks, kMaxDecks, 0.0);
  CHECK(io.has(0x90, 0x0B, 0x7F));  // play lit
  CHECK(io.has(0x97, 0x01, 0x7F));  // hot cue 2 lit
  CHECK(io.has(0x97, 0x00, 0x00));  // hot cue 1 dark
  CHECK(io.has(0x91, 0x58, 0x7F));  // sync deck 2
  CHECK(io.has(0x9F, 0x00, 0x7F));  // deck 1 loaded illumination
  CHECK(io.has(0xB0, 0x02, 0x7F));  // level meter full
  CHECK(io.has(0x90, 0x1B, 0x7F));  // hot cue pad mode selected by default
  io.sent.clear();
  hub.tick(decks, kMaxDecks, 0.0);
  CHECK(io.sent.empty());
  decks[0].playing = false;
  hub.tick(decks, kMaxDecks, 0.3);  // paused: blink phase off
  CHECK(io.has(0x90, 0x0B, 0x00));

  // Unplugging removes the controller.
  io.names.clear();
  hub.devices_changed();
  CHECK(hub.connected().empty());

  // ---- shipped and example mappings parse cleanly ----
  for (const char* file : {"controllers/ddj-flx4.dhmap", "controllers/generic-2deck.dhmap.example",
                           "controllers/shift-layer.dhmap.example"}) {
    std::ifstream ef(file);
    CHECK(ef.good());
    std::stringstream et;
    et << ef.rdbuf();
    Mapping em;
    std::string err;
    const bool ok = parse_mapping(et.str(), em, err);
    if (!ok) std::fprintf(stderr, "%s: %s\n", file, err.c_str());
    CHECK(ok);
  }
  Mapping flx;
  CHECK(parse_mapping(text.str(), flx, error));
  {  // beat loop pad LED follows the active loop length
    Sink s2;
    FakeIO i2;
    ControllerHub h2(s2, {flx});
    h2.set_io(&i2);
    DeckView dv[kMaxDecks];
    dv[0].loaded = dv[0].loop_active = true;
    dv[0].bpm = 120;
    dv[0].loop_in = 0;
    dv[0].loop_out = 88200;  // 2 s at 120 BPM = 4 beats
    i2.sent.clear();
    h2.tick(dv, kMaxDecks, 0.0);
    CHECK(i2.has(0x97, 0x64, 0x7F) && i2.has(0x97, 0x63, 0x00));
  }

  // ---- new actions ----
  const auto make = [](const char* body, std::string dev) {
    Mapping mm;
    std::string err;
    const std::string t = "name \"T\"\nmatch \"" + dev + "\"\n" + body;
    if (!parse_mapping(t, mm, err)) std::fprintf(stderr, "%s\n", err.c_str());
    CHECK(err.empty());
    return mm;
  };
  struct Rig {
    Sink sink;
    FakeIO io;
    std::unique_ptr<ControllerHub> hub;
    std::string dev;
    Rig(Mapping mm, std::string d) : dev(d) {
      io.names = {d};
      hub = std::make_unique<ControllerHub>(sink, std::vector<Mapping>{std::move(mm)});
      hub->set_io(&io);
    }
    void send(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
      const std::uint8_t msg[3] = {a, b, c};
      hub->midi(dev, msg, 3);
    }
  };
  {
    Rig r(make(R"(
in button 90 01 key_lock deck=1
in button 90 02 key_shift deck=1 dir=1
in button 90 03 key_shift deck=1 semitones=0
in button 90 04 quantize deck=1
in button 90 05 slip deck=2 hold=1
in button 90 06 reverse deck=1 value=1
in button 90 07 loop_move deck=1 beats=-2
in button 90 08 loop_exit deck=1
in button 90 09 loop_halve deck=1
in button 90 0A loop_double deck=1
in button 90 0B color_fx deck=1 effect=crush
in button 90 0C fx_select unit=master slot=2 dir=1
in button 90 0D fx_on deck=1 slot=3
in button 90 0E fx_time unit=master slot=1 dir=1
in button 90 0F fx_time unit=master slot=1 dir=-1
in abs B0 01 fx_wet unit=master slot=1
in abs B0 02 fx_param deck=1 slot=2
in button 90 10 sampler slot=3
in button 90 11 sampler_stop
in abs B0 03 sampler_level
in button 90 12 master_bpm value=128
in abs B0 04 auto_gain deck=1 lo=-12 hi=6
in rel B0 05 key_shift deck=1
in rel B0 06 fx_select deck=1 slot=1
in button 90 13 sampler_stop slot=2
in abs B0 07 key_shift deck=1 lo=-6 hi=6
out 90 01 key_lock deck=1
out 90 04 quantize deck=1
out 90 05 slip deck=2
out 90 06 reverse deck=1
)", "Act Test"), "Act Test");
    Sink& k = r.sink;
    r.send(0x90, 1, 0x7F);
    CHECK(k.last().op == Op::KeyLock && k.last().i == -1 && k.last().deck == 0);
    r.send(0x90, 2, 0x7F);
    CHECK(k.last().op == Op::KeyShift && k.last().f == 1.0f);
    r.send(0x90, 2, 0x7F);
    CHECK(k.last().f == 2.0f);
    r.send(0x90, 3, 0x7F);
    CHECK(k.last().op == Op::KeyShift && k.last().f == 0.0f);
    r.send(0x90, 4, 0x7F);
    CHECK(k.last().op == Op::Quantize && k.last().i == -1);
    r.send(0x90, 5, 0x7F);
    CHECK(k.last().op == Op::Slip && k.last().i == 1 && k.last().deck == 1);
    r.send(0x90, 5, 0x00);
    CHECK(k.last().op == Op::Slip && k.last().i == 0);
    r.send(0x90, 6, 0x7F);
    CHECK(k.last().op == Op::Reverse && k.last().i == 1);
    r.send(0x90, 7, 0x7F);
    CHECK(k.last().op == Op::LoopMove && k.last().f == -2.0f);
    r.send(0x90, 8, 0x7F);
    CHECK(k.last().op == Op::LoopExit);
    r.send(0x90, 9, 0x7F);
    CHECK(k.last().op == Op::LoopScale && k.last().f == 0.5f);
    r.send(0x90, 10, 0x7F);
    CHECK(k.last().op == Op::LoopScale && k.last().f == 2.0f);
    r.send(0x90, 11, 0x7F);
    CHECK(k.last().op == Op::ColorFx && k.last().i == int(ColorFx::Crush));
    r.send(0x90, 12, 0x7F);
    CHECK(k.last().op == Op::FxSelect && k.last().deck == kFxMaster && k.last().i == 1 &&
          k.last().d == double(EffectKind::Echo));
    r.send(0x90, 13, 0x7F);
    CHECK(k.last().op == Op::FxEnable && k.last().deck == 0 && k.last().i == 2 && k.last().f == 1.0f);
    r.send(0x90, 13, 0x7F);  // toggles off
    CHECK(k.last().f == 0.0f);
    r.send(0x90, 14, 0x7F);  // 1 beat -> 2
    CHECK(k.last().op == Op::FxTime && k.last().deck == kFxMaster && k.last().f == 2.0f);
    r.send(0x90, 15, 0x7F);
    CHECK(k.last().f == 1.0f);
    r.send(0x90, 15, 0x7F);
    CHECK(k.last().f == 0.5f);
    r.send(0xB0, 1, 0x7F);
    CHECK(k.last().op == Op::FxWet && k.last().deck == kFxMaster && k.last().i == 0 && k.last().f == 1.0f);
    r.send(0xB0, 2, 0x00);
    CHECK(k.last().op == Op::FxParam && k.last().deck == 0 && k.last().i == 1 && k.last().f == 0.0f);
    r.send(0x90, 16, 0x7F);
    CHECK(k.last().op == Op::SamplerTrigger && k.last().deck == 2);
    r.send(0x90, 17, 0x7F);
    CHECK(k.last().op == Op::SamplerStop && k.last().i == -1);
    r.send(0x90, 19, 0x7F);
    CHECK(k.last().op == Op::SamplerStop && k.last().i == 0 && k.last().deck == 1);
    r.send(0xB0, 3, 0x7F);
    CHECK(k.last().op == Op::SamplerLevel && k.last().f == 1.0f);
    r.send(0x90, 18, 0x7F);
    CHECK(k.last().op == Op::MasterBpm && k.last().f == 128.0f);
    r.send(0xB0, 4, 0x00);
    CHECK(k.last().op == Op::AutoGain && k.last().f == -12.0f);
    r.send(0xB0, 4, 0x7F);
    CHECK(std::fabs(k.last().f - 6.0f) < 1e-4f);
    r.send(0xB0, 5, 0x41);  // encoder: one semitone up from 0
    CHECK(k.last().op == Op::KeyShift && k.last().f == 1.0f);
    r.send(0xB0, 6, 0x41);
    CHECK(k.last().op == Op::FxSelect && k.last().deck == 0 && k.last().d == double(EffectKind::Echo));
    r.send(0xB0, 6, 0x41);
    CHECK(k.last().d == double(EffectKind::PingPong));
    r.send(0xB0, 7, 0x7F);
    CHECK(k.last().op == Op::KeyShift && k.last().f == 6.0f);

    DeckView dv[kMaxDecks];
    dv[0].key_lock = dv[0].reverse = true;
    dv[1].slip = true;
    r.io.sent.clear();
    r.hub->tick(dv, kMaxDecks, 0.0);
    CHECK(r.io.has(0x90, 1, 0x7F) && r.io.has(0x90, 4, 0x00) && r.io.has(0x90, 5, 0x7F) && r.io.has(0x90, 6, 0x7F));
  }

  // ---- pad modes ----
  {
    Rig r(make(R"(
in button 90 70 pad_mode deck=1 mode=beatloop
in button 90 71 pad_mode deck=1 mode=hotcue
in button 90 72 pad_mode deck=1 mode=sampler
in button 90 73 pad_mode deck=1 mode=keyboard
in button 90 74 pad_mode deck=1 mode=padfx1
in button 90 75 pad_mode deck=1 mode=beatjump
in button 90 76 pad_mode deck=1 mode=keyshift
in button 97 00 hotcue deck=1 slot=1 mode=hotcue
in button 97 00 beatloop deck=1 slot=1 mode=beatloop
in button 97 01 beatloop deck=1 slot=2 mode=beatloop beats=3
in button 97 00 beatjump deck=1 slot=1 mode=beatjump
in button 97 01 beatjump deck=1 slot=2 mode=beatjump
in button 97 02 beatjump deck=1 slot=3 mode=beatjump beats=16
in button 97 00 sampler deck=1 slot=1 mode=sampler offset=8
in button 97 00 keyboard deck=1 slot=3 mode=keyboard
in button 97 00 keyshift deck=1 slot=1 mode=keyshift
in button 97 00 padfx deck=1 pad=1 mode=padfx1 effect=echo beats=0.5 slot=2
out 97 00 loop_beats deck=1 beats=4 mode=beatloop
out 97 00 hotcue deck=1 slot=1 mode=hotcue
out 97 00 key_shift deck=1 slot=3 mode=keyboard
out 97 00 fx_on deck=1 slot=2 effect=echo mode=padfx1
out 97 00 sampler_playing slot=1 offset=8 mode=sampler
out 97 00 loaded deck=1 mode=beatjump
)", "Pad Test"), "Pad Test");
    Sink& k = r.sink;
    std::size_t n = k.commands.size();
    r.send(0x97, 0, 0x7F);  // default mode is hot cue
    CHECK(k.commands.size() == n + 1 && k.last().op == Op::HotCueDown && k.last().i == 0);
    r.send(0x90, 0x70, 0x7F);  // switch to beat loop while the pad is still down
    r.send(0x97, 0, 0x00);     // the release still reaches the hot cue
    CHECK(k.last().op == Op::HotCueUp && k.last().i == 0);
    n = k.commands.size();
    r.send(0x97, 0, 0x7F);
    CHECK(k.commands.size() == n + 1 && k.last().op == Op::BeatLoop && k.last().f == 0.25f);
    r.send(0x97, 1, 0x7F);
    CHECK(k.last().op == Op::BeatLoop && k.last().f == 3.0f);
    r.send(0x90, 0x75, 0x7F);
    r.send(0x97, 0, 0x7F);
    CHECK(k.last().op == Op::BeatJump && k.last().f == -1.0f);
    r.send(0x97, 1, 0x7F);
    CHECK(k.last().op == Op::BeatJump && k.last().f == 1.0f);
    r.send(0x97, 2, 0x7F);
    CHECK(k.last().f == 16.0f);
    r.send(0x90, 0x72, 0x7F);
    r.send(0x97, 0, 0x7F);
    CHECK(k.last().op == Op::SamplerTrigger && k.last().deck == 8);
    r.send(0x90, 0x73, 0x7F);  // keyboard: key shift, then the hot cue pressed last (slot 1)
    n = k.commands.size();
    r.send(0x97, 0, 0x7F);
    CHECK(k.commands.size() == n + 2 && k.commands[n].op == Op::KeyShift && k.commands[n].f == 2.0f);
    CHECK(k.last().op == Op::HotCueDown && k.last().i == 0);
    r.send(0x97, 0, 0x00);
    CHECK(k.last().op == Op::HotCueUp && k.last().i == 0);
    r.send(0x90, 0x76, 0x7F);
    r.send(0x97, 0, 0x7F);
    CHECK(k.last().op == Op::KeyShift && k.last().f == -4.0f);
    r.send(0x90, 0x74, 0x7F);  // pad FX: momentary effect
    n = k.commands.size();
    r.send(0x97, 0, 0x7F);
    CHECK(k.commands.size() == n + 3);
    CHECK(k.commands[n].op == Op::FxSelect && k.commands[n].i == 1 && k.commands[n].d == double(EffectKind::Echo));
    CHECK(k.commands[n + 1].op == Op::FxTime && k.commands[n + 1].f == 0.5f);
    CHECK(k.last().op == Op::FxEnable && k.last().f == 1.0f);
    r.send(0x97, 0, 0x00);
    CHECK(k.last().op == Op::FxEnable && k.last().f == 0.0f);
    n = k.commands.size();
    r.send(0x97, 0, 0x7F);  // the effect is already selected
    CHECK(k.commands.size() == n + 2);

    // LEDs: the pad shows the state of its current mode.
    DeckView dv[kMaxDecks];
    SamplerView sv[kSamplerSlots];
    MixerView mv;
    dv[0].loop_active = true;
    dv[0].bpm = 120;
    dv[0].loop_in = 0;
    dv[0].loop_out = 88200;
    dv[0].hot_cue[0] = 100;
    dv[0].key_shift = 2;
    dv[0].fx[1].on = true;
    dv[0].fx[1].kind = EffectKind::Echo;
    sv[8].playing = true;
    int pad_led = -1;
    const auto lit = [&](int mode_note, std::uint8_t expect) {
      r.send(0x90, static_cast<std::uint8_t>(mode_note), 0x7F);
      r.io.sent.clear();
      r.hub->tick(dv, kMaxDecks, &mv, sv, kSamplerSlots, 0.0);
      for (const auto& m : r.io.sent)  // only changes are sent; keep the last value seen
        if (m.size() == 3 && m[0] == 0x97 && m[1] == 0x00) pad_led = m[2];
      return pad_led == expect;
    };
    CHECK(lit(0x70, 0x7F));   // beat loop: loop of 4 beats active
    dv[0].loop_out = 44100;
    CHECK(lit(0x70, 0x00));   // 2 beats: not this pad
    CHECK(lit(0x71, 0x7F));   // hot cue 1 set
    CHECK(lit(0x73, 0x7F));   // keyboard: key shift 2 = pad 3
    CHECK(lit(0x74, 0x7F));   // pad FX: echo running
    dv[0].fx[1].on = false;
    CHECK(lit(0x74, 0x00));
    CHECK(lit(0x72, 0x7F));   // sampler slot 9 playing
    sv[8].playing = false;
    CHECK(lit(0x72, 0x00));
  }

  // ---- modifiers and when= ----
  {
    Rig r(make(R"(
in button 90 7F modifier name=shift
in button 91 7F modifier name=shift deck=2
in button 90 0C cue deck=1 when=!shift
in button 90 0C seek_start deck=1 when=shift
in button 90 0D cue deck=2 when=!shift
in button 90 20 sampler slot=1
in abs B0 10 filter deck=1 when=!shift pickup=1
in abs B0 10 trim deck=1 when=shift pickup=1
out 90 7F modifier name=shift
)", "Shift Test"), "Shift Test");
    Sink& k = r.sink;
    r.send(0x90, 0x0C, 0x7F);
    CHECK(k.last().op == Op::CueDown);
    r.send(0x90, 0x0C, 0x00);
    CHECK(k.last().op == Op::CueUp);
    r.send(0x90, 0x0C, 0x7F);  // press, then shift, then release: the release still ends the cue
    r.send(0x90, 0x7F, 0x7F);
    r.send(0x90, 0x0C, 0x00);
    CHECK(k.last().op == Op::CueUp);
    std::size_t n = k.commands.size();
    r.send(0x90, 0x0C, 0x7F);
    CHECK(k.commands.size() == n + 1 && k.last().op == Op::Seek);
    r.send(0x90, 0x20, 0x7F);  // sampler + shift = stop
    CHECK(k.last().op == Op::SamplerStop && k.last().deck == 0);
    DeckView dv[kMaxDecks];
    r.io.sent.clear();
    r.hub->tick(dv, kMaxDecks, 0.0);
    CHECK(r.io.has(0x90, 0x7F, 0x7F));  // modifier LED
    r.send(0x90, 0x7F, 0x00);
    r.send(0x90, 0x20, 0x7F);
    CHECK(k.last().op == Op::SamplerTrigger);
    r.send(0x91, 0x7F, 0x7F);  // deck 2's shift does not touch deck 1's rules
    r.send(0x90, 0x0C, 0x7F);
    CHECK(k.last().op == Op::CueDown && k.last().deck == 0);
    r.send(0x90, 0x0D, 0x7F);  // ...but changes deck 2's
    r.send(0x91, 0x7F, 0x00);

    // soft takeover restarts after a layer change
    dv[0].filter = 0.8f;
    dv[0].trim = 0.2f;
    r.hub->tick(dv, kMaxDecks, 0.1);
    n = k.commands.size();
    r.send(0xB0, 0x10, 0x7F);  // first sample far from the engine value: ignored
    CHECK(k.commands.size() == n);
    r.send(0xB0, 0x10, 0x60);  // crosses 0.8: takes over
    CHECK(k.commands.size() == n + 1 && k.last().op == Op::Filter);
    r.send(0xB0, 0x10, 0x00);
    CHECK(k.last().op == Op::Filter && k.last().f == 0.0f);
    r.send(0x90, 0x7F, 0x7F);  // shift layer drives trim (engine 0.2)
    n = k.commands.size();
    r.send(0xB0, 0x10, 0x7F);
    CHECK(k.commands.size() == n);
    r.send(0xB0, 0x10, 0x10);
    CHECK(k.last().op == Op::Trim);
    r.send(0x90, 0x7F, 0x00);  // back to the first layer: waits again
    n = k.commands.size();
    r.send(0xB0, 0x10, 0x7F);
    CHECK(k.commands.size() == n);
  }

  // ---- soft takeover default, dead zone ----
  {
    Rig r(make(R"(
defaults pickup=1
in abs B0 01 filter deck=1
in abs B0 02 trim deck=1 pickup=0
in abs B0 03 filter deck=2 deadzone=0.1 pickup=0
in abs B0 04 tempo deck=1
)", "Pickup Test"), "Pickup Test");
    Sink& k = r.sink;
    r.send(0xB0, 1, 0x00);  // before the first tick the engine value is unknown: takes effect at once
    CHECK(k.last().op == Op::Filter && k.last().f == 0.0f);
    DeckView dv[kMaxDecks];
    dv[0].filter = 0.5f;
    dv[0].tempo_range = 0.06;
    dv[0].tempo = 0.03;  // knob position 0.75
    r.hub->tick(dv, kMaxDecks, 0.0);
    std::size_t n = k.commands.size();
    r.send(0xB0, 1, 0x00);
    r.send(0xB0, 1, 0x10);
    CHECK(k.commands.size() == n);       // no jump
    r.send(0xB0, 1, 0x7F);               // crossed 0.5
    CHECK(k.commands.size() == n + 1 && k.last().f == 1.0f);
    r.send(0xB0, 1, 0x20);
    CHECK(k.commands.size() == n + 2);   // stays engaged
    r.send(0xB0, 2, 0x00);               // pickup=0 on a rule overrides the default
    CHECK(k.last().op == Op::Trim);
    n = k.commands.size();
    r.send(0xB0, 4, 0x00);
    CHECK(k.commands.size() == n);
    r.send(0xB0, 4, 0x60);  // 0.756 passes 0.75 from below? starts at 0, crosses 0.75 here
    CHECK(k.last().op == Op::Tempo && std::fabs(k.last().f - 0.512f) < 0.01f);
    r.send(0xB0, 3, 0x42);  // 0.52 is inside the +-0.1 dead zone around the centre
    CHECK(k.last().op == Op::Filter && k.last().deck == 1 && k.last().f == 0.5f);
    r.send(0xB0, 3, 0x7F);  // the ends stay reachable
    CHECK(std::fabs(k.last().f - 1.0f) < 1e-4f);
    r.send(0xB0, 3, 0x00);
    CHECK(std::fabs(k.last().f) < 1e-4f);
  }

  // ---- MIDI learn on a device without a mapping ----
  {
    Rig r(make("", "Never Matches"), "Mystery Controller");
    ControllerHub& h = *r.hub;
    CHECK(h.connected().empty());  // an auto-created empty mapping is not listed
    Sink& k = r.sink;
    CHECK(!h.learn_begin("frobnicate deck=1") && h.learn_state() == LearnState::Failed);
    CHECK(!h.learn_begin("play deck=9"));

    CHECK(h.learn_begin("play deck=1") && h.learn_state() == LearnState::Waiting);
    r.send(0x80, 0x0B, 0x00);  // a note-off is not a control
    CHECK(h.learn_state() == LearnState::Waiting);
    r.send(0x90, 0x0B, 0x7F);
    CHECK(h.learn_state() == LearnState::Done && h.learn_device() == "Mystery Controller");
    CHECK(h.learn_result() == "in button 90 0B play deck=1");
    CHECK(k.commands.empty());  // captured, not dispatched
    CHECK(h.learn_commit());
    r.send(0x90, 0x0B, 0x7F);   // the learned rule is live
    CHECK(k.last().op == Op::PlayToggle);

    CHECK(h.learn_begin("filter deck=2"));  // 7-bit fader
    for (int v : {0, 20, 45, 70, 100, 127}) r.send(0xB0, 0x07, static_cast<std::uint8_t>(v));
    r.send(0xB0, 0x09, 0x10);               // another control: ignored
    CHECK(h.learn_result(true) == "in abs B0 07 filter deck=2");

    CHECK(h.learn_begin("tempo deck=1"));   // 14-bit fader: MSB 00, LSB 20
    for (int v : {1, 20, 60, 90}) {
      r.send(0xB0, 0x00, static_cast<std::uint8_t>(v));
      r.send(0xB0, 0x20, static_cast<std::uint8_t>(v / 2));
    }
    CHECK(h.learn_result(true) == "in abs14 B0 00 20 tempo deck=1");

    CHECK(h.learn_begin("jog deck=1"));     // relative, 0x40 centred
    for (int v : {0x41, 0x41, 0x3F, 0x42}) r.send(0xB1, 0x22, static_cast<std::uint8_t>(v));
    CHECK(h.learn_result(true) == "in rel B1 22 jog deck=1");

    CHECK(h.learn_begin("browse scale=3")); // relative, 0x01 / 0x7F
    for (int v : {0x01, 0x01, 0x7F, 0x02}) r.send(0xB6, 0x40, static_cast<std::uint8_t>(v));
    CHECK(h.learn_result(true) == "in rel2 B6 40 browse scale=3");

    CHECK(h.learn_begin("sync deck=1"));    // a button that sends control changes
    r.send(0xB0, 0x33, 0x7F);
    CHECK(h.learn_result() == "in button B0 33 sync deck=1");

    CHECK(h.learn_begin("fader deck=1"));   // a note cannot be a fader
    r.send(0x90, 0x01, 0x7F);
    CHECK(h.learn_state() == LearnState::Failed && h.learn_result().empty() && !h.learn_error().empty());
    h.learn_cancel();
    CHECK(h.learn_state() == LearnState::Idle);

    // Saving creates or updates a user mapping; a re-learned action replaces its old line.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dh_test_learn";
    fs::remove_all(dir);
    std::string err;
    CHECK(save_user_mapping("Mystery Controller", {"in button 90 0B play deck=1", "in abs B0 07 filter deck=2"},
                            dir.string(), &err));
    CHECK(save_user_mapping("Mystery Controller", {"in button 90 0C play deck=1", "in abs B0 08 filter deck=2 pickup=1"},
                            dir.string(), &err));
    CHECK(!save_user_mapping("Mystery Controller", {"in button 90 0C bogus"}, dir.string(), &err));
    const std::vector<Mapping> loaded = load_mappings({dir.string()});
    CHECK(loaded.size() == 1 && loaded[0].name == "Mystery Controller" && loaded[0].in.size() == 2);
    CHECK(loaded[0].in[0].data1 == 0x0C && loaded[0].in[1].data1 == 0x08 && loaded[0].in[1].pickup == 1);
    fs::remove_all(dir);
  }

  std::puts("controller ok");
  return 0;
}
