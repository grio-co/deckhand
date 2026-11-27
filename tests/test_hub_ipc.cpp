// Controller options and "switch decks now" through the engine process: DeviceOptions text over the hub request, the
// options as the hub applied them, and the deck side switch. The helper runs with null audio and a fake MIDI port that
// lists one pretend controller (--fake-midi); nothing opens a device. POSIX only for the process part.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include "check.hpp"
#include "deckhand/audio/ipc.hpp"
#include "deckhand/audio/remote_engine.hpp"
#include "deckhand/midi/controller.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {

bool wait_for(const std::function<bool()>& pred, double seconds) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return pred();
}

const char* kMap =
    "name \"Fake 2-deck\"\n"
    "match \"FAKE-2DECK\"\n"
    "in button 90 0B play deck=1\n"
    "in button 91 0B play deck=2\n"
    "in button 90 50 deck_switch\n";

void test_options_text() {  // the text form is what crosses the wire: it must survive a round trip
  DeviceOptions o;
  o.jog_scale = 2.5;
  o.deck_pair = static_cast<int>(DeckPair::Switchable);
  o.eq_kill = true;
  o.invert_tempo = true;
  CHECK(DeviceOptions::from_text(o.to_text()) == o);
  CHECK(DeviceOptions::from_text("").to_text().empty());
  CHECK(DeviceOptions::from_text("unknown_key=7 jog_scale=abc eq_kill=1").eq_kill);
}

#if !defined(_WIN32)
void test_process(const std::string& helper) {
  const fs::path dir = fs::temp_directory_path() / ("dh-hub-ipc-" + std::to_string(ipc::current_pid()));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "controllers", ec);
  {
    std::ofstream f(dir / "controllers" / "fake.dhmap");
    f << kMap;
  }
  RemoteEngine::Options o;
  o.data_dir = dir.string();
  o.helper_path = helper;
  o.null_audio = true;
  o.extra_args = {"--fake-midi", "FAKE-2DECK"};
  o.backoff_ms = 100;
  std::string error;
  {
    RemoteEngine eng(o);
    CHECK(eng.connect(error));
    OutputConfig oc;
    CHECK(eng.start(oc, error));
    const std::string name = "Fake 2-deck";
    CHECK(wait_for([&] { return !eng.hub_connected().empty(); }, 20));
    // not switchable yet
    int layer = -1;
    CHECK(!eng.hub_switch_decks(name, -1, &layer) && layer == 0);
    // options as text, answered with what the hub is using (sanitised)
    DeviceOptions want;
    want.jog_scale = 2.0;
    want.deck_pair = static_cast<int>(DeckPair::Switchable);
    want.crossfader_curve = static_cast<int>(CrossfaderCurve::Cut);
    want.invert_fader = true;
    std::string applied;
    CHECK(eng.hub_set_options(name, want.to_text(), &applied));
    CHECK(DeviceOptions::from_text(applied) == want);
    // unknown keys and out-of-range values are tolerated and clamped by the helper
    CHECK(eng.hub_set_options(name, want.to_text() + " future_key=9 jog_scale=900", &applied));
    CHECK(DeviceOptions::from_text(applied).jog_scale <= 8.0 && DeviceOptions::from_text(applied).invert_fader);
    CHECK(eng.hub_set_options(name, want.to_text(), &applied));
    // switch decks: toggle, explicit, toggle back
    CHECK(eng.hub_switch_decks(name, -1, &layer) && layer == 1);
    CHECK(eng.hub_switch_decks(name, 1, &layer) && layer == 1);
    CHECK(eng.hub_switch_decks(name, 0, &layer) && layer == 0);
    CHECK(eng.hub_switch_decks(name, -1, &layer) && layer == 1);
    // a mapping nobody is using: refused
    CHECK(!eng.hub_switch_decks("No such controller", -1, &layer));
    // back to the defaults
    CHECK(eng.hub_set_options(name, "", &applied) && applied.empty());
    CHECK(!eng.hub_switch_decks(name, -1, &layer));
    eng.shutdown_remote(0);
  }
  fs::remove_all(dir, ec);
}
#endif

}  // namespace

int main(int, char** argv) {
  test_options_text();
#if !defined(_WIN32)
  fs::path exe = fs::absolute(argv[0]).parent_path();
  std::string helper = (exe / "deckhand-engine").string();
  if (const char* h = std::getenv("DECKHAND_ENGINE_BIN")) helper = h;
  if (fs::exists(helper)) test_process(helper);
  else std::printf("deckhand-engine not built (%s): process test skipped\n", helper.c_str());
#else
  (void)argv;
#endif
  std::printf("hub ipc ok\n");
  return 0;
}
