// Lighting show: the automatic director on a scripted track, safety limits, and the allocation-free render path.
// Everything runs on an injected clock (no sleeping, no threads, no sockets).
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/lighting/engine.hpp"
#include "deckhand/lighting/music.hpp"

using namespace deckhand;
using namespace deckhand::lighting;

// ---- allocation counter (armed only around the render loop) ----
static std::atomic<bool> g_armed{false};
static std::atomic<long> g_allocs{0};
void* operator new(std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr double kDt = 1.0 / 40;

PhraseSection sec(double start_s, double end_s, const char* label, int energy) {
  PhraseSection s;
  s.start_ms = start_s * 1000;
  s.end_ms = end_s * 1000;
  s.label = label;
  s.label_confidence = 0.9;
  s.energy = energy;
  return s;
}

// 120 BPM (a beat every 500 ms, a bar every 2 s): intro 0-16, build 16-32, drop 32-64, breakdown 64-80, drop 80-112, outro 112-128.
struct Track {
  TrackTimeline tl;
  Track() {
    std::vector<double> beats;
    for (int i = 0; i < 256; ++i) beats.push_back(i * 500.0);
    tl.set_track(beats, 0,
                 {sec(0, 16, "intro", 3), sec(16, 32, "build", 6), sec(32, 64, "drop", 9), sec(64, 80, "breakdown", 4),
                  sec(80, 112, "drop", 9), sec(112, 128, "outro", 2)});
  }
};

LightingConfig base_config(const char* profile = "generic-rgb", int n = 4) {
  LightingConfig c;
  c.show.seed = 7;
  for (int i = 0; i < n; ++i) {
    PatchEntry p;
    p.id = "f" + std::to_string(i);
    p.profile = profile;
    p.universe = 1;
    p.address = 1 + i * 16;
    p.position = i;
    c.patch.push_back(p);
  }
  return c;
}

struct Rig {
  LightingEngine eng;
  Track track;
  double t = 0;
  long n = 0;
  bool playing = true;
  std::uint32_t rgb = 0;
  int key = -1;
  explicit Rig(const LightingConfig& c) {
    std::string err;
    if (!eng.configure(c, &err)) { std::fprintf(stderr, "configure: %s\n", err.c_str()); std::exit(1); }
  }
  // One 40 Hz frame: sample the track at the current time, hand it to the engine, render.
  void step() {
    MusicState m;
    track.tl.locate(t * 1000.0, 1.0, &m);
    m.playing = playing;
    m.stamp_s = t;
    m.track_rgb = rgb;
    m.key_index = key;
    eng.set_state(m);
    eng.tick(t);
    t = static_cast<double>(++n) * kDt;  // counted, not accumulated: no drift
  }
  void tick_only() {  // the application stopped sending states
    eng.tick(t);
    t = static_cast<double>(++n) * kDt;
  }
  void run_to(double secs) { while (t < secs - 1e-9) step(); }
  int sum(int fixture) const { return ch(fixture, 0) + ch(fixture, 1) + ch(fixture, 2); }
  std::uint8_t ch(int fixture, int channel) const { return eng.preview()[0].data[static_cast<std::size_t>(fixture * 16 + channel)]; }
};

void test_timeline() {
  Track tr;
  MusicState m;
  tr.tl.locate(16250, 1.0, &m);  // a quarter of a beat into beat 32, in the build
  CHECK(std::fabs(m.beat_pos - 32.5) < 1e-9 && std::fabs(m.bpm - 120) < 1e-9);
  CHECK(m.section_index == 1 && m.section == Section::Build && m.energy == 6);
  CHECK(std::fabs(m.section_elapsed_s - 0.25) < 1e-9 && std::fabs(m.time_to_boundary_s - 15.75) < 1e-9);
  CHECK(m.has_next && m.next_section == Section::Drop && m.next_energy == 9);
  tr.tl.locate(16250, 1.04, &m);  // pitched up 4 %: tempo and times in real seconds follow
  CHECK(std::fabs(m.bpm - 124.8) < 1e-9 && std::fabs(m.time_to_boundary_s - 15.75 / 1.04) < 1e-9);
  tr.tl.locate(127000, 1.0, &m);
  CHECK(m.section == Section::Outro && !m.has_next);
  int num = 0;
  bool minor = false;
  CHECK(camelot_of_key(0, &num, &minor) && num == 8 && !minor);   // C = 8B
  CHECK(camelot_of_key(21, &num, &minor) && num == 8 && minor);   // A minor = 8A
  CHECK(!camelot_of_key(-1, &num, &minor));
  CHECK(section_from_phrase("drop", 0.9, "chorus") == Section::Drop);
  CHECK(section_from_phrase("drop", 0.1, "chorus") == Section::Chorus);  // low confidence: the safe label
}

void test_beats() {
  LightingConfig c = base_config();
  Rig rig(c);
  Override o;
  o.has_scene = true;
  o.scene = SceneId::Pulse;
  rig.eng.set_override(o);
  rig.run_to(3.0);
  // The summed colour is the pulse: strongest on the beat, weaker half a beat later, back up on the next beat.
  int beat_frames = 0;
  for (; rig.t < 3.95;) {
    const int frame = static_cast<int>(std::lround(rig.t / kDt));
    rig.step();
    if (frame % 20 == 0) {  // beats are every 0.5 s = 20 frames
      const int on = rig.sum(1);
      CHECK(on > 0);
      beat_frames++;
    }
    if (frame % 20 == 10) {
      const int off = rig.sum(1);
      CHECK(off < 120);
    }
  }
  CHECK(beat_frames >= 1);
  // Direct comparison on the exact beat versus the following frames.
  Rig r2(c);
  r2.eng.set_override(o);
  r2.run_to(3.0);   // t = 3.0 is a beat
  r2.step();
  const int at_beat = r2.sum(1);
  r2.run_to(3.0 + 0.25);
  r2.step();
  const int mid = r2.sum(1);
  r2.run_to(3.5);
  r2.step();
  const int next_beat = r2.sum(1);
  CHECK(at_beat > mid + 40);
  CHECK(next_beat >= at_beat - 40);
  CHECK(r2.eng.status().show.scene == SceneId::Pulse && r2.eng.status().show.override_active);
}

void test_boundaries_and_anticipation() {
  LightingConfig c = base_config();
  c.show.latency_ms = 100;
  c.show.crossfade_s = 1.5;
  Rig rig(c);
  rig.run_to(15.1);
  CHECK(rig.eng.status().show.section_index == 0);  // soft change: leads by crossfade/2 + latency = 0.85 s
  rig.run_to(15.2);
  CHECK(rig.eng.status().show.section_index == 1 && rig.eng.status().show.section == Section::Build);
  CHECK(rig.eng.status().show.scene == SceneId::BuildRamp);
  rig.run_to(31.85);
  CHECK(rig.eng.status().show.section_index == 1);  // the drop is a hard hit: exactly latency (0.1 s) early
  rig.run_to(31.95);
  const auto st = rig.eng.status().show;
  CHECK(st.section_index == 2 && st.section == Section::Drop && st.hit);
  rig.run_to(33.0);  // 1 s after the drop at 120 BPM = two beats: the blinder (one beat) is over
  CHECK(!rig.eng.status().show.hit);
  // The picture at the drop is an immediate cut, not a fade: crossfade progress is already complete.
  CHECK(rig.eng.status().show.crossfade == 1.0f);
  rig.run_to(63.0);
  CHECK(rig.eng.status().show.section == Section::Drop);  // the breakdown is a soft change: it starts 0.85 s early
  rig.run_to(64.0);
  CHECK(rig.eng.status().show.section == Section::Breakdown);
  CHECK(rig.eng.status().show.scene == SceneId::Calm);
}

void test_strobe_cap_and_disable() {
  // Software strobe (fixture without a strobe channel): count flashes per second in the output frames.
  LightingConfig c = base_config("generic-rgb", 2);
  Rig rig(c);
  Override o;
  o.has_scene = true; o.scene = SceneId::Wash;
  o.has_strobe = true; o.strobe_hz = 50;  // asks for far more than allowed
  rig.eng.set_override(o);
  rig.run_to(5.0);
  int rising = 0, frames = 0;
  bool prev = false;
  for (; rig.t < 7.0;) {
    rig.step();
    const bool on = rig.ch(0, 0) > 0 || rig.ch(0, 1) > 0 || rig.ch(0, 2) > 0;
    if (on && !prev) ++rising;
    prev = on;
    ++frames;
  }
  CHECK(frames == 80);
  CHECK(rising >= 15 && rising <= 21);  // 10 Hz cap over 2 s = 20 flashes
  CHECK(rig.eng.status().show.strobe_hz <= 10.0f);

  // Lowering the cap lowers the rate; disabling strobes removes every flash.
  rig.eng.set_strobe(true, 4.0f);
  rig.run_to(8.0);
  rising = 0;
  prev = false;
  for (; rig.t < 10.0;) {
    rig.step();
    const bool on = rig.ch(0, 0) > 0;
    if (on && !prev) ++rising;
    prev = on;
  }
  CHECK(rising >= 7 && rising <= 9);
  rig.eng.set_strobe(false, 10.0f);
  rig.run_to(11.0);
  int off_frames = 0;
  for (; rig.t < 13.0;) {
    rig.step();
    if (rig.ch(0, 0) == 0) ++off_frames;
  }
  CHECK(off_frames == 0 && rig.eng.status().show.strobe_hz == 0);

  // Hardware strobe channel: the value follows the capped rate, never the requested one.
  LightingConfig h = base_config("generic-rgba-par", 1);
  Rig hr(h);
  hr.eng.set_override(o);
  hr.run_to(2.0);
  const int at_cap = hr.eng.preview()[0].data[5];
  Look l;
  l.strobe_hz = 10; l.intensity = 1; l.r = 1;
  std::uint8_t ref[6];
  map_look(builtin_profiles()[3], l, ref);
  CHECK(at_cap == ref[5] && at_cap > 0 && at_cap < 240);
  hr.eng.set_strobe(false, 10);
  hr.run_to(2.5);
  CHECK(hr.eng.preview()[0].data[5] == 0);
}

void test_stop_blackout_hold_master() {
  LightingConfig c = base_config();
  Rig rig(c);
  rig.run_to(10.0);
  int lit = 0;
  for (int i = 0; i < 4; ++i) lit += rig.ch(i, 0) + rig.ch(i, 1) + rig.ch(i, 2);
  CHECK(lit > 0);
  // Stop: fades to black within the stop fade (0.6 s default), then stays black.
  rig.playing = false;
  rig.run_to(10.0 + 0.7);
  for (int i = 0; i < 4; ++i) CHECK(rig.ch(i, 0) == 0 && rig.ch(i, 1) == 0 && rig.ch(i, 2) == 0);
  CHECK(!rig.eng.status().show.active && rig.eng.status().show.scene == SceneId::Blackout);
  // Playing again brings the show back.
  rig.playing = true;
  rig.run_to(14.0);
  int lit2 = 0;
  for (int i = 0; i < 4; ++i) lit2 += rig.ch(i, 0) + rig.ch(i, 1) + rig.ch(i, 2);
  CHECK(lit2 > 0);
  // Blackout command: the very next frame is dark; releasing restores.
  rig.eng.set_blackout(true);
  rig.step();
  for (int i = 0; i < 4; ++i) CHECK(rig.ch(i, 0) == 0 && rig.ch(i, 1) == 0 && rig.ch(i, 2) == 0);
  rig.eng.set_blackout(false);
  rig.run_to(16.0);
  CHECK(rig.ch(0, 0) + rig.ch(0, 1) + rig.ch(0, 2) + rig.ch(1, 0) + rig.ch(1, 1) + rig.ch(1, 2) > 0);
  // Master dimmer scales everything; 0 = dark.
  Override o;
  o.has_scene = true; o.scene = SceneId::Wash; o.has_colour = true; o.colour = {1, 1, 1};
  o.has_intensity = true; o.intensity = 1;
  rig.eng.set_override(o);
  rig.eng.set_master(1.0f);
  rig.run_to(18.0);
  const int full = rig.ch(0, 0);
  CHECK(full > 150);
  rig.eng.set_master(0.5f);
  rig.step();
  CHECK(std::abs(rig.ch(0, 0) - full / 2) <= 2);
  rig.eng.set_master(0);
  rig.step();
  CHECK(rig.ch(0, 0) == 0);
  rig.eng.set_master(1.0f);
  // Hold look: frames freeze while the music moves on; releasing resumes. Override gone: director back in charge.
  rig.eng.release_override();
  rig.step();
  CHECK(!rig.eng.status().show.override_active);
  rig.run_to(20.0);
  rig.eng.set_hold(true);
  rig.step();
  const auto held = rig.eng.preview()[0].data;
  for (int i = 0; i < 60; ++i) rig.step();
  CHECK(rig.eng.preview()[0].data == held && rig.eng.status().show.hold);
  rig.eng.set_hold(false);
  rig.run_to(rig.t + 0.5);
  CHECK(rig.eng.preview()[0].data != held);
  // Missing updates (the app froze): treated as stopped after the stale time.
  const double stop_at = rig.t;
  while (rig.t < stop_at + 4.0) rig.tick_only();  // ticks continue, set_state does not
  CHECK(!rig.eng.status().show.active && rig.ch(0, 0) == 0 && rig.ch(1, 1) == 0);
}

void test_override_layer() {
  LightingConfig c = base_config("generic-rgbw", 2);
  Rig rig(c);
  rig.run_to(20.0);
  Override o;
  o.has_scene = true; o.scene = SceneId::Wash;
  o.has_colour = true; o.colour = {1, 0, 0};
  o.has_intensity = true; o.intensity = 1;
  rig.eng.set_override(o);
  rig.run_to(21.0);
  CHECK(rig.eng.status().show.scene == SceneId::Wash && rig.eng.status().show.override_active);
  // Wash at base intensity 1 * 0.8, pure red: R channel 204, others zero (RGBW: no white component).
  CHECK(rig.ch(0, 0) == 204 && rig.ch(0, 1) == 0 && rig.ch(0, 2) == 0 && rig.ch(0, 3) == 0);
  rig.eng.release_override();
  rig.run_to(23.0);
  CHECK(!rig.eng.status().show.override_active && rig.eng.status().show.scene != SceneId::Wash);  // the build is on (BuildRamp)
}

void test_determinism_and_variety() {
  auto run = [](std::uint64_t seed, std::vector<std::uint64_t>* hashes, std::vector<int>* scenes) {
    LightingConfig c = base_config("generic-rgbw", 5);
    c.show.seed = seed;
    Rig rig(c);
    std::uint64_t h = 1469598103934665603ull;
    int last_idx = -2;
    while (rig.t < 128.0) {
      rig.step();
      for (const auto& f : rig.eng.preview())
        for (int i = 0; i < 100; ++i) { h ^= f.data[static_cast<std::size_t>(i)]; h *= 1099511628211ull; }
      const auto st = rig.eng.status().show;
      if (st.section_index != last_idx) { last_idx = st.section_index; scenes->push_back(static_cast<int>(st.scene) * 10 + static_cast<int>(st.move)); }
    }
    hashes->push_back(h);
  };
  std::vector<std::uint64_t> h1, h2, h3;
  std::vector<int> s1, s2;
  run(42, &h1, &s1);
  run(42, &h2, &s2);
  CHECK(h1 == h2 && s1 == s2);  // same seed, same track: bit identical output
  std::set<std::vector<int>> choices;
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    std::vector<int> s;
    run(seed, &h3, &s);
    choices.insert(s);
  }
  CHECK(choices.size() >= 3);  // the seed really varies the show
  // The structure rules hold for every seed: builds ramp, breakdowns are calm.
  for (const auto& s : choices) {
    CHECK(s.size() == 6);
    CHECK(s[1] / 10 == static_cast<int>(SceneId::BuildRamp));
    CHECK(s[3] / 10 == static_cast<int>(SceneId::Calm));
  }
}

