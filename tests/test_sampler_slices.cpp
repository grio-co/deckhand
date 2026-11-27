// Sequencer steps that trigger a slice of a sliced slot: pattern text (with "@<slice>", old texts unchanged), the step
// command, playback of exactly that slice, fallback to the whole sample, recording of slice pads. Offline engine only.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"

using namespace deckhand;

namespace {
constexpr double kSr = 44100;
constexpr int kSliceLen = 2000;  // frames per slice of the 8-slice test sample

Command cmd(Op op, int deck = 0, float f = 0, int i = 0, double d = 0) {
  Command c;
  c.op = op;
  c.deck = static_cast<std::uint8_t>(deck);
  c.f = f;
  c.i = i;
  c.d = d;
  return c;
}

// 8 slices, slice j holds the constant level 0.1 * (j + 1).
std::unique_ptr<LoadedSample> stair() {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = 44100;
  audio->frames = kSliceLen * 8;
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i) audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = 0.1f * static_cast<float>(i / kSliceLen + 1);
  auto s = std::make_unique<LoadedSample>();
  s->audio = audio;
  return s;
}

struct Rig {
  Engine e;
  std::vector<float> out;
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
  std::size_t first_above(std::size_t from, float thr = 1e-5f) const {
    for (std::size_t i = from; i < frames(); ++i)
      if (std::fabs(at(i)) > thr) return i;
    return frames();
  }
  // Frames from `a` until the level stays below thr for good.
  std::size_t last_above(std::size_t a, std::size_t b, float thr = 1e-5f) const {
    std::size_t l = a;
    for (std::size_t i = a; i < b && i < frames(); ++i)
      if (std::fabs(at(i)) > thr) l = i;
    return l;
  }
};

