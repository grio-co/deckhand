// The sequencer and the slicing in the application layer: the named controls (seq.*, sampler.slot.K.*), the settings the
// patterns are kept in, and noticing changes. The engine runs offline; nothing opens a device.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/app/controls.hpp"
#include "deckhand/app/seq_store.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {

struct Rig {
  Engine e;
  Rig() {
    e.configure_offline(44100, 2);
    render(1024);
  }
  void render(std::size_t frames) {
    std::vector<float> b(512 * 2);
    while (frames > 0) {
      const std::size_t n = std::min<std::size_t>(frames, 512);
      e.render(b.data(), static_cast<std::uint32_t>(n));
      frames -= n;
    }
  }
  bool ctl(const std::string& name, bool has = false, double v = 0) {
    std::string error;
    const bool ok = apply_control(e, name, has, v, error);
    render(1024);
    return ok;
  }
};

std::unique_ptr<LoadedSample> sample() {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = 44100;
  audio->frames = 44100;
  audio->pcm.assign(44100 * 2, 0.3f);
  auto s = std::make_unique<LoadedSample>();
  s->audio = audio;
  return s;
}

void test_controls() {
  Rig r;
  CHECK(r.ctl("seq.pattern.3.select"));
  CHECK(r.e.step_seq().current == 2);
  // a step of the pattern being played: toggle, set, clear
  CHECK(r.ctl("seq.step.5.slot2"));
  CHECK((r.e.step_seq().pattern[2].cell[4][1] & 0x80) != 0);
  double v = -1;
  CHECK(read_control(r.e, "seq.step.5.slot2", v) && v > 0);
  CHECK(r.ctl("seq.step.5.slot2"));  // toggles off
  CHECK(read_control(r.e, "seq.step.5.slot2", v) && v == 0);
  CHECK(r.ctl("seq.step.16.slot16", true, 0.5));
  CHECK(read_control(r.e, "seq.step.16.slot16", v) && std::fabs(v - 0.5) < 0.02);
  CHECK(r.ctl("seq.step.16.slot16", true, 0));
  CHECK(read_control(r.e, "seq.step.16.slot16", v) && v == 0);
  // the other patterns are not touched
  CHECK(seq_pattern_to_text(r.e.step_seq().pattern[0]) == seq_pattern_to_text(SeqPatternData{}));
  // tempo, swing, run, record
  CHECK(r.ctl("seq.bpm", true, 140) && r.ctl("seq.swing", true, 0.5));
  CHECK(std::fabs(r.e.step_seq().swing - 0.5f) < 1e-6f);
  CHECK(r.ctl("seq.run", true, 1) && r.e.step_seq().running);
  CHECK(r.ctl("seq.record") && r.e.step_seq().recording);  // toggled on
  CHECK(r.ctl("seq.record", true, 0) && !r.e.step_seq().recording);
  CHECK(r.ctl("seq.run") && !r.e.step_seq().running);        // toggled off
  // queue = next bar only while running; stopped it switches at once either way
  CHECK(r.ctl("seq.pattern.7.queue") && r.e.step_seq().current == 6);
  // clear: this pattern, one pattern, all
  CHECK(r.ctl("seq.step.1.slot1"));
  CHECK(r.ctl("seq.pattern.2.select") && r.ctl("seq.step.2.slot2"));
  CHECK(r.ctl("seq.clear") && seq_pattern_to_text(r.e.step_seq().pattern[1]) == seq_pattern_to_text(SeqPatternData{}));
  CHECK((r.e.step_seq().pattern[6].cell[0][0] & 0x80) != 0);
  CHECK(r.ctl("seq.clear", true, 7) && (r.e.step_seq().pattern[6].cell[0][0] & 0x80) == 0);
  CHECK(r.ctl("seq.pattern.1.select") && r.ctl("seq.step.3.slot3") && r.ctl("seq.clear", true, 0));
  CHECK(seq_pattern_to_text(r.e.step_seq().pattern[0]) == seq_pattern_to_text(SeqPatternData{}));
  // slices and scratch
  r.e.sampler_load(3, sample());
  r.render(4096);
  CHECK(r.ctl("sampler.slot.4.slices", true, 8) && r.e.sampler(3).slices == 8);
  CHECK(!r.ctl("sampler.slot.4.slices", true, 5));
  CHECK(r.ctl("sampler.slot.4.slice.3") && r.e.sampler(3).slice == 2);
  CHECK(r.ctl("sampler.slot.4.slice.8", true, 0.5) && r.e.sampler(3).slice == 7);
  CHECK(r.ctl("sampler.slot.4.scratch") && r.e.sampler(3).scratching);
  CHECK(r.ctl("sampler.slot.4.scratch.delta", true, -0.01));
  CHECK(r.ctl("sampler.slot.4.scratch", true, 0) && !r.e.sampler(3).scratching);
  CHECK(r.ctl("sampler.slot.4.slices", true, 0) && r.e.sampler(3).slices == 0);
  // what is refused
  for (const char* bad : {"seq.step.17.slot1", "seq.step.0.slot1", "seq.step.1.slot17", "seq.pattern.9.select", "seq.pattern.0.queue",
                          "sampler.slot.17.slices", "sampler.slot.1.slice.17", "sampler.slot.1.slice.0", "seq.nothing"})
    CHECK(!r.ctl(bad));
  CHECK(!r.ctl("seq.bpm") && !r.ctl("seq.bpm", true, 900) && !r.ctl("seq.swing", true, 0.9) && !r.ctl("seq.clear", true, 12));
  CHECK(!r.ctl("seq.step.1.slot1", true, 2));
  // the control list names them
  bool run = false, slices = false;
  for (const ControlInfo& c : list_controls()) {
    run |= c.name == "seq.run";
    slices |= c.name == "sampler.slot.1.slices";
  }
  CHECK(run && slices);
}

