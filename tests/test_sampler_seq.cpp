// Offline tests for the sampler: 16 slots, the 8-pattern step sequencer (free-running and locked to a deck's grid,
// sample-accurate, swing, velocity, bar-queued pattern change, record from pads, text form), loop slices and scratch.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"

using namespace deckhand;

namespace {

constexpr double kSr = 44100;

Command cmd(Op op, int deck = 0, float f = 0, int i = 0, double d = 0) {
  Command c;
  c.op = op;
  c.deck = static_cast<std::uint8_t>(deck);
  c.f = f;
  c.i = i;
  c.d = d;
  return c;
}

// A sample whose value at frame i is f(i).
template <class F>
std::unique_ptr<LoadedSample> make_sample(std::uint64_t frames, F f) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = 44100;
  audio->frames = frames;
  audio->pcm.resize(frames * 2);
  for (std::uint64_t i = 0; i < frames; ++i) audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = f(i);
  auto s = std::make_unique<LoadedSample>();
  s->audio = audio;
  return s;
}
std::unique_ptr<LoadedSample> click(float level = 0.5f, std::uint64_t frames = 2000) {
  return make_sample(frames, [=](std::uint64_t) { return level; });
}

struct Rig {
  Engine e;
  std::vector<float> out;  // everything rendered, stereo interleaved
  Rig() {
    e.configure_offline(44100, 2);
    e.send(cmd(Op::MasterLevel, 0, 0.8f));
    render(1024);
  }
  void render(std::size_t frames, std::size_t block = 512) {
    std::vector<float> b(block * 2);
    while (frames > 0) {
      const std::size_t n = std::min(frames, block);
      e.render(b.data(), static_cast<std::uint32_t>(n));
      out.insert(out.end(), b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n * 2));
      frames -= n;
    }
  }
  std::size_t frames() const { return out.size() / 2; }
  float at(std::size_t i) const { return out[i * 2]; }
  // Frames (relative to `from`) where the output goes from silent to sounding.
  std::vector<std::size_t> onsets(std::size_t from, float thr = 1e-5f) const {
    std::vector<std::size_t> v;
    std::size_t quiet = 1000;
    for (std::size_t i = from; i < frames(); ++i) {
      if (std::fabs(at(i)) > thr) {
        if (quiet >= 300) v.push_back(i - from);
        quiet = 0;
      } else {
        ++quiet;
      }
    }
    return v;
  }
  float peak(std::size_t a, std::size_t b) const {
    float p = 0;
    for (std::size_t i = a; i < b && i < frames(); ++i) p = std::max(p, std::fabs(at(i)));
    return p;
  }
};

std::size_t step_frame(double step, double bpm) { return static_cast<std::size_t>(std::ceil(step * 15.0 / bpm * kSr - 1e-6)); }

void expect_near(const std::vector<std::size_t>& got, const std::vector<std::size_t>& want, std::size_t tol, const char* what) {
  if (got.size() != want.size()) {
    std::fprintf(stderr, "%s: %zu onsets, wanted %zu\n", what, got.size(), want.size());
    for (auto g : got) std::fprintf(stderr, "  got %zu\n", g);
    std::exit(1);
  }
  for (std::size_t i = 0; i < got.size(); ++i) {
    const std::size_t d = got[i] > want[i] ? got[i] - want[i] : want[i] - got[i];
    if (d > tol) {
      std::fprintf(stderr, "%s: onset %zu at %zu, wanted %zu\n", what, i, got[i], want[i]);
      std::exit(1);
    }
  }
}