void test_text() {
  SeqPatternData p;
  p.cell[0][0] = 0x80 | 127;
  p.slice[0][0] = 4;  // slice 3
  p.cell[0][2] = 0x80 | 64;
  p.cell[5][15] = 0x80 | 10;
  p.slice[5][15] = 16;  // slice 15
  const std::string t = seq_pattern_to_text(p);
  CHECK(t.rfind("dhseq1;0005:7f@340;", 0) == 0);
  CHECK(t.find("8000:0a@f") != std::string::npos);
  SeqPatternData q;
  CHECK(seq_pattern_from_text(t, q));
  CHECK(q.slice[0][0] == 4 && q.slice[0][2] == 0 && q.slice[5][15] == 16 && q.cell[0][2] == (0x80 | 64));
  CHECK(seq_pattern_to_text(q) == t);
  CHECK(seq_pattern_from_text("DHSEQ1;0001:7F@A;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q) == false);  // the head is lower case
  CHECK(seq_pattern_from_text("dhseq1;0001:7F@A;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q) && q.slice[0][0] == 11);
  // saved texts written before slices existed read the same and are written back identically
  const std::string old = "dhseq1;0005:7f40;-;-;-;8000:7f;-;-;-;-;-;-;-;-;-;-;-";
  CHECK(seq_pattern_from_text(old, q) && q.slice[0][0] == 0 && q.slice[0][2] == 0 && q.cell[4][15] == (0x80 | 127));
  CHECK(seq_pattern_to_text(q) == old);
  // refused whole
  CHECK(!seq_pattern_from_text("dhseq1;0001:7f@;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q));
  CHECK(!seq_pattern_from_text("dhseq1;0001:7f@g;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q));
  CHECK(!seq_pattern_from_text("dhseq1;0001:7f@1x;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q));
  CHECK(!seq_pattern_from_text("dhseq1;0003:7f@1;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-", q));  // second velocity missing
}

void test_playback() {
  Rig r;
  r.e.sampler_load(4, stair());
  r.e.sampler_slice(4, 8);
  r.e.seq_bpm(120);
  r.e.seq_step(0, 0, 4, true, 1.0f, 5);   // step 1: slice 5 (level 0.6)
  r.e.seq_step(0, 4, 4, true, 1.0f);      // step 5: the whole sample from its start (level 0.1 first)
  r.e.seq_step(0, 8, 4, true, 1.0f, 0);   // step 9: slice 0
  r.e.seq_step(0, 12, 4, true, 1.0f, 7);  // step 13: slice 7
  r.render(2048);
  CHECK((r.e.step_seq().pattern[0].slice[0][4] == 6) && r.e.step_seq().pattern[0].slice[4][4] == 0);
  CHECK(r.e.seq_pattern_text(0).find("@5") != std::string::npos);
  const std::size_t from = r.frames();
  r.e.seq_run(1);
  r.render(static_cast<std::size_t>(kSr * 2.2));
  const std::size_t step = static_cast<std::size_t>(kSr * 0.125);
  const std::size_t o0 = r.first_above(from);
  CHECK(o0 < r.frames());
  auto level_at = [&](int s, int off) { return std::fabs(r.at(o0 + step * static_cast<std::size_t>(s) * 1 + static_cast<std::size_t>(off))); };
  // the gain of the sampler path is the same for every hit: compare ratios against the slice 0 hit
  const float base = level_at(8, 600);  // slice 0 = 0.1
  CHECK(base > 0.01f);
  std::printf("slice levels: slice5 %.4f whole %.4f slice0 %.4f slice7 %.4f\n", level_at(0, 600), level_at(4, 600), base, level_at(12, 600));
  CHECK(std::fabs(level_at(0, 600) / base - 6.0f) < 0.15f);
  CHECK(std::fabs(level_at(4, 600) / base - 1.0f) < 0.05f);
  CHECK(std::fabs(level_at(12, 600) / base - 8.0f) < 0.2f);
  // a slice plays once and stops at its end; the whole sample runs longer than one slice
  const std::size_t e0 = r.last_above(o0, o0 + step) - o0, e4 = r.last_above(o0 + step * 4, o0 + step * 5) - (o0 + step * 4);
  std::printf("slice length %zu frames, whole sample cut off by the next step at %zu\n", e0, e4);
  CHECK(e0 >= kSliceLen - 4 && e0 <= kSliceLen + 130);
  CHECK(e4 > kSliceLen * 2);
  CHECK(r.e.sampler(4).slice != 0xFF);
}

void test_fallback_and_clear() {
  Rig r;
  r.e.sampler_load(4, stair());  // not sliced: a slice step plays the whole sample
  r.e.seq_bpm(120);
  r.e.seq_step(0, 0, 4, true, 1.0f, 5);
  r.render(1024);
  std::size_t from = r.frames();
  r.e.seq_run(1);
  r.render(8000);
  const std::size_t o = r.first_above(from);
  CHECK(std::fabs(r.at(o + 3000) / r.at(o + 600) - 2.0f) < 0.1f);  // the whole sample: starts at slice 0 (0.1) and climbs to slice 1 (0.2)
  r.e.seq_run(0);
  // a slice beyond the slot's slice count (re-sliced coarser) also plays whole
  r.e.sampler_slice(4, 8);
  r.e.seq_step(1, 0, 4, true, 1.0f, 12);
  r.render(2048);
  CHECK(r.e.step_seq().pattern[1].slice[0][4] == 13);
  // clearing the slot's cells clears its slices; turning a cell off keeps the slice for the next time it is turned on
  r.e.seq_step(1, 0, 4, false);
  r.render(1024);
  CHECK(r.e.step_seq().pattern[1].slice[0][4] == 13);
  r.e.seq_step(1, 0, 4, true, -1.0f, -2);  // -2 = whole sample again
  r.render(1024);
  CHECK(r.e.step_seq().pattern[1].slice[0][4] == 0);
  r.e.seq_step(1, 3, 4, true, 0.5f, 2);
  r.e.seq_clear(1, 4);
  r.render(1024);
  CHECK(r.e.step_seq().pattern[1].slice[3][4] == 0 && r.e.step_seq().pattern[1].cell[3][4] == 0);
  // set from text through the engine
  CHECK(r.e.seq_pattern_set(3, "dhseq1;0010:7f@2;-;-;-;-;-;-;-;-;-;-;-;-;-;-;-"));
  r.render(1024);
  CHECK(r.e.step_seq().pattern[3].slice[0][4] == 3);
}

void test_record() {
  Rig r;
  r.e.sampler_load(4, stair());
  r.e.sampler_slice(4, 8);
  r.e.seq_bpm(120);
  r.e.seq_select(0, false);
  r.e.seq_record(1);
  r.e.seq_run(1);
  r.render(static_cast<std::size_t>(kSr * 0.5));
  r.e.sampler_slice_trigger(4, 3, 1.0f);
  r.render(static_cast<std::size_t>(kSr * 0.4));
  r.e.send(cmd(Op::SamplerTrigger, 4, 1.0f));  // a whole-sample pad written later in the same pattern
  r.render(static_cast<std::size_t>(kSr * 0.6));
  const auto pat = r.e.step_seq().pattern[0];
  int sliced = 0, whole = 0;
  for (int s = 0; s < kSeqSteps; ++s)
    if (pat.cell[s][4] & 0x80) {
      if (pat.slice[s][4] == 4) ++sliced;
      else if (pat.slice[s][4] == 0) ++whole;
    }
  CHECK(sliced == 1 && whole == 1);
}

}  // namespace

int main() {
  test_text();
  test_playback();
  test_fallback_and_clear();
  test_record();
  std::printf("sampler slices ok\n");
  return 0;
}
