// The sampler features through the engine process (RemoteEngine -> deckhand-engine, null audio): protocol 8 wire
// validation, the step sequencer snapshot, pattern text over the control socket, slices and scratch.
// POSIX only for the process part (the Windows engine process is covered by platform/windows/tests); the wire
// validation runs everywhere. Skips the process part when deckhand-engine is not built.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

Command cmd(Op op, int deck = 0, float f = 0, int i = 0, double d = 0) {
  Command c;
  c.op = op;
  c.deck = static_cast<std::uint8_t>(deck);
  c.f = f;
  c.i = i;
  c.d = d;
  return c;
}

WireCommand wire(Op op, int deck = 0, float f = 0, int i = 0, double d = 0) { return to_wire(cmd(op, deck, f, i, d)); }

void test_wire() {
  CHECK(kProtocolVersion >= 8);
  Command out;
  CHECK(validate_command(wire(Op::SeqRun, 0, 0, 1), out) && out.op == Op::SeqRun && out.i == 1);
  CHECK(validate_command(wire(Op::SeqRun, 0, 0, -1), out));
  CHECK(!validate_command(wire(Op::SeqRun, 0, 0, 2), out));
  CHECK(validate_command(wire(Op::SeqBpm, 0, 128.5f), out) && !validate_command(wire(Op::SeqBpm, 0, 500.0f), out));
  CHECK(validate_command(wire(Op::SeqSwing, 0, 0.75f), out) && !validate_command(wire(Op::SeqSwing, 0, 0.9f), out));
  CHECK(validate_command(wire(Op::SeqStep, 15, 0.5f, 7 * 16 + 15, 1), out) && out.deck == 15);
  CHECK(!validate_command(wire(Op::SeqStep, 16, 0.5f, 3, 1), out));       // slot out of range
  CHECK(!validate_command(wire(Op::SeqStep, 0, 0.5f, 8 * 16, 1), out));   // pattern out of range
  CHECK(!validate_command(wire(Op::SeqStep, 0, 0.5f, -1, 1), out));
  CHECK(validate_command(wire(Op::SeqSelect, 0, 0, 7, 1), out) && !validate_command(wire(Op::SeqSelect, 0, 0, 8), out));
  CHECK(validate_command(wire(Op::SeqRecord, 0, 0, -1), out));
  CHECK(validate_command(wire(Op::SeqClear, 0, 0, -1, -1), out) && !validate_command(wire(Op::SeqClear, 0, 0, 8, -1), out));
  CHECK(!validate_command(wire(Op::SeqPatternSwap, 0, 0, 0), out));  // pointer command: never from the wire
  CHECK(validate_command(wire(Op::SamplerSlice, 3, 0, 8), out) && validate_command(wire(Op::SamplerSlice, 3, 0, 16), out));
  CHECK(!validate_command(wire(Op::SamplerSlice, 3, 0, 4), out) && !validate_command(wire(Op::SamplerSlice, 16, 0, 8), out));
  CHECK(validate_command(wire(Op::SamplerSliceTrigger, 3, 1.0f, 15), out) && !validate_command(wire(Op::SamplerSliceTrigger, 3, 1.0f, 16), out));
  CHECK(validate_command(wire(Op::SamplerScratch, 3, 0, 2, -0.01), out) && !validate_command(wire(Op::SamplerScratch, 3, 0, 3), out));
  CHECK(!validate_command(wire(Op::SamplerScratch, 3, 0, 2, 1e9), out));
  WireCommand w = wire(Op::SeqBpm, 0, 100.0f);
  w.f = std::nanf("");
  CHECK(!validate_command(w, out));
}

std::unique_ptr<LoadedSample> click() {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = 44100;
  audio->frames = 44100 * 2;
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i)
    audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = 0.3f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
  auto s = std::make_unique<LoadedSample>();
  s->audio = audio;
  return s;
}