void test_build_ramp_and_palette() {
  // The build chases faster toward the boundary: the lit fixture moves further per second late than early.
  LightingConfig c = base_config("generic-rgb", 8);
  c.show.latency_ms = 0;
  Rig rig(c);
  auto head_moves = [&](double from, double to) {
    rig.run_to(from);
    int moves = 0, prev = -1;
    while (rig.t < to) {
      rig.step();
      int best = 0, bv = -1;
      for (int i = 0; i < 8; ++i) {
        const int v = rig.ch(i, 0) + rig.ch(i, 1) + rig.ch(i, 2);
        if (v > bv) { bv = v; best = i; }
      }
      if (prev >= 0 && best != prev) ++moves;
      prev = best;
    }
    return moves;
  };
  const int early = head_moves(17.0, 19.0);
  const int late = head_moves(29.0, 31.0);
  CHECK(late > early);

  // Palettes: deterministic from the track's colour tag and key.
  ShowEngine a, b;
  CompiledPatch cp;
  std::vector<Issue> iss;
  compile_patch(base_config().patch, ProfileSet::with_builtins(), &cp, &iss);
  ShowConfig sc;
  sc.palette.mode = PaletteMode::Track;
  a.configure(sc, cp);
  b.configure(sc, cp);
  MusicState m;
  m.track_rgb = 0xff0000;
  a.set_music(m);
  b.set_music(m);
  CHECK(a.palette_colour(0).r > 0.95f && a.palette_colour(0).g < 0.1f && a.palette_colour(0).b < 0.1f);
  CHECK(a.palette_colour(2).r < 0.2f && a.palette_colour(2).g > 0.9f);  // the complement of red
  CHECK(a.palette_colour(1).r == b.palette_colour(1).r && a.palette_colour(1).g == b.palette_colour(1).g);
  m.track_rgb = 0;
  m.key_index = 0;  // C major = 8B
  a.set_music(m);
  const Rgb c8 = a.palette_colour(0);
  m.key_index = 7;  // G major = 9B, a different hue
  a.set_music(m);
  const Rgb c9 = a.palette_colour(0);
  CHECK(std::fabs(c8.r - c9.r) + std::fabs(c8.g - c9.g) + std::fabs(c8.b - c9.b) > 0.2f);
  m.key_index = 0;
  b.set_music(m);
  CHECK(b.palette_colour(0).r == c8.r && b.palette_colour(0).g == c8.g && b.palette_colour(0).b == c8.b);
  // User mode ignores both.
  sc.palette.mode = PaletteMode::User;
  sc.palette.colours = {{0, 1, 0}};
  a.set_config(sc);
  CHECK(a.palette_colour(0).g == 1.0f && a.palette_colour(3).g == 1.0f);
}

