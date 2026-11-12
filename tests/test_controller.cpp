// Controller mapping tests: parse the shipped mapping and drive it with fake MIDI.
#include <cmath>
#include <cstdio>
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
  std::puts("controller ok");
  return 0;
}