void test_store_and_restore() {
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_seq_store";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  std::string text2, text5;
  {
    Library lib((dir / "library.db").string());
    Rig a;
    a.ctl("seq.pattern.3.select");
    a.ctl("seq.step.1.slot1");
    a.ctl("seq.step.9.slot4", true, 0.25);
    a.ctl("seq.pattern.6.select");
    a.ctl("seq.step.16.slot16");
    a.ctl("seq.bpm", true, 133);
    a.ctl("seq.swing", true, 0.25);
    a.render(2048);
    const StepSeqView v = a.e.step_seq();
    text2 = seq_pattern_to_text(v.pattern[2]);
    text5 = seq_pattern_to_text(v.pattern[5]);
    save_sequencer(lib, v);
    CHECK(lib.setting("seq.pattern.3") == text2 && lib.setting("seq.pattern.6") == text5);
    CHECK(lib.setting("seq.pattern.1").empty() && lib.setting("seq.current") == "6");
    CHECK(std::fabs(std::atof(lib.setting("seq.bpm").c_str()) - 133) < 0.01 && std::fabs(std::atof(lib.setting("seq.swing").c_str()) - 0.25) < 0.01);
  }
  {
    // a fresh engine (an engine process that was restarted) gets everything back
    Library lib((dir / "library.db").string());
    Rig b;
    CHECK(restore_sequencer(lib, b.e) == 2);
    b.render(4096);
    const StepSeqView v = b.e.step_seq();
    CHECK(seq_pattern_to_text(v.pattern[2]) == text2 && seq_pattern_to_text(v.pattern[5]) == text5);
    CHECK(v.current == 5 && std::fabs(v.swing - 0.25f) < 1e-6f && std::fabs(v.bpm - 133.0f) < 0.01f);
    // saving again writes nothing new and an emptied pattern empties its setting
    b.ctl("seq.clear", true, 6);
    b.render(1024);
    save_sequencer(lib, b.e.step_seq());
    CHECK(lib.setting("seq.pattern.6").empty() && lib.setting("seq.pattern.3") == text2);
  }
  fs::remove_all(dir, ec);
}

void test_watch() {
  Rig r;
  SeqWatch w;
  CHECK(!w.changed(r.e.step_seq()));  // the first look only records
  CHECK(!w.changed(r.e.step_seq()));
  r.ctl("seq.step.1.slot1");
  CHECK(w.changed(r.e.step_seq()));
  CHECK(!w.changed(r.e.step_seq()));
  r.ctl("seq.swing", true, 0.3);
  CHECK(w.changed(r.e.step_seq()));
  r.ctl("seq.pattern.4.select");
  CHECK(w.changed(r.e.step_seq()));
}

}  // namespace

int main() {
  test_controls();
  test_store_and_restore();
  test_watch();
  std::puts("seq app ok");
  return 0;
}
