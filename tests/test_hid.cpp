// HID controller tests: a fake HidIO feeds reports to the hub; mapping syntax, report fields, output reports,
// hot-plug, safety refusals, learn classification and a macOS enumeration smoke test that opens nothing.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include "check.hpp"
#include "deckhand/midi/controller.hpp"

using namespace deckhand;

namespace {

struct FakeHid : HidIO {
  std::vector<HidDeviceInfo> present;  // what is plugged in
  std::vector<HidMatch> matches;
  std::set<std::string> opened, closed;
  struct Sent { std::string id; int report; std::vector<std::uint8_t> data; bool feature; };
  std::vector<Sent> sent;
  int match_calls = 0;
  void set_matches(const std::vector<HidMatch>& m) override { matches = m; ++match_calls; }
  std::vector<HidDeviceInfo> devices() override {
    std::vector<HidDeviceInfo> out;
    for (const HidDeviceInfo& d : present)
      for (const HidMatch& m : matches)
        if (hid_matches(m, d)) { out.push_back(d); break; }
    return out;
  }
  bool open(const std::string& id) override { opened.insert(id); return true; }
  void close(const std::string& id) override { closed.insert(id); }
  bool send_output(const std::string& id, int r, const std::uint8_t* d, std::size_t n) override {
    sent.push_back({id, r, std::vector<std::uint8_t>(d, d + n), false});
    return true;
  }
  bool send_feature(const std::string& id, int r, const std::uint8_t* d, std::size_t n) override {
    sent.push_back({id, r, std::vector<std::uint8_t>(d, d + n), true});
    return true;
  }
  bool get_feature(const std::string&, int, std::vector<std::uint8_t>&) override { return false; }
  std::size_t outputs(int report) const {
    std::size_t n = 0;
    for (const Sent& s : sent) n += !s.feature && s.report == report;
    return n;
  }
};

struct Sink : ControlSink {
  std::vector<Command> commands;
  int browsed = 0;
  void command(const Command& c) override { commands.push_back(c); }
  void browse(int steps) override { browsed += steps; }
  void browse_press() override {}
  void load_selected(int) override {}
  const Command& last() const { return commands.back(); }
};

HidDeviceInfo device(const std::string& id, int vid = 0x1234, int pid = 0xABCD, int page = 0xFF00, int usage = 1) {
  HidDeviceInfo d;
  d.id = id;
  d.product = "Test Pad";
  d.vid = vid;
  d.pid = pid;
  d.usage_page = page;
  d.usage = usage;
  return d;
}

const char* kHeader = "name \"Test HID\"\nhid vid=0x1234 pid=0xABCD usage_page=0xFF00\njog ticks_per_rev=100\n";

Mapping parse(const std::string& body) {
  Mapping m;
  std::string error;
  if (!parse_mapping(std::string(kHeader) + body, m, error)) std::fprintf(stderr, "mapping error: %s\n", error.c_str());
  CHECK(error.empty());
  return m;
}

bool fails(const std::string& text, const std::string& needle = "") {
  Mapping m;
  std::string error;
  const bool ok = parse_mapping(text, m, error);
  if (ok) return false;
  if (!needle.empty() && error.find(needle) == std::string::npos) std::fprintf(stderr, "error was: %s\n", error.c_str());
  return needle.empty() || error.find(needle) != std::string::npos;
}

struct Rig {
  Sink sink;
  FakeHid io;
  ControllerHub hub;
  explicit Rig(const Mapping& m) : hub(sink, {m}) {
    io.present.push_back(device("dev0"));
    hub.set_hid_io(&io);
  }
  void send(std::vector<std::uint8_t> bytes, int report = 1, const char* id = "dev0") {
    hub.hid(id, report, bytes.data(), bytes.size());
  }
};

}  // namespace

