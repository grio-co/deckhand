// Sample-accurate scheduled commands: clock, deck-position and beat triggers, cancel, capacity, benchmark.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"

using namespace deckhand;

namespace {

constexpr int kCh = 2;

// Constant 0.5 level: the first audible sample of a started deck is non-zero (a sine would start at 0).
std::unique_ptr<LoadedTrack> flat_track(double seconds, std::vector<GridSection> grid = {{0, 120, 1}}, double bpm = 120) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = 44100;
  audio->frames = static_cast<std::uint64_t>(seconds * 44100);
  audio->pcm.assign(audio->frames * 2, 0.5f);
  build_waveform(*audio);
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->bpm = bpm;
  t->grid = std::move(grid);
  return t;
}

Command cmd(Op op, int deck, float f = 0, int i = 0, double d = 0) {
  Command c;
  c.op = op;
  c.deck = static_cast<std::uint8_t>(deck);
  c.f = f;
  c.i = i;
  c.d = d;
  return c;
}

struct Events {
  std::vector<EngineEvent> fired, dropped;
  void poll(Engine& e) {
    EngineEvent ev;
    while (e.poll(ev)) {
      if (ev.type == EventType::ScheduledFired) fired.push_back(ev);
      if (ev.type == EventType::ScheduledDropped) dropped.push_back(ev);
    }
  }
};

// Render `frames` in callbacks of `block` frames; appends master-left samples to `left`.
void render_blocks(Engine& e, std::uint32_t frames, std::uint32_t block, std::vector<float>* left = nullptr,
                   Events* ev = nullptr) {
  std::vector<float> buf(static_cast<std::size_t>(block) * kCh);
  std::uint32_t done = 0;
  while (done < frames) {
    const std::uint32_t n = std::min(block, frames - done);
    e.render(buf.data(), n);
    if (left)
      for (std::uint32_t i = 0; i < n; ++i) left->push_back(buf[static_cast<std::size_t>(i) * kCh]);
    if (ev) ev->poll(e);
    done += n;
  }
}

long first_nonzero(const std::vector<float>& v) {
  for (std::size_t i = 0; i < v.size(); ++i)
    if (std::fabs(v[i]) > 1e-7f) return static_cast<long>(i);
  return -1;
}

// ---- 1. clock-frame triggers -------------------------------------------------------------------------
void test_clock_exact() {
  for (std::uint32_t block : {64u, 256u, 333u, 512u, 1024u, 2048u}) {
    for (std::uint64_t N : {0ull, 1ull, 777ull, 1000ull, 4097ull, 12345ull}) {
      Engine e;
      e.configure_offline(44100, kCh);
      e.load(0, flat_track(5));
      render_blocks(e, 100, 100);  // clock now 100; schedule relative to it
      const std::uint64_t at = e.clock_frames() + N;
      const std::uint64_t id = e.schedule(cmd(Op::PlayToggle, 0), Schedule::at_clock_frame(at));
      CHECK(id != 0);
      CHECK(e.scheduled_count() == 1);
      std::vector<float> left;
      Events ev;
      render_blocks(e, 20000, block, &left, &ev);
      CHECK(first_nonzero(left) == static_cast<long>(N));
      CHECK(ev.fired.size() == 1 && ev.fired[0].sched_id == id && ev.fired[0].clock_frame == at);
      CHECK(e.scheduled_count() == 0);
      CHECK(e.clock_frames() == 20100);
    }
  }
  std::puts("sched: clock-frame start exact for 6 block sizes x 6 offsets");
}

// ---- 2. deck-position triggers -----------------------------------------------------------------------
using Setup = std::function<void(Engine&)>;

// Prefix: deck 0 plays silently (fader 0) with `extra` applied; deck 1 (flat) waits.
void common_setup(Engine& e, const Setup& extra) {
  e.configure_offline(44100, kCh);
  e.load(0, flat_track(60));
  e.load(1, flat_track(60));
  e.send(cmd(Op::Fader, 0, 0.0f));
  if (extra) extra(e);
  render_blocks(e, 256, 256);
  e.send(cmd(Op::PlayToggle, 0));
  render_blocks(e, 44100 / 2, 256);
}