void test_moving_head_and_unpatched() {
  LightingConfig c;
  c.patch = {{"mh", "generic-moving-head", 1, 1, {}, 0}, {"mh2", "generic-moving-head", 1, 20, {}, 1}};
  Rig rig(c);
  Override o;
  o.has_scene = true; o.scene = SceneId::Wash;
  o.has_move = true; o.move = MoveId::Circle;
  rig.eng.set_override(o);
  rig.run_to(3.0);
  std::set<int> pans;
  for (; rig.t < 7.0;) {
    rig.step();
    pans.insert(rig.eng.preview()[0].data[0]);
  }
  CHECK(pans.size() > 10);  // the pan channel really moves
  for (int i = 17; i < 19; ++i) CHECK(rig.eng.preview()[0].data[static_cast<std::size_t>(i)] == 0);  // between fixtures: untouched
}

void test_no_allocation() {
  LightingConfig c = base_config("generic-moving-head", 3);
  c.patch.push_back({"par", "generic-rgba-par", 2, 1, {"front"}, 3});
  c.patch.push_back({"rgb", "generic-rgb", 2, 20, {"back"}, 4});
  c.show.palette.mode = PaletteMode::Track;
  c.patch[0].address = 1;
  c.patch[1].address = 20;
  c.patch[2].address = 40;
  Rig rig(c);
  auto cap = std::make_shared<CaptureOutput>();
  rig.eng.attach_output(cap);
  std::string err;
  CHECK(rig.eng.start(&err, false));
  rig.rgb = 0x3366ff;
  rig.run_to(15.0);  // warm up: palette, first fades, first send
  g_allocs = 0;
  g_armed = true;
  rig.run_to(75.0);  // crosses build, drop (hit), breakdown
  g_armed = false;
  CHECK(g_allocs.load() == 0);
  CHECK(cap->send_calls > 100);
  rig.eng.stop();
}

}  // namespace

int main() {
  test_timeline();
  test_beats();
  test_boundaries_and_anticipation();
  test_strobe_cap_and_disable();
  test_stop_blackout_hold_master();
  test_override_layer();
  test_determinism_and_variety();
  test_build_ramp_and_palette();
  test_moving_head_and_unpatched();
  test_no_allocation();
  std::puts("test_lighting_show: ok");
  return 0;
}
