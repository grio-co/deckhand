// Slice steps through the engine process (RemoteEngine -> deckhand-engine, null audio): wire validation of the slice code,
// pattern text with slices over the control socket, the step command with a slice, and a run that plays the slice.
// Skips the process part when deckhand-engine is not built or on Windows (the wire checks run everywhere).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/ipc.hpp"
#include "deckhand/audio/remote_engine.hpp"

using namespace deckhand;
using namespace deckhand::ipc;
namespace fs = std::filesystem;

namespace {
bool wait_for(const std::function<bool()>& pred, double seconds) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}
Command cmd(Op op, int deck, float f, int i, double d) {
  Command c;
  c.op = op;
  c.deck = static_cast<std::uint8_t>(deck);
  c.f = f;
  c.i = i;
  c.d = d;
  return c;
}

void test_wire() {
  CHECK(kProtocolVersion >= 10);
  Command out;
  CHECK(validate_command(to_wire(cmd(Op::SeqStep, 3, 0.5f, 5 * 16 + 7 + (1 << 8), 1)), out) && out.i == 5 * 16 + 7 + 256);
  CHECK(validate_command(to_wire(cmd(Op::SeqStep, 3, 0.5f, 127 + (17 << 8), 1)), out));
  CHECK(!validate_command(to_wire(cmd(Op::SeqStep, 3, 0.5f, 3 + (18 << 8), 1)), out));  // slice code out of range
  CHECK(!validate_command(to_wire(cmd(Op::SeqStep, 3, 0.5f, 128 + (2 << 8), 1)), out));  // pattern out of range
}

std::unique_ptr<LoadedSample> tone() {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = 44100;
  audio->frames = 44100;
  audio->pcm.assign(audio->frames * 2, 0.2f);
  auto s = std::make_unique<LoadedSample>();
  s->audio = audio;
  return s;
}

#if !defined(_WIN32)
void test_process(const std::string& helper) {
  const std::string dir = (fs::temp_directory_path() / ("dh-slice-ipc-" + std::to_string(current_pid()))).string();
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  RemoteEngine::Options o;
  o.data_dir = dir;
  o.helper_path = helper;
  o.null_audio = true;
  o.log_level = 2;
  o.backoff_ms = 100;
  std::string error;
  {
    RemoteEngine eng(o);
    CHECK(eng.connect(error));
    OutputConfig oc;
    CHECK(eng.start(oc, error));
    eng.send(cmd(Op::MasterLevel, 0, 0.0f, 0, 0));
    eng.send(cmd(Op::HeadLevel, 0, 0.0f, 0, 0));
    eng.sampler_load(15, tone());
    CHECK(wait_for([&] { return eng.sampler(15).loaded; }, 10));
    eng.sampler_slice(15, 16);
    CHECK(wait_for([&] { return eng.sampler(15).slices == 16; }, 5));
    // text with slices over the socket, read back from the snapshot
    const std::string text = "dhseq1;8000:7f@b;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-";
    CHECK(!eng.seq_pattern_set(0, "dhseq1;8000:7f@;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-"));
    CHECK(eng.seq_pattern_set(4, text));
    CHECK(wait_for([&] { return eng.seq_pattern_text(4) == text; }, 5));
    CHECK(eng.step_seq().pattern[4].slice[0][15] == 12);
    // the step command with a slice, keep (-1) and whole (-2)
    eng.seq_step(5, 2, 15, true, 0.5f, 7);
    CHECK(wait_for([&] { return eng.step_seq().pattern[5].slice[2][15] == 8; }, 5));
    eng.seq_step(5, 2, 15, true, 1.0f);
    CHECK(wait_for([&] { return (eng.step_seq().pattern[5].cell[2][15] & 0x7F) == 127; }, 5));
    CHECK(eng.step_seq().pattern[5].slice[2][15] == 8);
    eng.seq_step(5, 2, 15, true, -1.0f, -2);
    CHECK(wait_for([&] { return eng.step_seq().pattern[5].slice[2][15] == 0; }, 5));
    // a run plays that slice: the sampler view reports the slice last triggered
    eng.seq_select(4, false);
    eng.seq_bpm(240);
    eng.seq_run(1);
    CHECK(wait_for([&] { return eng.sampler(15).slice == 11; }, 10));
    eng.seq_run(0);
    CHECK(wait_for([&] { return !eng.step_seq().running; }, 5));
    eng.shutdown_remote(0);
  }
  fs::remove_all(dir, ec);
}
#endif
}  // namespace

int main(int, char** argv) {
  test_wire();
#if !defined(_WIN32)
  std::string helper = (fs::absolute(argv[0]).parent_path() / "deckhand-engine").string();
  if (const char* h = std::getenv("DECKHAND_ENGINE_BIN")) helper = h;
  std::error_code ec;
  if (fs::exists(helper, ec)) test_process(helper);
  else std::printf("deckhand-engine not built: process part skipped\n");
#else
  (void)argv;
#endif
  std::printf("sampler slices ipc ok\n");
  return 0;
}