int main() {
  // ---- helpers and syntax ---------------------------------------------------------------------------
  CHECK(hid_usage_refused(0x01, 0x06) && hid_usage_refused(0x01, 0x02) && hid_usage_refused(0x01, 0x01) &&
        hid_usage_refused(0x01, 0x07) && hid_usage_refused(0x07, 0x00));
  CHECK(!hid_usage_refused(0x01, 0x05) && !hid_usage_refused(0xFF00, 0x01) && !hid_usage_refused(0x0C, 0x01));
  CHECK(fails("name \"x\"\nhid vid=0x1 pid=0x2 usage_page=0x01 usage=0x06\n", "never opened"));
  CHECK(fails("name \"x\"\nhid vid=0x1 pid=0x2 usage_page=1 usage=2\n", "never opened"));
  CHECK(fails("name \"x\"\nhid vid=0x1\n", "vid"));
  CHECK(fails("name \"x\"\nmatch \"x\"\nin hid report=1 byte=0 bit=0 button play\n", "hid"));  // no hid line
  CHECK(fails(std::string(kHeader) + "in hid report=1 byte=0 size=1 bits=0-9 abs fader\n", "bit range"));
  CHECK(fails(std::string(kHeader) + "in hid report=1 byte=0 size=5 abs fader\n", "size"));
  CHECK(fails(std::string(kHeader) + "in hid report=1 byte=0 size=2 min=5 max=5 abs fader\n", "equal"));
  CHECK(fails(std::string(kHeader) + "in hid byte=0 bit=0 button play\n", "report"));
  CHECK(fails(std::string(kHeader) + "in hid report=1 byte=0 bit=0 wobble play\n"));
  CHECK(fails(std::string(kHeader) + "hid_out report=2 size=2\nout hid report=2 byte=2 bit=0 playing\n", "outside"));
  CHECK(fails(std::string(kHeader) + "out hid report=2 byte=0 bit=0 playing on=2\n", "fit"));
  CHECK(fails(std::string(kHeader) + "hid_init feature report=3 ZZ\n", "hex"));
  {
    Mapping m = parse(
        "in hid report=1 byte=2 bit=3 button play deck=1\n"
        "in hid report=1 byte=3 touch jog_touch deck=1\n"
        "in hid report=1 byte=4 size=2 endian=be min=0 max=1000 abs fader deck=1\n"
        "in hid report=1 byte=6 bits=0-11 abs crossfader\n"
        "in hid report=1 byte=8 rel jog deck=1 surface=top ticks_per_rev=360\n"
        "hid_init feature report=3 01 02\nhid_init output report=4 AA\n"
        "out hid report=2 byte=1 bit=0 playing deck=1 on=1 off=0\n"
        "out hid report=5 byte=2 size=2 level deck=1 on=FFFF off=0\n");
    CHECK(m.hid && m.hid_match.vid == 0x1234 && m.hid_match.pid == 0xABCD && m.hid_match.usage_page == 0xFF00);
    CHECK(m.in.size() == 5 && m.in[0].kind == InKind::HidButton && m.in[1].kind == InKind::HidButton);
    CHECK(m.in[2].hid.size == 2 && m.in[2].hid.big && m.in[2].hid.has_max && m.in[2].hid.vmax == 1000);
    CHECK(m.in[3].hid.lo == 0 && m.in[3].hid.hi == 11 && m.in[3].hid.size == 2);
    CHECK(m.in[4].kind == InKind::HidRel && m.in[4].ticks_per_rev == 360);
    CHECK(m.hid_init.size() == 2 && m.hid_init[0].feature && m.hid_init[1].data.size() == 1);
    CHECK(m.hid_out.size() == 2);  // templates made from the rules
    CHECK(m.index.empty());
  }
  {  // the documented example parses (the shipped mappings dir is the working directory under ctest)
    std::ifstream f("controllers/generic-hid.dhmap.example");
    CHECK(f.good());
    std::stringstream text;
    text << f.rdbuf();
    Mapping m;
    std::string error;
    if (!parse_mapping(text.str(), m, error)) std::fprintf(stderr, "example: %s\n", error.c_str());
    CHECK(error.empty() && m.hid && m.hid_init.size() == 1 && m.hid_out.size() == 1 && m.hid_out[0].size == 8);
    CHECK(m.in.size() >= 15 && m.out.size() == 8);
  }
  CHECK(fails(std::string(kHeader) + "in hid report=1 byte=0 bit=0 button mode_x\n", "unknown action"));

  // ---- bit buttons: baseline, edges, no repeats ---------------------------------------------------
  {
    Rig r(parse("in hid report=1 byte=2 bit=3 button play deck=1\nin hid report=1 byte=2 bit=4 button cue deck=2\n"));
    CHECK(r.io.opened.count("dev0") && r.hub.connected().size() == 1);
    CHECK(r.io.matches.size() == 1 && r.io.matches[0].vid == 0x1234);
    r.send({0, 0, 0x08, 0});  // first report: baseline only, even with a bit already set
    CHECK(r.sink.commands.empty());
    r.send({0, 0, 0x00, 0});  // release of the held bit: nothing (it never counted as a press)
    CHECK(r.sink.commands.empty());
    r.send({0, 0, 0x08, 0});
    CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::PlayToggle && r.sink.last().deck == 0);
    r.send({9, 9, 0x08, 0});  // other bytes change, the held bit is unchanged
    CHECK(r.sink.commands.size() == 1);
    r.send({0, 0, 0x18, 0});  // second button
    CHECK(r.sink.commands.size() == 2 && r.sink.last().op == Op::CueDown && r.sink.last().deck == 1);
    r.send({0, 0, 0x08, 0});
    CHECK(r.sink.last().op == Op::CueUp);
    r.send({0, 0, 0x00, 0});
    CHECK(r.sink.commands.size() == 3);
    r.send({0, 0, 0x08, 0}, 7);  // another report id: not mapped
    CHECK(r.sink.commands.size() == 3);
  }
  {  // inverted (active-low) button
    Rig r(parse("in hid report=1 byte=0 bit=0 button play deck=1 invert=1\n"));
    r.send({1});
    r.send({0});
    CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::PlayToggle);
  }

  // ---- absolute values ----------------------------------------------------------------------------
  {
    Rig r(parse(
        "in hid report=1 byte=0 size=2 endian=le min=0 max=1000 abs fader deck=1\n"
        "in hid report=1 byte=2 size=2 endian=be abs trim deck=1\n"
        "in hid report=1 byte=4 bits=0-11 abs crossfader\n"
        "in hid report=1 byte=6 signed=1 abs eq_low deck=1 min=-100 max=100\n"
        "in hid report=1 byte=7 bits=4-7 abs eq_high deck=1\n"
        "in hid report=1 byte=8 abs filter deck=1 invert=1\n"));
    r.send({0, 0, 0, 0, 0, 0, 0, 0, 0});  // baseline
    const auto last_f = [&] { return r.sink.last().f; };
    r.send({0xF4, 0x01, 0, 0, 0, 0, 0, 0, 0});  // 500 little endian of 0..1000
    CHECK(r.sink.last().op == Op::Fader && std::fabs(last_f() - 0.5f) < 1e-4f);
    r.send({0xE8, 0x03, 0, 0, 0, 0, 0, 0, 0});  // 1000
    CHECK(std::fabs(last_f() - 1.0f) < 1e-4f);
    r.send({0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0});  // 65535: clamped
    CHECK(last_f() == 1.0f);
    r.send({0, 0, 0x80, 0x00, 0, 0, 0, 0, 0});  // big endian 0x8000 of 0..65535
    CHECK(r.sink.last().op == Op::Trim && std::fabs(last_f() - 32768.0f / 65535.0f) < 1e-4f);
    r.send({0, 0, 0, 0, 0xFF, 0x0F, 0, 0, 0});  // 12 bits across two bytes: 0xFFF; the top nibble is not part of it
    CHECK(r.sink.last().op == Op::Crossfader && std::fabs(last_f() - 1.0f) < 1e-4f);
    r.send({0, 0, 0, 0, 0xFF, 0xF0, 0, 0, 0});  // upper nibble of byte 5 set: still 0x0FF = 255/4095
    CHECK(std::fabs(last_f() - 255.0f / 4095.0f) < 1e-4f);
    r.send({0, 0, 0, 0, 0, 0, 0xCE, 0, 0});  // signed -50 of -100..100
    CHECK(r.sink.last().op == Op::EqLow && std::fabs(last_f() - 0.25f) < 1e-4f);
    r.send({0, 0, 0, 0, 0, 0, 0, 0xA0, 0});  // bits 4..7 = 10 of 0..15
    CHECK(r.sink.last().op == Op::EqHigh && std::fabs(last_f() - 10.0f / 15.0f) < 1e-4f);
    r.send({0, 0, 0, 0, 0, 0, 0, 0xA0, 0xFF});  // inverted 8-bit
    CHECK(r.sink.last().op == Op::Filter && std::fabs(last_f() - 0.0f) < 1e-4f);
    const std::size_t n = r.sink.commands.size();
    r.send({0, 0, 0, 0, 0, 0, 0, 0xA0, 0xFF});  // nothing changed
    CHECK(r.sink.commands.size() == n);
  }
  {  // default signed range
    Rig r(parse("in hid report=1 byte=0 signed=1 abs trim deck=1\n"));
    r.send({0});
    r.send({0x80});  // -128
    CHECK(r.sink.last().f == 0.0f);
    r.send({0x7F});
    CHECK(r.sink.last().f == 1.0f);
  }

  // ---- wrapping relative counters ------------------------------------------------------------------
  {
    Rig r(parse("in hid report=1 byte=0 rel jog deck=1 surface=top\nin hid report=1 byte=1 size=2 endian=le rel browse scale=2\n"
                "in hid report=1 byte=3 delta jog deck=2 invert=1 signed=1\n"));
    r.send({250, 0xFE, 0xFF, 0});  // baseline
    r.send({3, 0xFE, 0xFF, 0});    // 250 -> 3 wraps forward: +9
    CHECK(r.sink.last().op == Op::JogTurn && r.sink.last().i == 1 && std::fabs(r.sink.last().d - 9.0 / 100.0) < 1e-9);
    r.send({250, 0xFE, 0xFF, 0});  // 3 -> 250 wraps backward: -9
    CHECK(std::fabs(r.sink.last().d + 9.0 / 100.0) < 1e-9);
    r.send({255, 0xFE, 0xFF, 0});
    CHECK(std::fabs(r.sink.last().d - 5.0 / 100.0) < 1e-9);
    r.send({0, 0xFE, 0xFF, 0});  // exactly at the wrap point
    CHECK(std::fabs(r.sink.last().d - 1.0 / 100.0) < 1e-9);
    const std::size_t n = r.sink.commands.size();
    r.send({0, 0xFE, 0xFF, 0});  // unchanged: no movement
    CHECK(r.sink.commands.size() == n);
    r.send({0, 0x02, 0x00, 0});  // 16-bit 0xFFFE -> 0x0002: +4, browse scale 2 -> 8
    CHECK(r.sink.browsed == 8);
    r.send({0, 0xFE, 0xFF, 0});  // and back: -4
    CHECK(r.sink.browsed == 0);
    r.send({0, 0xFE, 0xFF, 3});  // signed delta field, inverted: +3 becomes -3 on deck 2
    CHECK(r.sink.last().op == Op::JogTurn && r.sink.last().deck == 1 && std::fabs(r.sink.last().d + 3.0 / 100.0) < 1e-9);
    r.send({0, 0xFE, 0xFF, 3});  // a delta repeats while it stays non-zero
    CHECK(std::fabs(r.sink.last().d + 3.0 / 100.0) < 1e-9);
    const std::size_t m = r.sink.commands.size();
    r.send({0, 0xFE, 0xFF, 0});
    CHECK(r.sink.commands.size() == m);
  }

  // ---- touch, modifiers, pad modes -------------------------------------------------------------------
  {
    Rig r(parse(
        "in hid report=1 byte=0 bit=0 touch jog_touch deck=1\n"
        "in hid report=1 byte=0 bit=1 button modifier name=shift\n"
        "in hid report=1 byte=0 bit=2 button play deck=1 when=!shift\n"
        "in hid report=1 byte=0 bit=2 button sync deck=1 when=shift\n"
        "in hid report=1 byte=0 bit=3 button pad_mode mode=beatloop deck=1\n"
        "in hid report=1 byte=0 bit=4 button hotcue slot=1 deck=1 mode=hotcue\n"
        "in hid report=1 byte=0 bit=4 button beatloop slot=2 deck=1 mode=beatloop\n"));
    r.send({0});
    r.send({0x01});
    CHECK(r.sink.last().op == Op::JogTouch && r.sink.last().i == 1);
    r.send({0x00});
    CHECK(r.sink.last().op == Op::JogTouch && r.sink.last().i == 0);
    r.sink.commands.clear();
    r.send({0x04});  // play (shift not held)
    CHECK(r.sink.last().op == Op::PlayToggle);
    r.send({0x00});
    r.send({0x02});  // shift down (a modifier is not a command)
    const std::size_t n = r.sink.commands.size();
    r.send({0x06});  // bit 2 while shift is held
    CHECK(r.sink.commands.size() == n + 1 && r.sink.last().op == Op::SyncToggle);
    r.send({0x00});
    r.sink.commands.clear();
    r.send({0x10});  // pad in hotcue mode
    CHECK(r.sink.last().op == Op::HotCueDown && r.sink.last().i == 0);
    r.send({0x00});
    CHECK(r.sink.last().op == Op::HotCueUp);
    r.send({0x08});  // select the beat loop mode
    r.send({0x00});
    r.sink.commands.clear();
    r.send({0x10});
    CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::BeatLoop);
  }

  // ---- soft takeover ------------------------------------------------------------------------------
  {
    Rig r(parse("defaults pickup=1\nin hid report=1 byte=0 abs fader deck=1\n"));
    DeckView dv;
    dv.fader = 0.9f;
    r.hub.tick(&dv, 1, 0.0);
    r.send({0});  // baseline
    r.send({26});  // ~0.10: far from 0.9, ignored
    r.send({51});  // 0.2
    CHECK(r.sink.commands.empty());
    r.send({204});  // 0.8: not yet within 2 %, but the value was passed? not crossed either
    CHECK(r.sink.commands.empty());
    r.send({240});  // 0.94: crossed 0.9
    CHECK(r.sink.commands.size() == 1 && r.sink.last().op == Op::Fader);
    r.send({230});
    CHECK(r.sink.commands.size() == 2);  // engaged now
  }

  // ---- output reports -----------------------------------------------------------------------------
  {
    Rig r(parse(
        "hid_out report=2 size=5 init=\"80 00 00 00 7E\" interval=20\n"
        "out hid report=2 byte=1 bit=0 playing deck=1 on=1 off=0\n"
        "out hid report=2 byte=1 bit=1 cue deck=1 on=1 off=0\n"
        "out hid report=2 byte=2 size=2 endian=be level deck=1 on=FFFF off=1000\n"));
    DeckView dv;
    dv.loaded = true;
    r.hub.tick(&dv, 1, 1.0);
    CHECK(r.io.outputs(2) == 1);
    const FakeHid::Sent& first = r.io.sent.back();
    CHECK(first.id == "dev0" && first.data.size() == 5 && first.data[0] == 0x80 && first.data[4] == 0x7E);  // template kept
    CHECK((first.data[1] & 1) == 0 && first.data[2] == 0 && first.data[3] == 0);
    r.hub.tick(&dv, 1, 1.001);  // nothing changed: nothing sent
    CHECK(r.io.outputs(2) == 1);
    dv.playing = true;
    r.hub.tick(&dv, 1, 1.005);  // changed but inside the 20 ms interval
    CHECK(r.io.outputs(2) == 1);
    r.hub.tick(&dv, 1, 1.030);  // ... goes out on a later tick
    CHECK(r.io.outputs(2) == 2 && (r.io.sent.back().data[1] & 1) == 1 && r.io.sent.back().data[0] == 0x80);
    dv.level = 1.0f;
    r.hub.tick(&dv, 1, 2.0);
    CHECK(r.io.outputs(2) == 3 && r.io.sent.back().data[2] == 0xFF && r.io.sent.back().data[3] == 0xFF);
    r.hub.tick(&dv, 1, 2.1);
    CHECK(r.io.outputs(2) == 3);
  }
  {  // a rule in another pad mode is dark unless an active rule claims the field
    Rig r(parse(
        "in hid report=1 byte=0 bit=0 button pad_mode mode=beatloop deck=1\n"
        "out hid report=2 byte=0 bit=0 hotcue deck=1 slot=1 mode=hotcue on=1 off=0\n"
        "out hid report=2 byte=0 bit=0 loop deck=1 mode=beatloop on=1 off=0\n"));
    DeckView dv;
    dv.hot_cue[0] = 1000;
    dv.loop_active = false;
    r.hub.tick(&dv, 1, 1.0);
    CHECK(r.io.sent.back().data[0] == 1);  // hotcue mode: the hot cue LED
    r.send({0});
    r.send({1});  // beat loop mode
    r.send({0});
    r.hub.tick(&dv, 1, 2.0);
    CHECK(r.io.sent.back().data[0] == 0);
  }

  // ---- hid_init at connect, hot-plug ---------------------------------------------------------------
  {
    Rig r(parse("hid_init feature report=3 01 02\nhid_init output report=4 AA\nin hid report=1 byte=0 bit=0 button play\n"));
    CHECK(r.io.sent.size() == 2 && r.io.sent[0].feature && r.io.sent[0].report == 3 && r.io.sent[0].data == std::vector<std::uint8_t>({1, 2}));
    CHECK(!r.io.sent[1].feature && r.io.sent[1].report == 4 && r.io.sent[1].data[0] == 0xAA);
    r.send({0});
    r.send({1});
    CHECK(r.sink.commands.size() == 1);
    // unplug in the middle of a stream: reports after it are ignored, the device is closed
    r.io.present.clear();
    r.hub.devices_changed();
    CHECK(r.hub.connected().empty() && r.io.closed.count("dev0"));
    r.send({0});
    r.send({1});
    CHECK(r.sink.commands.size() == 1);
    // plug a different unit (same ids, new path): connects again, inits again, baseline starts fresh
    r.io.present.push_back(device("dev1"));
    r.hub.devices_changed();
    CHECK(r.hub.connected().size() == 1 && r.io.sent.size() == 4);
    r.send({1}, 1, "dev1");  // first report after connecting: baseline
    CHECK(r.sink.commands.size() == 1);
    r.send({0}, 1, "dev1");
    r.send({1}, 1, "dev1");
    CHECK(r.sink.commands.size() == 2);
    // two identical units at once
    r.io.present.push_back(device("dev2"));
    r.hub.devices_changed();
    CHECK(r.hub.connected().size() == 2);
    // a different product is not matched
    r.io.present.push_back(device("dev3", 0x1234, 0x0001));
    r.hub.devices_changed();
    CHECK(r.hub.connected().size() == 2 && !r.io.opened.count("dev3"));
  }

  // ---- safety: keyboards and mice are never opened, whatever a mapping says -------------------------
  {
    Mapping m = parse("in hid report=1 byte=0 bit=0 button play\n");
    m.hid_match.usage_page = -1;  // a mapping that would match every interface of the vid/pid
    Rig r(m);
    r.io.present.push_back(device("kbd", 0x1234, 0xABCD, 0x01, 0x06));
    r.io.present.push_back(device("mouse", 0x1234, 0xABCD, 0x01, 0x02));
    r.io.present.push_back(device("ptr", 0x1234, 0xABCD, 0x01, 0x01));
    r.io.present.push_back(device("keys", 0x1234, 0xABCD, 0x07, 0x00));
    r.io.present.push_back(device("pad", 0x1234, 0xABCD, 0x01, 0x05));
    r.hub.devices_changed();
    CHECK(r.io.opened.count("dev0") && r.io.opened.count("pad"));
    CHECK(!r.io.opened.count("kbd") && !r.io.opened.count("mouse") && !r.io.opened.count("ptr") && !r.io.opened.count("keys"));
    CHECK(r.hub.connected().size() == 2);
    // and with no mapping at all, and HID learn off: nothing is even listed
    Sink s2;
    FakeHid io2;
    io2.present.push_back(device("pad", 0x1234, 0xABCD, 0x01, 0x05));
    io2.present.push_back(device("kbd", 0x1234, 0xABCD, 0x01, 0x06));
    ControllerHub h2(s2, {});
    h2.set_hid_io(&io2);
    CHECK(io2.matches.empty() && io2.opened.empty() && h2.connected().empty());
    h2.hid_learn_gamepads(true);  // game pads only; the keyboard stays out
    CHECK(io2.opened.count("pad") && !io2.opened.count("kbd"));
  }

  // ---- malformed and short reports, fuzz -----------------------------------------------------------
  {
    Rig r(parse(
        "in hid report=1 byte=0 bit=0 button play deck=1\n"
        "in hid report=1 byte=3 size=4 endian=le abs fader deck=1\n"
        "in hid report=1 byte=7 size=3 endian=be signed=1 rel jog deck=1\n"
        "in hid report=1 byte=20 bits=3-10 abs crossfader\n"
        "in hid report=1 byte=40 size=2 signed=1 delta jog deck=2\n"
        "in hid report=0 byte=0 bit=1 button sync deck=1\n"
        "hid_out report=2 size=8\nout hid report=2 byte=4 size=2 level deck=1 on=FFFF off=0\n"));
    r.send({});
    r.hub.hid("dev0", 1, nullptr, 0);
    r.hub.hid("nope", 1, nullptr, 0);
    r.send({1});  // too short for most rules: the 1-byte button still works
    r.send({0});
    r.send({1});
    CHECK(r.sink.commands.size() == 1);
    std::mt19937 rng(1234);
    DeckView dv;
    for (int i = 0; i < 20000; ++i) {
      std::vector<std::uint8_t> bytes(rng() % 80);
      for (auto& b : bytes) b = static_cast<std::uint8_t>(rng());
      r.hub.hid("dev0", static_cast<int>(rng() % 3), bytes.data(), bytes.size());
      if (i % 500 == 0) r.hub.tick(&dv, 1, i * 0.1);
    }
    CHECK(true);
  }

  // ---- learn ---------------------------------------------------------------------------------------
  const auto learn_rig = [](Sink& sink, FakeHid& io, ControllerHub& hub) {
    io.present.push_back(device("lrn", 0x1234, 0x9999));
    hub.set_hid_io(&io);
    hub.hid_learn_allow(0x1234, 0x9999);
    CHECK(io.opened.count("lrn") && hub.connected().empty());  // learn-only device is not listed as connected
    (void)sink;
  };
  const auto feed = [](ControllerHub& hub, std::vector<std::uint8_t> bytes) { hub.hid("lrn", 1, bytes.data(), bytes.size()); };
  {  // bit button
    Sink sink;
    FakeHid io;
    ControllerHub hub(sink, {});
    learn_rig(sink, io, hub);
    CHECK(hub.learn_begin("play deck=1"));
    feed(hub, {0, 0, 0, 0});
    CHECK(hub.learn_state() == LearnState::Waiting);
    feed(hub, {0, 0, 0, 0x20});
    CHECK(hub.learn_state() == LearnState::Done);
    CHECK(hub.learn_result() == "in hid report=1 byte=3 bit=5 button play deck=1");
    CHECK(hub.learn_device() == "lrn" && hub.learn_header() == "hid vid=0x1234 pid=0x9999 usage_page=0xFF00 usage=0x1");
    CHECK(sink.commands.empty());  // the learned press is not dispatched
    const std::string header = hub.learn_header();  // read before commit (it resets the learn state)
    CHECK(hub.learn_commit());
    feed(hub, {0, 0, 0, 0x00});
    feed(hub, {0, 0, 0, 0x20});
    CHECK(sink.commands.size() == 1 && sink.last().op == Op::PlayToggle);
    // saved with a header, then parsed back by the loader
    const std::string dir = (std::filesystem::temp_directory_path() / "dh_hid_learn").string();
    std::filesystem::remove_all(dir);
    CHECK(save_user_mapping("Test Pad", {"in hid report=1 byte=3 bit=5 button play deck=1"}, dir, nullptr, header));
    CHECK(save_user_mapping("Test Pad", {"in hid report=1 byte=3 bit=5 button cue deck=1"}, dir, nullptr, header));
    const std::vector<Mapping> loaded = load_mappings({dir});
    CHECK(loaded.size() == 1 && loaded[0].hid && loaded[0].hid_match.pid == 0x9999 && loaded[0].in.size() == 1 &&
          loaded[0].in[0].action == Action::Cue);  // same control: replaced
    std::filesystem::remove_all(dir);
  }
  {  // active-low button; a multi-control change is ignored until a clear press
    Sink sink;
    FakeHid io;
    ControllerHub hub(sink, {});
    learn_rig(sink, io, hub);
    CHECK(hub.learn_begin("cue deck=1"));
    feed(hub, {0xFF, 0xFF});
    feed(hub, {0xFE, 0xFE});  // two controls at once: ignore
    CHECK(hub.learn_state() == LearnState::Waiting);
    feed(hub, {0xFE, 0xEE});
    CHECK(hub.learn_result() == "in hid report=1 byte=1 bit=4 button cue deck=1 invert=1");
  }
  {  // fader: one byte, two bytes little endian, two bytes big endian, with a noisy counter byte elsewhere
    for (int variant = 0; variant < 3; ++variant) {
      Sink sink;
      FakeHid io;
      ControllerHub hub(sink, {});
      learn_rig(sink, io, hub);
      std::uint8_t counter = 0;
      for (int i = 0; i < 100; ++i) feed(hub, {0, 0, 0, 0, 0, 0, counter++});  // a byte that always changes
      CHECK(hub.learn_begin("fader deck=1"));
      const int values[] = {0, 40, 90, 150, 200, 255};
      const int wide[] = {0x00C0, 0x0120, 0x0180, 0x01E0, 0x0240, 0x02A0, 0x0300, 0x0360, 0x03C0};
      if (variant == 0) {
        for (int v : values) feed(hub, {0, 0, 0, static_cast<std::uint8_t>(v), 0, 0, counter++});
        CHECK(hub.learn_result(true) == "in hid report=1 byte=3 abs fader deck=1");
      } else {
        for (int v : wide) {
          const std::uint8_t lo = v & 0xFF, hi = static_cast<std::uint8_t>(v >> 8);
          if (variant == 1) feed(hub, {0, 0, 0, lo, hi, 0, counter++});
          else feed(hub, {0, 0, 0, hi, lo, 0, counter++});
        }
        CHECK(hub.learn_result(true) == (variant == 1 ? "in hid report=1 byte=3 size=2 endian=le abs fader deck=1"
                                                      : "in hid report=1 byte=3 size=2 endian=be abs fader deck=1"));
      }
      CHECK(hub.learn_state() == LearnState::Done);
    }
  }
  {  // wrapping jog counter, signed delta jog, button misuse
    Sink sink;
    FakeHid io;
    ControllerHub hub(sink, {});
    learn_rig(sink, io, hub);
    CHECK(hub.learn_begin("jog deck=1"));
    feed(hub, {0, 250});
    for (int v : {252, 254, 0, 2, 4, 6}) feed(hub, {0, static_cast<std::uint8_t>(v)});
    CHECK(hub.learn_result(true) == "in hid report=1 byte=1 rel jog deck=1");
    CHECK(hub.learn_commit());
    feed(hub, {0, 10});  // the new rule works at once: +4 from the last seen 6
    CHECK(sink.last().op == Op::JogTurn && std::fabs(sink.last().d - 4.0 / 720.0) < 1e-9);

    Sink sink2;
    FakeHid io2;
    ControllerHub hub2(sink2, {});
    learn_rig(sink2, io2, hub2);
    CHECK(hub2.learn_begin("jog deck=1"));
    feed(hub2, {0, 0});
    for (int v : {1, 0, 2, 0, 255, 0}) feed(hub2, {0, static_cast<std::uint8_t>(v)});
    CHECK(hub2.learn_result(true) == "in hid report=1 byte=1 signed=1 delta jog deck=1");

    Sink sink3;
    FakeHid io3;
    ControllerHub hub3(sink3, {});
    learn_rig(sink3, io3, hub3);
    CHECK(hub3.learn_begin("filter deck=1"));  // a fader action given a button
    feed(hub3, {0, 0});
    feed(hub3, {0, 1});
    feed(hub3, {0, 0});
    feed(hub3, {0, 1});
    CHECK(hub3.learn_result(true).empty() && hub3.learn_state() == LearnState::Failed &&
          hub3.learn_error().find("fader or knob") != std::string::npos);

    Sink sink4;
    FakeHid io4;
    ControllerHub hub4(sink4, {});
    learn_rig(sink4, io4, hub4);
    CHECK(hub4.learn_begin("sync deck=1"));
    hub4.learn_cancel();
    CHECK(hub4.learn_state() == LearnState::Idle);
    CHECK(!hub4.learn_begin("not_an_action"));
  }

  // ---- microphone actions (only dispatch once the engine has the commands) ----------------------------
  {
    Rig r(parse("in hid report=1 byte=0 bit=0 button mic_on\nin hid report=1 byte=1 abs mic_level\n"
                "in hid report=1 byte=2 abs mic_eq_low\nin hid report=1 byte=3 abs mic_eq_high\n"
                "in hid report=1 byte=0 bit=1 button mic_talkover hold=1\n"
                "out hid report=2 byte=0 bit=0 mic_on on=1 off=0\nout hid report=2 byte=0 bit=1 mic_talkover on=1 off=0\n"));
    r.send({0, 0, 0, 0});
#ifdef DECKHAND_ENGINE_HAS_MIC
    r.send({1, 0, 0, 0});
    CHECK(r.sink.last().op == Op::MicOn && r.sink.last().i == -1);
    r.send({0, 0, 0, 0});
    DeckView dv;
    r.hub.tick(&dv, 1, 1.0);
    CHECK(r.io.sent.back().data[0] == 1);  // LED follows the toggle
    r.send({0, 255, 0, 0});
    CHECK(r.sink.last().op == Op::MicLevel && r.sink.last().f == 1.0f);
    r.send({0, 255, 0, 255});
    CHECK(r.sink.last().op == Op::MicEqHigh);
    r.send({2, 255, 0, 255});
    CHECK(r.sink.last().op == Op::MicTalkover && r.sink.last().i == 1);
#else
    r.send({1, 255, 255, 255});  // parses, sends nothing
    CHECK(r.sink.commands.empty());
#endif
  }

  // ---- platform enumeration smoke test: a criterion that matches nothing opens nothing -----------------
  {
    std::atomic<int> reports{0};
    std::unique_ptr<HidIO> io = create_hid_io([&](const std::string&, int, const std::uint8_t*, std::size_t) { ++reports; }, [] {});
    if (io) {
      io->set_matches({});
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      CHECK(io->devices().empty());
      io->set_matches({HidMatch{0xDEAD, 0xBEEF, -1, -1, -1}});  // no such device
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      CHECK(io->devices().empty());
      CHECK(!io->open("no-such-device") && !io->send_output("no-such-device", 0, nullptr, 0));
      CHECK(reports == 0);
      std::printf("platform HID I/O created; criteria that match nothing listed 0 devices\n");
    } else {
      std::printf("no HID I/O on this platform\n");
    }
  }
  std::printf("test_hid ok\n");
  return 0;
}