#if !defined(_WIN32)
void test_process(const std::string& helper) {
  const std::string dir = (fs::temp_directory_path() / ("dh-sampler-ipc-" + std::to_string(current_pid()))).string();
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
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
    eng.send(cmd(Op::MasterLevel, 0, 0.0f));
    eng.send(cmd(Op::HeadLevel, 0, 0.0f));
    eng.sampler_load(15, click());
    CHECK(wait_for([&] { return eng.sampler(15).loaded; }, 10));
    CHECK(!eng.seq_pattern_set(0, "garbage") && !eng.seq_pattern_set(9, seq_pattern_to_text(SeqPatternData{})));
    // pattern text over the control socket, read back from the shared snapshot
    SeqPatternData p;
    p.cell[0][15] = 0x80 | 100;
    p.cell[4][15] = 0x80 | 50;
    p.cell[8][0] = 0x80 | 127;
    const std::string text = seq_pattern_to_text(p);
    CHECK(eng.seq_pattern_set(2, text));
    CHECK(wait_for([&] { return eng.seq_pattern_text(2) == text; }, 5));
    CHECK(eng.seq_pattern_text(1) == seq_pattern_to_text(SeqPatternData{}));
    // edits through commands
    eng.seq_step(3, 0, 15, true, 0.25f);
    CHECK(wait_for([&] { return (eng.step_seq().pattern[3].cell[0][15] & 0x80) != 0; }, 5));
    CHECK((eng.step_seq().pattern[3].cell[0][15] & 0x7F) == 32);
    eng.seq_clear(3, -1);
    CHECK(wait_for([&] { return eng.step_seq().pattern[3].cell[0][15] == 0; }, 5));
    // run at a set tempo: the step marker advances and slot 15 plays
    eng.seq_select(2, false);
    eng.seq_bpm(240);
    eng.seq_swing(0.25);
    eng.seq_run(1);
    CHECK(wait_for([&] { return eng.step_seq().running && eng.step_seq().current == 2; }, 5));
    CHECK(wait_for([&] { return eng.sampler(15).playing; }, 10));
    CHECK(eng.step_seq().bpm == 240.0f && !eng.step_seq().locked && std::fabs(eng.step_seq().swing - 0.25f) < 1e-6f);
    int seen = 0, last = -2;
    CHECK(wait_for([&] {
      const int s = eng.step_seq().step;
      if (s != last) {
        last = s;
        ++seen;
      }
      return seen >= 5;
    }, 15));
    eng.seq_run(0);
    CHECK(wait_for([&] { return !eng.step_seq().running && eng.step_seq().step == 0xFF; }, 5));
    // slices and scratch show in the sampler view
    eng.sampler_slice(15, 8);
    CHECK(wait_for([&] { return eng.sampler(15).slices == 8; }, 5));
    eng.sampler_slice_trigger(15, 5);
    CHECK(wait_for([&] { return eng.sampler(15).slice == 5; }, 5));
    eng.sampler_scratch(15, 1);
    CHECK(wait_for([&] { return eng.sampler(15).scratching; }, 5));
    eng.sampler_scratch(15, 0);
    CHECK(wait_for([&] { return !eng.sampler(15).scratching; }, 5));
    eng.shutdown_remote(0);
  }
  fs::remove_all(dir, ec);
}
#endif

}  // namespace

int main(int, char** argv) {
  test_wire();
#if !defined(_WIN32)
  fs::path exe = fs::absolute(argv[0]).parent_path();
  std::string helper = (exe / "deckhand-engine").string();
  if (const char* h = std::getenv("DECKHAND_ENGINE_BIN")) helper = h;
  if (fs::exists(helper)) {
    test_process(helper);
    std::printf("sampler ipc ok\n");
  } else {
    std::printf("deckhand-engine not built (%s): process tests skipped\nsampler ipc ok (wire only)\n", helper.c_str());
  }
#else
  (void)argv;
  std::printf("sampler ipc ok (wire only on Windows)\n");
#endif
  return 0;
}