// Frames rendered after the prefix until deck 0's playhead first reaches `F`, rendering one frame at a time.
long reference_cross(const Setup& extra, const Setup& after, double F) {
  Engine e;
  common_setup(e, extra);
  if (after) after(e);
  for (long c = 1; c < 400000; ++c) {
    render_blocks(e, 1, 1);
    if (e.deck(0).position >= F) return c;
  }
  return -1;
}

// Sample index (after the prefix) at which the scheduled deck 1 start becomes audible. `frames_after` = extra
// frames to keep rendering; the deck 1 must still be playing (exactly one firing).
long triggered_start(const Setup& extra, const Setup& after, double F, std::uint32_t frames, std::uint32_t block = 256) {
  Engine e;
  common_setup(e, extra);
  if (after) after(e);
  const std::uint64_t id = e.schedule(cmd(Op::PlayToggle, 1), Schedule::at_deck_frame(0, F));
  CHECK(id != 0);
  std::vector<float> left;
  Events ev;
  render_blocks(e, frames, block, &left, &ev);
  CHECK(ev.fired.size() == 1 && ev.fired[0].sched_id == id);
  CHECK(e.deck(1).playing);  // a second firing would have toggled it off again
  CHECK(e.scheduled_count() == 0);
  return first_nonzero(left);
}

double pos_after_prefix(const Setup& extra, const Setup& after) {
  Engine e;
  common_setup(e, extra);
  if (after) after(e);
  return e.deck(0).position;
}

void test_deck_cross() {
  struct Case { const char* name; Setup extra, after; double dist; std::uint32_t frames; };
  std::vector<Case> cases;
  cases.push_back({"tempo 1.0", nullptr, nullptr, 30000.5, 60000});
  cases.push_back({"+8% varispeed", [](Engine& e) { e.send(cmd(Op::TempoRange, 0, 0.08f)); e.send(cmd(Op::Tempo, 0, 1.0f)); },
                   nullptr, 30000.5, 60000});
  cases.push_back({"+8% key lock",
                   [](Engine& e) {
                     e.send(cmd(Op::TempoRange, 0, 0.08f));
                     e.send(cmd(Op::Tempo, 0, 1.0f));
                     e.send(cmd(Op::KeyLock, 0, 0, 1));
                   },
                   nullptr, 30000.5, 60000});
  cases.push_back({"-6.5% + sync-free odd frame", [](Engine& e) { e.send(cmd(Op::TempoRange, 0, 0.1f)); e.send(cmd(Op::Tempo, 0, -0.65f)); },
                   nullptr, 12345.25, 40000});
  // Inside an active 2-beat loop (1 s): the playhead crosses the target on every pass; only the first counts.
  cases.push_back({"inside loop", nullptr, [](Engine& e) { e.send(cmd(Op::BeatLoop, 0, 2.0f)); render_blocks(e, 256, 256); },
                   30000.5, 3 * 44100});
  for (const Case& c : cases) {
    const double F = std::floor(pos_after_prefix(c.extra, c.after) + c.dist) + (c.dist - std::floor(c.dist));
    const long expect = reference_cross(c.extra, c.after, F);
    CHECK(expect > 0);
    for (std::uint32_t block : {256u, 333u, 1024u}) {
      const long got = triggered_start(c.extra, c.after, F, c.frames, block);
      std::printf("sched: deck-position [%s] block %u: crossing at +%ld, command audible at +%ld\n", c.name, block, expect, got);
      CHECK(got == expect);
    }
  }
  std::puts("sched: deck-position trigger is exact to the playhead (key lock: audio content is within the stretcher's +-10 ms search range of the playhead)");
}