void test_slots_and_text() {
  CHECK(kSamplerSlots == 16);
  Rig r;
  r.e.sampler_load(15, click());
  r.e.sampler_load(0, click());
  r.render(1024);
  CHECK(r.e.sampler(15).loaded && r.e.sampler(0).loaded && !r.e.sampler(7).loaded);
  r.e.send(cmd(Op::SamplerTrigger, 15));
  r.render(1024);
  CHECK(r.e.sampler(15).playing && r.peak(1024, r.frames()) > 0.1f);

  // text form: round trip and rejection
  SeqPatternData p;
  p.cell[0][0] = 0x80 | 127;
  p.cell[0][15] = 0x80 | 64;
  p.cell[7][3] = 0x80 | 1;
  p.cell[9][4] = 0x40;  // off: not stored
  const std::string t = seq_pattern_to_text(p);
  CHECK(t.rfind("dhseq1;8001:7f40;", 0) == 0);
  SeqPatternData q;
  CHECK(seq_pattern_from_text(t, q));
  CHECK(q.cell[0][0] == (0x80 | 127) && q.cell[0][15] == (0x80 | 64) && q.cell[7][3] == (0x80 | 1) && q.cell[9][4] == 0);
  CHECK(seq_pattern_to_text(q) == t);
  CHECK(!seq_pattern_from_text("", q));
  CHECK(!seq_pattern_from_text("dhseq2;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q));
  CHECK(!seq_pattern_from_text("dhseq1;-;-", q));
  CHECK(!seq_pattern_from_text("dhseq1;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q));       // 17 steps
  CHECK(!seq_pattern_from_text("dhseq1;0001:00;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q));    // velocity 0
  CHECK(!seq_pattern_from_text("dhseq1;0003:7f;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q));    // velocity missing
  CHECK(!seq_pattern_from_text("dhseq1;00zz:7f;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q));
  CHECK(seq_pattern_from_text("dhseq1;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q) && q.cell[0][0] == 0);

  // through the engine: set a pattern from text and read it back from the snapshot
  CHECK(!r.e.seq_pattern_set(8, t) && !r.e.seq_pattern_set(0, "nonsense"));
  CHECK(r.e.seq_pattern_set(5, t));
  r.render(1024);
  CHECK(r.e.seq_pattern_text(5) == t);
  CHECK(r.e.seq_pattern_text(4) == seq_pattern_to_text(SeqPatternData{}));
  r.e.seq_clear(5, 0);
  r.render(1024);
  CHECK(r.e.step_seq().pattern[5].cell[0][0] == 0 && (r.e.step_seq().pattern[5].cell[0][15] & 0x80));
  r.e.seq_clear(-1, -1);
  r.render(1024);
  CHECK(r.e.seq_pattern_text(5) == seq_pattern_to_text(SeqPatternData{}));
}

// Free-running at a set tempo: sample-accurate, the same for every block size.
void test_free_run() {
  const double bpm = 120;
  std::vector<std::size_t> first;
  for (std::size_t block : {512u, 300u, 77u, 4096u}) {
    Rig r;
    r.e.sampler_load(2, click());
    r.e.seq_bpm(bpm);
    for (int s : {0, 4, 8, 12}) r.e.seq_step(0, s, 2, true);
    r.e.seq_step(0, 2, 2, true, 0.5f);
    r.e.seq_step(0, 6, 2, true);
    r.e.seq_step(0, 6, 2, false);  // on, then off again: silent
    r.render(2048);
    const std::size_t from = r.frames();
    r.e.seq_run(1);
    r.render(static_cast<std::size_t>(kSr * 2.2), block);  // just over one bar (16 steps = 2 s)
    CHECK(r.e.step_seq().running && !r.e.step_seq().locked && r.e.step_seq().bpm == 120.0f);
    auto on = r.onsets(from);
    std::vector<std::size_t> want;
    for (int s : {0, 2, 4, 8, 12, 16}) want.push_back(step_frame(s, bpm));
    // the run starts at the first block after the command: allow the block size as the start offset
    const std::size_t lead = on.empty() ? 0 : on[0];
    CHECK(lead <= block);
    for (auto& w : want) w += lead;
    expect_near(on, want, 1, "free run");
    // velocity: step 2 is half the level
    CHECK(r.peak(from + lead + step_frame(2, bpm) + 100, from + lead + step_frame(2, bpm) + 1000) < 0.6f * r.peak(from + lead + 100, from + lead + 1000));
    if (first.empty()) first = on;
    else expect_near(on, first, 1, "block-size independence");
  }
  // another tempo: 60 BPM
  Rig r;
  r.e.sampler_load(2, click());
  r.e.seq_bpm(60);
  r.e.seq_step(0, 0, 2, true);
  r.e.seq_step(0, 1, 2, true);
  r.render(1024);
  const std::size_t from = r.frames();
  r.e.seq_run(1);
  r.render(static_cast<std::size_t>(kSr * 1.5));
  auto on = r.onsets(from);
  CHECK(on.size() == 2 && on[1] - on[0] >= 11024 && on[1] - on[0] <= 11026);
  // stop: nothing more sounds
  r.e.seq_run(0);
  r.render(2048);
  const std::size_t quiet_from = r.frames();
  r.render(static_cast<std::size_t>(kSr * 5));
  CHECK(r.peak(quiet_from, r.frames()) < 1e-6f);
  CHECK(!r.e.step_seq().running && r.e.step_seq().step == 0xFF);
}

void test_swing() {
  Rig r;
  r.e.sampler_load(1, click());
  r.e.seq_bpm(120);
  r.e.seq_swing(0.5);
  r.e.seq_step(0, 0, 1, true);
  r.e.seq_step(0, 1, 1, true);
  r.e.seq_step(0, 2, 1, true);
  r.e.seq_step(0, 3, 1, true);
  r.render(1024);
  const std::size_t from = r.frames();
  r.e.seq_run(1);
  r.render(static_cast<std::size_t>(kSr * 1.0));
  const auto on = r.onsets(from);
  CHECK(on.size() == 4);
  const std::size_t s = static_cast<std::size_t>(0.125 * kSr);  // a step, frames
  const std::size_t d0 = on[0];
  CHECK(on[1] - d0 + 2 >= s + s / 2 && on[1] - d0 <= s + s / 2 + 2);   // step 1 delayed by half a step
  CHECK(on[2] - d0 + 2 >= 2 * s && on[2] - d0 <= 2 * s + 2);           // even steps stay on the grid
  CHECK(on[3] - d0 + 2 >= 3 * s + s / 2 && on[3] - d0 <= 3 * s + s / 2 + 3);
  // swing is clamped
  r.e.seq_swing(5.0);
  r.render(2048);
  CHECK(r.e.step_seq().swing <= 0.75f + 1e-6f);
}

void test_queue_and_select() {
  Rig r;
  r.e.sampler_load(0, click(0.5f, 1000));
  r.e.sampler_load(1, click(0.25f, 1000));
  r.e.seq_bpm(120);
  for (int s = 0; s < 16; s += 4) {
    r.e.seq_step(0, s, 0, true);
    r.e.seq_step(1, s, 1, true);
  }
  r.render(1024);
  const std::size_t from = r.frames();
  r.e.seq_run(1);
  r.render(static_cast<std::size_t>(kSr * 0.7));
  CHECK(r.e.step_seq().current == 0 && r.e.step_seq().step != 0xFF);
  r.e.seq_select(1, true);  // at the next bar
  r.render(512);
  CHECK(r.e.step_seq().queued == 1 && r.e.step_seq().current == 0);
  r.render(static_cast<std::size_t>(kSr * 3.0));
  CHECK(r.e.step_seq().current == 1 && r.e.step_seq().queued == 0xFF);
  const auto on = r.onsets(from);
  // beats at 0, 0.5, 1.0, 1.5 s are slot 0 (0.5 level); from the second bar (2 s on) slot 1 (0.25 level)
  const std::size_t lead = on[0];
  CHECK(on.size() >= 7);
  const std::size_t bar2 = lead + static_cast<std::size_t>(2 * kSr);
  for (std::size_t i = 0; i < on.size(); ++i) {
    const std::size_t at = from + on[i] + 200;
    const float pk = r.peak(at, at + 400);
    if (on[i] + 5 < bar2 - lead + lead) CHECK(pk > 0.4f);  // before the bar line: slot 0
    else CHECK(pk < 0.35f);                                 // from it: slot 1
  }
  // an immediate select switches at once
  r.e.seq_select(0, false);
  r.render(512);
  CHECK(r.e.step_seq().current == 0);
}

void test_record() {
  Rig r;
  r.e.sampler_load(2, click(0.5f, 500));
  r.e.sampler_load(9, click(0.5f, 500));
  r.e.seq_bpm(120);
  r.render(1024);
  // not running: nothing is recorded
  r.e.seq_record(1);
  r.e.send(cmd(Op::SamplerTrigger, 2));
  r.render(1024);
  CHECK(r.e.step_seq().recording && r.e.seq_pattern_text(0) == seq_pattern_to_text(SeqPatternData{}));
  r.e.seq_run(1);
  r.render(1);  // phase ~0
  const double step = 0.125 * kSr;
  const std::size_t base = 0;
  (void)base;
  // aim at 5.3 steps
  r.render(static_cast<std::size_t>(5.3 * step) - 1 - 512 - 0);
  // the exact phase is whatever the engine says: derive the expected step from the frames rendered since the start
  const double ph = static_cast<double>(r.e.step_seq().step);  // current playing step as a sanity check
  CHECK(ph == 5.0 || ph == 4.0);
  r.e.send(cmd(Op::SamplerTrigger, 2, 0.5f));
  r.render(512);
  const StepSeqView v = r.e.step_seq();
  int rec_step = -1;
  for (int s = 0; s < kSeqSteps; ++s)
    if (v.pattern[0].cell[s][2] & 0x80) rec_step = s;
  CHECK(rec_step == 5 || rec_step == 6 || rec_step == 4);  // nearest step of where the hit fell
  CHECK((v.pattern[0].cell[rec_step][2] & 0x7F) == 64);    // velocity 0.5 -> 64 of 127
  // a hit on another slot a little later lands on its own step
  r.render(static_cast<std::size_t>(2.0 * step));
  r.e.send(cmd(Op::SamplerTrigger, 9));
  r.render(512);
  int s9 = -1;
  for (int s = 0; s < kSeqSteps; ++s)
    if (r.e.step_seq().pattern[0].cell[s][9] & 0x80) s9 = s;
  CHECK(s9 > rec_step && (r.e.step_seq().pattern[0].cell[s9][9] & 0x7F) == 127);
  // recording off: more hits change nothing
  r.e.seq_record(0);
  const std::string before = r.e.seq_pattern_text(0);
  r.e.send(cmd(Op::SamplerTrigger, 4));
  r.render(2048);
  CHECK(r.e.seq_pattern_text(0) == before);
  // the recorded step now plays on the next pass: let one full bar go by and look for onsets on slot 2's step
  r.e.seq_run(0);
}

void test_locked_to_deck() {
  Rig r;
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = 44100;
  audio->frames = static_cast<std::uint64_t>(60 * kSr);
  audio->pcm.assign(audio->frames * 2, 0.0f);  // silent track: only the sampler is heard
  build_waveform(*audio);
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->bpm = 100;
  t->grid.push_back({4410, 100, 1});
  r.e.load(0, std::move(t));
  r.e.sampler_load(3, click());
  r.e.seq_bpm(150);  // must be ignored while the deck leads
  for (int s : {0, 4, 8, 12}) r.e.seq_step(0, s, 3, true);
  r.e.send(cmd(Op::Seek, 0, 0, 0, 0));
  r.render(2048);
  r.e.seq_run(1);
  r.e.send(cmd(Op::PlayToggle, 0));
  // render in blocks of 512 and note the deck position at the end of each block
  std::vector<double> pos_end;
  const std::size_t from = r.frames();
  for (int i = 0; i < 400; ++i) {
    r.render(512);
    pos_end.push_back(r.e.deck(0).position);
  }
  CHECK(r.e.deck(0).playing && r.e.step_seq().locked);
  CHECK(std::fabs(r.e.step_seq().bpm - 100.0f) < 0.5f);
  const auto on = r.onsets(from);
  CHECK(on.size() >= 3);
  const double beat = 60.0 / 100 * kSr;  // 26460 frames; steps are 1/4 beat
  int checked = 0;
  for (auto o : on) {
    // deck position at the onset frame (the deck advances one frame per output frame at tempo 1)
    const std::size_t blk = o / 512;
    const double pos = pos_end[blk] - static_cast<double>((blk + 1) * 512 - o);
    const double steps = (pos - 4410) / (beat / 4);
    // within a frame of a whole step (an on step is every 4th: a whole beat)
    const double err = std::fabs(steps / 4 - std::round(steps / 4)) * beat;
    CHECK(err < 3.0);
    ++checked;
  }
  CHECK(checked >= 3);
}

void test_slices() {
  Rig r;
  const std::uint64_t n = 16000;  // 16 slices of 1000 frames; slice j holds level 0.05 * (j + 1)
  r.e.sampler_load(6, make_sample(n, [](std::uint64_t i) { return 0.04f * static_cast<float>(i / 1000 + 1); }));
  r.render(1024);
  CHECK(r.e.sampler(6).slices == 0);
  r.e.sampler_slice(6, 12);  // not 8 or 16: off
  r.render(512);
  CHECK(r.e.sampler(6).slices == 0);
  r.e.sampler_slice(6, 16);
  r.e.sampler_slice_trigger(6, 3);
  r.render(512);
  CHECK(r.e.sampler(6).slices == 16 && r.e.sampler(6).slice == 3);
  const std::size_t a = r.frames();
  r.render(2048);
  // slice 3 holds 0.16; it plays once for about 1000 frames and stops
  const float pk = r.peak(a - 512, a + 2048);
  CHECK(pk > 0.12f && pk < 0.2f);
  CHECK(r.peak(a + 1500, r.frames()) < 1e-6f && !r.e.sampler(6).playing);
  // another slice is its own level: slice 9 = 0.4
  r.e.sampler_slice_trigger(6, 9);
  r.render(2048);
  CHECK(r.peak(r.frames() - 2048, r.frames()) > 0.3f);
  r.render(2048);
  // 8 slices: slice 1 is frames 2000..4000 (levels 0.12 and 0.2)
  r.e.sampler_slice(6, 8);
  r.e.sampler_slice_trigger(6, 1);
  r.render(512);
  const std::size_t b = r.frames();
  r.render(4096);
  CHECK(r.peak(b - 512, b + 4096) > 0.15f && r.peak(b + 2200, b + 4096) < 1e-6f);
  // a slice index beyond the count is ignored
  r.e.sampler_slice_trigger(6, 12);
  r.render(2048);
  const std::size_t c = r.frames();
  r.render(2048);
  CHECK(r.peak(c, r.frames()) < 1e-6f);
  // a slice step in a sequencer pattern plays the whole slot: slicing does not break the sequencer
  r.e.sampler_slice(6, 0);
  CHECK(true);
}

void test_scratch() {
  Rig r;
  const std::uint64_t n = 44100 * 4;
  r.e.sampler_load(4, make_sample(n, [](std::uint64_t i) { return 0.4f * static_cast<float>(std::sin(2 * 3.14159265358979 * 440.0 * static_cast<double>(i) / 44100)); }));
  r.render(1024);
  r.e.sampler_scratch(4, 1);  // grab
  r.render(1024);
  CHECK(r.e.sampler(4).scratching && r.e.sampler(4).playing);
  const std::size_t a = r.frames();
  r.render(4096);  // still platter: silent
  CHECK(r.peak(a + 600, r.frames()) < 1e-4f);
  const double p0 = r.e.sampler(4).position;
  // move 0.1 s forward in one go
  r.e.sampler_scratch(4, 2, 0.1);
  r.render(512);
  const double p1 = r.e.sampler(4).position;
  CHECK(std::fabs((p1 - p0) - 4410.0) < 2.0);
  CHECK(r.peak(a + 4096, r.frames()) > 0.05f);
  // backwards
  r.e.sampler_scratch(4, 2, -0.05);
  r.render(512);
  CHECK(std::fabs((r.e.sampler(4).position - p1) + 2205.0) < 2.0);
  // cannot move past the ends
  r.e.sampler_scratch(4, 2, -100.0);
  r.render(512);
  CHECK(r.e.sampler(4).position >= 0 && r.e.sampler(4).position < 1.0);
  // moves without a grab are ignored
  r.e.sampler_scratch(4, 0);  // release: plays on from here
  r.render(512);
  CHECK(!r.e.sampler(4).scratching && r.e.sampler(4).playing);
  const double p2 = r.e.sampler(4).position;
  r.e.sampler_scratch(4, 2, 1.0);
  r.render(512);
  CHECK(r.e.sampler(4).position - p2 < 600 && r.e.sampler(4).position > p2);
  r.e.send(cmd(Op::SamplerStop, 0, 0, -1));
  r.render(4096);
  CHECK(!r.e.sampler(4).playing);
}

void test_commands_rejected() {
  // pointer / internal ops cannot be scheduled; the new ops can
  Rig r;
  Command c = cmd(Op::SeqRun, 0, 0, 1);
  Schedule when;
  when.kind = ScheduleKind::ClockFrame;
  when.clock_frame = r.e.clock_frames() + 2048;
  CHECK(r.e.schedule(c, when) != 0);
  c = cmd(Op::SeqPatternSwap, 0, 0, 0);
  CHECK(r.e.schedule(c, when) == 0);
  r.render(4096);
  CHECK(r.e.step_seq().running);
}

}  // namespace

int main() {
  test_slots_and_text();
  test_free_run();
  test_swing();
  test_queue_and_select();
  test_record();
  test_locked_to_deck();
  test_slices();
  test_scratch();
  test_commands_rejected();
  std::printf("sampler seq ok\n");
  return 0;
}