void test_deck_jump_rules() {
  Engine e;
  e.configure_offline(44100, kCh);
  e.load(0, flat_track(60));
  e.load(1, flat_track(60));
  e.send(cmd(Op::Fader, 0, 0.0f));
  render_blocks(e, 256, 256);
  Events ev;
  const std::uint64_t id = e.schedule(cmd(Op::PlayToggle, 1), Schedule::at_deck_frame(0, 50000));
  render_blocks(e, 256, 256, nullptr, &ev);
  e.send(cmd(Op::Seek, 0, 0, 0, 60000));  // jump past: must not fire
  render_blocks(e, 256, 256, nullptr, &ev);
  e.send(cmd(Op::PlayToggle, 0));
  render_blocks(e, 44100, 256, nullptr, &ev);
  CHECK(ev.fired.empty() && e.scheduled_count() == 1 && !e.deck(1).playing);
  e.send(cmd(Op::Seek, 0, 0, 0, 10000));  // back behind the target: re-armed, fires on the crossing
  render_blocks(e, 3 * 44100, 256, nullptr, &ev);
  CHECK(ev.fired.size() == 1 && ev.fired[0].sched_id == id && e.deck(1).playing && e.scheduled_count() == 0);
  CHECK(ev.fired[0].frame >= 50000 && ev.fired[0].frame < 50003);

  // fire_if_passed: already past at acceptance -> runs at the start of the next callback.
  e.send(cmd(Op::PlayToggle, 1));  // stop deck 1
  render_blocks(e, 256, 256);
  CHECK(!e.deck(1).playing);
  const double p = e.deck(0).position;
  const std::uint64_t clk = e.clock_frames();
  ev = Events{};
  const std::uint64_t id2 = e.schedule(cmd(Op::PlayToggle, 1), Schedule::at_deck_frame(0, p - 1000, true));
  render_blocks(e, 256, 256, nullptr, &ev);
  CHECK(ev.fired.size() == 1 && ev.fired[0].sched_id == id2 && ev.fired[0].clock_frame == clk && e.deck(1).playing);
  // ... and by a seek jump.
  e.send(cmd(Op::PlayToggle, 1));
  e.send(cmd(Op::Seek, 0, 0, 0, 1000));
  render_blocks(e, 256, 256);
  ev = Events{};
  e.schedule(cmd(Op::PlayToggle, 1), Schedule::at_deck_frame(0, 20000, true));
  render_blocks(e, 256, 256, nullptr, &ev);
  CHECK(ev.fired.empty());
  e.send(cmd(Op::Seek, 0, 0, 0, 40000));
  render_blocks(e, 256, 256, nullptr, &ev);
  CHECK(ev.fired.size() == 1 && e.deck(1).playing);

  // Unloading the trigger deck drops the entry and says so.
  ev = Events{};
  const std::uint64_t id3 = e.schedule(cmd(Op::PlayToggle, 1), Schedule::at_deck_frame(0, 1e9));
  render_blocks(e, 256, 256, nullptr, &ev);
  CHECK(e.scheduled_count() == 1);
  e.send(cmd(Op::Eject, 0));
  render_blocks(e, 256, 256, nullptr, &ev);
  CHECK(e.scheduled_count() == 0 && ev.dropped.size() == 1 && ev.dropped[0].sched_id == id3 &&
        ev.dropped[0].slot == static_cast<int>(DropReason::DeckUnloaded));
  // No track on the trigger deck at acceptance -> dropped as unresolvable.
  ev = Events{};
  e.schedule(cmd(Op::PlayToggle, 1), Schedule::at_deck_beat(0, 4));
  render_blocks(e, 256, 256, nullptr, &ev);
  CHECK(ev.dropped.size() == 1 && ev.dropped[0].slot == static_cast<int>(DropReason::Unresolvable));
  // Pointer commands and bad arguments are rejected.
  Command bad = cmd(Op::Load, 0);
  CHECK(e.schedule(bad, Schedule::at_clock_frame(10)) == 0);
  CHECK(e.schedule(cmd(Op::SamplerLoad, 0), Schedule::at_clock_frame(10)) == 0);
  CHECK(e.schedule(cmd(Op::SchedInsert, 0), Schedule::at_clock_frame(10)) == 0);
  CHECK(e.schedule(cmd(Op::PlayToggle, 0), Schedule::at_deck_frame(9, 10)) == 0);
  CHECK(e.schedule(cmd(Op::PlayToggle, 0), Schedule::beats_from_now(0, 0)) == 0);
  std::puts("sched: jump / re-arm / fire_if_passed / unload / rejection rules ok");
}

// ---- 3. beats ----------------------------------------------------------------------------------------
void test_beats() {
  // One section, first beat at frame 5000, 120 BPM (22050 frames / beat), bar_beat 3 on beat 0.
  const std::vector<GridSection> single = {{5000, 120, 3}};
  auto grid_setup = [&](Engine& e) {
    e.configure_offline(44100, kCh);
    e.load(0, flat_track(60, single));
    e.load(1, flat_track(60));
    e.send(cmd(Op::Fader, 0, 0.0f));
    render_blocks(e, 256, 256);
    e.send(cmd(Op::PlayToggle, 0));
    render_blocks(e, 10000, 256);
  };
  auto run = [&](const Schedule& s, double expect_frame) {
    Engine ref;
    grid_setup(ref);
    long expect = -1;
    for (long c = 1; c < 400000 && expect < 0; ++c) {
      render_blocks(ref, 1, 1);
      if (ref.deck(0).position >= expect_frame) expect = c;
    }
    Engine e;
    grid_setup(e);
    const std::uint64_t id = e.schedule(cmd(Op::PlayToggle, 1), s);
    CHECK(id != 0);
    std::vector<float> left;
    Events ev;
    render_blocks(e, 300000, 256, &left, &ev);
    CHECK(ev.fired.size() == 1);
    const long got = first_nonzero(left);
    std::printf("sched: beat frame %.0f: crossing at +%ld, audible at +%ld\n", expect_frame, expect, got);
    CHECK(got == expect);
  };
  run(Schedule::at_deck_beat(0, 4), 5000 + 4 * 22050.0);
  // bar_beat 3 at beat 0 => bar lines at beats 2, 6, 10 ...; beat 7 aligned moves on to 10.
  run(Schedule::at_deck_beat(0, 7, true), 5000 + 10 * 22050.0);
  run(Schedule::at_deck_beat(0, 6, true), 5000 + 6 * 22050.0);
  {  // beats_from_now: the playhead is at ~10256 + (accept) => inside beat 0
    Engine p;
    grid_setup(p);
    const double pos = p.deck(0).position;
    const double beat0 = std::floor((pos - 5000) / 22050.0);
    run(Schedule::beats_from_now(0, 1), 5000 + (beat0 + 1) * 22050.0);
    run(Schedule::beats_from_now(0, 3), 5000 + (beat0 + 3) * 22050.0);
    const double want = std::ceil(beat0 + 2 - 1e-9);  // next bar line at/after beat0 + 2: beats 2, 6, 10
    double b = want;
    while (std::fmod(b - 2, 4.0) != 0.0) b += 1;
    run(Schedule::beats_from_now(0, 2, true), 5000 + b * 22050.0);
  }
  // Two sections: 120 BPM from 5000, 140 BPM from 5000 + 8 beats (the count continues: beat 8 starts section 2).
  {
    const std::vector<GridSection> multi = {{5000, 120, 1}, {5000 + 8 * 22050.0, 140, 1}};
    auto setup2 = [&](Engine& e) {
      e.configure_offline(44100, kCh);
      e.load(0, flat_track(60, multi));
      e.load(1, flat_track(60));
      e.send(cmd(Op::Fader, 0, 0.0f));
      render_blocks(e, 256, 256);
      e.send(cmd(Op::PlayToggle, 0));
      render_blocks(e, 256, 256);
    };
    const double bf2 = 60.0 / 140 * 44100;
    for (double beat : {3.0, 8.0, 12.0, 16.0}) {
      const double F = beat < 8 ? 5000 + beat * 22050 : 5000 + 8 * 22050.0 + (beat - 8) * bf2;
      Engine ref;
      setup2(ref);
      long expect = -1;
      for (long c = 1; c < 600000 && expect < 0; ++c) {
        render_blocks(ref, 1, 1);
        if (ref.deck(0).position >= F) expect = c;
      }
      Engine e;
      setup2(e);
      CHECK(e.schedule(cmd(Op::PlayToggle, 1), Schedule::at_deck_beat(0, beat)) != 0);
      std::vector<float> left;
      render_blocks(e, 400000, 512, &left);
      std::printf("sched: multi-section beat %.0f (frame %.1f): crossing +%ld, audible +%ld\n", beat, F, expect, first_nonzero(left));
      CHECK(first_nonzero(left) == expect);
    }
  }
  std::puts("sched: beat schedules land frame-exact on the grid");
}

// ---- 4. cancel, ordering, overflow --------------------------------------------------------------------
void test_cancel_order_overflow() {
  {
    Engine e;
    e.configure_offline(44100, kCh);
    Events ev;
    const auto a = e.schedule(cmd(Op::HeadLevel, 0, 0.1f), Schedule::at_clock_frame(1000));
    const auto b = e.schedule(cmd(Op::HeadLevel, 0, 0.2f), Schedule::at_clock_frame(2000));
    const auto c = e.schedule(cmd(Op::HeadLevel, 0, 0.3f), Schedule::at_clock_frame(3000));
    CHECK(a && b && c && e.scheduled_count() == 3);
    CHECK(e.cancel(b));
    CHECK(!e.cancel(0) && !e.cancel(999999));
    render_blocks(e, 4000, 256, nullptr, &ev);
    CHECK(ev.fired.size() == 2 && ev.fired[0].sched_id == a && ev.fired[1].sched_id == c);
    CHECK(ev.fired[0].clock_frame == 1000 && ev.fired[1].clock_frame == 3000);
    CHECK(std::fabs(e.mixer().head_level - 0.3f) < 1e-6f);
    CHECK(!e.cancel(a) && e.scheduled_count() == 0);
    for (int i = 0; i < 10; ++i) e.schedule(cmd(Op::HeadLevel, 0, 0.5f), Schedule::at_clock_frame(50000 + static_cast<unsigned>(i)));
    render_blocks(e, 256, 256);
    CHECK(e.scheduled_count() == 10);
    e.cancel_all();
    render_blocks(e, 256, 256);
    CHECK(e.scheduled_count() == 0);
  }
  {  // 1000 commands at distinct frames, inserted in scrambled order: all fire in clock order on their frame.
    Engine e;
    e.configure_offline(44100, kCh);
    std::vector<std::uint64_t> frames(1000);
    for (std::size_t k = 0; k < frames.size(); ++k) frames[k] = 300 + k * 37;
    std::uint32_t x = 12345;
    for (std::size_t k = frames.size() - 1; k > 0; --k) {  // Fisher-Yates with an LCG
      x = x * 1664525u + 1013904223u;
      std::swap(frames[k], frames[x % (k + 1)]);
    }
    std::vector<std::uint64_t> ids;
    for (std::uint64_t f : frames) {
      const auto id = e.schedule(cmd(Op::HeadLevel, 0, static_cast<float>(f % 100) / 100.0f), Schedule::at_clock_frame(f));
      CHECK(id != 0);
      ids.push_back(id);
    }
    CHECK(e.scheduled_count() == 1000);
    Events ev;
    render_blocks(e, 300 + 1000 * 37 + 500, 256, nullptr, &ev);
    CHECK(ev.fired.size() == 1000 && e.scheduled_count() == 0);
    for (std::size_t k = 0; k < ev.fired.size(); ++k) {
      CHECK(ev.fired[k].clock_frame == 300 + k * 37);
      if (k) CHECK(ev.fired[k].clock_frame > ev.fired[k - 1].clock_frame);
    }
    std::puts("sched: 1000 scheduled commands fired in order, each on its frame");
  }
  {  // overflow: the table holds 1024; the rest is reported as dropped, never undefined
    Engine e;
    e.configure_offline(44100, kCh);
    Events ev;
    for (int k = 0; k < 1000; ++k) CHECK(e.schedule(cmd(Op::HeadLevel, 0, 0.5f), Schedule::at_clock_frame(1000000 + static_cast<unsigned>(k))) != 0);
    render_blocks(e, 256, 256, nullptr, &ev);
    int accepted = 0, refused = 0;
    for (int k = 0; k < 100; ++k) {
      const auto id = e.schedule(cmd(Op::HeadLevel, 0, 0.5f), Schedule::at_clock_frame(2000000 + static_cast<unsigned>(k)));
      (id ? accepted : refused)++;
    }
    CHECK(accepted == 100 && refused == 0);
    render_blocks(e, 256, 256, nullptr, &ev);
    CHECK(ev.dropped.size() == 76);
    for (const auto& d : ev.dropped) CHECK(d.slot == static_cast<int>(DropReason::TableFull));
    CHECK(e.scheduled_count() == 1024);
    // Flooding the command queue without rendering is refused (0), not lost silently.
    int zeros = 0;
    for (int k = 0; k < 2000; ++k)
      if (e.schedule(cmd(Op::HeadLevel, 0, 0.5f), Schedule::at_clock_frame(3000000)) == 0) ++zeros;
    CHECK(zeros > 0);
    render_blocks(e, 256, 256);  // drain the flood (all dropped: table full)
    e.cancel_all();
    render_blocks(e, 2048, 256);
    CHECK(e.scheduled_count() == 0);
    std::puts("sched: table overflow reported (76 dropped of 1100), queue flood refused");
  }
}

// ---- 5. value changes at exact frames -----------------------------------------------------------------
void test_value_changes() {
  struct V { const char* name; Command c; };
  const V vs[] = {{"Crossfader", cmd(Op::Crossfader, 0, 1.0f)},
                  {"EqLow", cmd(Op::EqLow, 0, 0.0f)},
                  {"EqMid", cmd(Op::EqMid, 0, 0.0f)},
                  {"Trim", cmd(Op::Trim, 0, 0.2f)},
                  {"Fader", cmd(Op::Fader, 0, 0.3f)},
                  {"MasterLevel", cmd(Op::MasterLevel, 0, 0.3f)}};
  for (const V& v : vs) {
    for (std::uint64_t N : {20001ull, 20256ull, 23333ull}) {
      auto render = [&](bool with) {
        Engine e;
        e.configure_offline(44100, kCh);
        e.load(0, flat_track(10));
        e.send(cmd(Op::PlayToggle, 0));
        render_blocks(e, 8000, 256);
        if (with) CHECK(e.schedule(v.c, Schedule::at_clock_frame(N)) != 0);
        std::vector<float> left;
        render_blocks(e, 30000 - 8000, 256, &left);
        return left;
      };
      const auto a = render(true), b = render(false);
      long first = -1;
      for (std::size_t i = 0; i < a.size(); ++i)
        if (std::fabs(a[i] - b[i]) > 2e-6f) { first = static_cast<long>(i) + 8000; break; }
      // The new value ramps in from the sample it is applied at: nothing differs before it, and the first
      // difference is that sample or the next.
      CHECK(first == static_cast<long>(N) || first == static_cast<long>(N) + 1);
    }
    std::printf("sched: %s change lands on its frame\n", v.name);
  }
}

// ---- 6. benchmark -------------------------------------------------------------------------------------
double bench(int clock_entries, int deck_entries) {
  Engine e;
  e.configure_offline(44100, kCh);
  e.load(0, flat_track(30));
  e.load(1, flat_track(30));
  e.send(cmd(Op::PlayToggle, 0));
  e.send(cmd(Op::PlayToggle, 1));
  e.send(cmd(Op::BeatLoop, 0, 4.0f));
  e.send(cmd(Op::BeatLoop, 1, 4.0f));
  render_blocks(e, 4096, 256);
  for (int k = 0; k < clock_entries; ++k) e.schedule(cmd(Op::HeadLevel, 0, 0.5f), Schedule::at_clock_frame(100000000ull + static_cast<unsigned>(k) * 100));
  for (int k = 0; k < deck_entries; ++k) e.schedule(cmd(Op::HeadLevel, 0, 0.5f), Schedule::at_deck_frame(0, 1e9 + k));
  render_blocks(e, 256, 256);
  std::vector<float> buf(256 * kCh);
  const int blocks = 4000;
  double best = 1e9;
  for (int rep = 0; rep < 5; ++rep) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < blocks; ++i) e.render(buf.data(), 256);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
    best = std::min(best, us);
  }
  return best;
}

}  // namespace

int main() {
  test_clock_exact();
  test_deck_cross();
  test_deck_jump_rules();
  test_beats();
  test_cancel_order_overflow();
  test_value_changes();
  const double base = bench(0, 0), clk = bench(1000, 0), dk = bench(0, 1000);
  std::printf("benchmark (2 decks, 256-frame block): none %.2f us, 1000 clock entries %.2f us (%+.2f), 1000 deck-position entries %.2f us (%+.2f)\n",
              base, clk, clk - base, dk, dk - base);
  CHECK(clk - base < 5.0 && dk - base < 20.0);
  std::puts("engine sched ok");
  return 0;
}
