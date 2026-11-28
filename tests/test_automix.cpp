// Automix against the real Engine, rendered offline (no device, no real-time waiting): the state machine is ticked
// between render blocks of 256 frames. Tracks are synthetic click tracks with a known tempo and grid.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/app/automix.hpp"
#include "deckhand/audio/engine.hpp"

using namespace deckhand;
using automix::BeatGrid;

namespace {

constexpr double kSr = 44100;
constexpr std::uint32_t kBlock = 256;

struct Tk {
  std::int64_t id = 0;
  double bpm = 120, first_ms = 100, dur_s = 60;
  bool grid = true;
  bool fail = false;
  AutomixPoints points;
};

struct Sim final : AutomixHost {
  Engine eng;
  std::map<std::int64_t, Tk> tracks;
  Library* links = nullptr;  // set: the host applies linked mix points from this library (the adapter's code)
  PlayQueue queue;
  std::unique_ptr<AutomixService> svc;
  std::vector<AutomixEvent> events;
  std::vector<EngineEvent> fired;
  std::vector<std::int64_t> loads;
  std::vector<std::int64_t> source;  // endless source
  std::map<int, std::int64_t> failed;

  struct Sample {
    std::uint64_t clock;
    DeckView d[2];
    MixerView mx;
  };
  std::vector<Sample> samples;

  explicit Sim(Library* lib = nullptr) : links(lib), queue(lib) {
    eng.configure_offline(44100, 2);
    svc = std::make_unique<AutomixService>(*this, queue, nullptr);
    svc->set_listener([this](const AutomixEvent& e) { events.push_back(e); });
  }

  Tk& add(std::int64_t id, double bpm, double first_ms = 100, double dur_s = 60) {
    Tk& t = tracks[id];
    t.id = id;
    t.bpm = bpm;
    t.first_ms = first_ms;
    t.dur_s = dur_s;
    return t;
  }

  IEngine& engine() override { return eng; }
  TrackFacts facts(std::int64_t id) override {
    TrackFacts f;
    const auto it = tracks.find(id);
    if (it == tracks.end()) return f;
    const Tk& t = it->second;
    f.ok = true;
    f.id = id;
    f.bpm = t.grid ? t.bpm : 0;
    if (t.grid) f.grid.push_back({t.first_ms, t.bpm, 1});
    f.duration_ms = t.dur_s * 1000;
    f.cues.load_cue_ms = t.grid ? t.first_ms : 0;
    f.cues.last_sound_ms = f.duration_ms;
    f.points = t.points;
    if (links) apply_mix_links(*links, eng, f);
    return f;
  }
  bool request_load(int deck, std::int64_t id) override {
    loads.push_back(id);
    const Tk& t = tracks.at(id);
    if (t.fail) {
      failed[deck] = id;
      return true;
    }
    auto audio = std::make_shared<TrackAudio>();
    audio->sample_rate = 44100;
    audio->frames = static_cast<std::uint64_t>(t.dur_s * kSr);
    audio->pcm.assign(audio->frames * 2, 0.0f);
    const double bf = 60.0 / t.bpm * kSr;
    const double fb = t.first_ms / 1000 * kSr;
    for (double b = fb; b < static_cast<double>(audio->frames) - 500 && t.grid; b += bf) {
      const std::size_t s = static_cast<std::size_t>(b);
      for (int k = 0; k < 400; ++k) {
        const float v = static_cast<float>(0.7 * std::sin(6.2831853 * 1000.0 * k / kSr) * std::exp(-k / 60.0));
        audio->pcm[(s + static_cast<std::size_t>(k)) * 2] = v;
        audio->pcm[(s + static_cast<std::size_t>(k)) * 2 + 1] = v;
      }
    }
    if (!t.grid)  // a steady tone, no beats
      for (std::size_t i = 0; i < audio->frames; ++i) audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = 0.2f * static_cast<float>(std::sin(i * 0.05));
    build_waveform(*audio);
    auto lt = std::make_unique<LoadedTrack>();
    lt->audio = audio;
    lt->track_id = id;
    lt->bpm = t.grid ? t.bpm : 0;
    if (t.grid) lt->grid.push_back({fb, t.bpm, 1});
    lt->cue_point = t.grid ? fb : 0;
    eng.load(deck, std::move(lt));
    failed.erase(deck);
    return true;
  }
  LoadState load_state(int deck, std::int64_t id) override {
    if (failed.count(deck) && failed[deck] == id) return LoadState::Failed;
    const DeckView v = eng.deck(deck);
    return v.loaded && v.track_id == id ? LoadState::Ready : LoadState::Pending;
  }
  std::vector<std::int64_t> source_tracks(const std::string&) override { return source; }
  std::int64_t now_ms() override { return static_cast<std::int64_t>(eng.clock_seconds() * 1000); }

  // One render block, then a tick; records the state.
  void step() {
    std::vector<float> buf(kBlock * 2);
    eng.render(buf.data(), kBlock);
    EngineEvent e;
    while (eng.poll(e)) {
      if (e.type == EventType::ScheduledFired) fired.push_back(e);
      svc->on_engine_event(e);
    }
    svc->tick();
    eng.collect_garbage();
    Sample s;
    s.clock = eng.clock_frames();
    s.d[0] = eng.deck(0);
    s.d[1] = eng.deck(1);
    s.mx = eng.mixer();
    samples.push_back(s);
  }
  // Renders `seconds`; `each` runs after every block.
  void run(double seconds, const std::function<void(Sim&)>& each = nullptr) {
    const std::uint64_t end = eng.clock_frames() + static_cast<std::uint64_t>(seconds * kSr);
    while (eng.clock_frames() < end) {
      step();
      if (each) each(*this);
    }
  }
  // Renders until `done` is true (checked after every block) or `max_seconds` have passed.
  bool run_until(double max_seconds, const std::function<bool(Sim&)>& done) {
    const std::uint64_t end = eng.clock_frames() + static_cast<std::uint64_t>(max_seconds * kSr);
    while (eng.clock_frames() < end) {
      step();
      if (done(*this)) return true;
    }
    return false;
  }
  int count_events(const char* type) const {
    int n = 0;
    for (const AutomixEvent& e : events) n += e.type == type;
    return n;
  }
  const AutomixEvent* find_event(const char* type, int nth = 0) const {
    for (const AutomixEvent& e : events)
      if (e.type == type && nth-- == 0) return &e;
    return nullptr;
  }
};

BeatGrid grid_of(const Tk& t) { return BeatGrid::make({{t.first_ms, t.bpm, 1}}, t.bpm, kSr); }
double wrap_half(double x) { return x - std::floor(x + 0.5); }

// Phase offset of deck b's beat grid against deck a's at sample index `i`, in output samples.
double misalignment(const Sim::Sample& s, const Tk& ta, const Tk& tb, int a, int b) {
  const BeatGrid ga = grid_of(ta), gb = grid_of(tb);
  const double pa = ga.frame_to_beat(s.d[a].position), pb = gb.frame_to_beat(s.d[b].position);
  const double period = ga.beat_frames_at(s.d[a].position) / s.d[a].speed;  // output samples per beat
  return wrap_half(pb - pa) * period;
}

// First sample index at which deck `b` plays (after `from`).
int first_playing(const Sim& sim, int b, int from = 0) {
  for (std::size_t i = static_cast<std::size_t>(from); i < sim.samples.size(); ++i)
    if (sim.samples[i].d[b].playing) return static_cast<int>(i);
  return -1;
}

AutomixSettings base_settings() {
  AutomixSettings s;
  s.transition_beats = 8;
  s.mix_out_bars = 4;
  return s;
}

void start(Sim& sim) {
  std::string err;
  CHECK(sim.svc->start(err));
}

// ---- 1. a synced crossfade: bar start, beat alignment, curve, tempo ramp, bass swap ----------------------------------
double g_worst_align = 0;

void check_crossfade(double bpm_a, double bpm_b, double first_a, double first_b, int block_settle_ms = 60) {
  Sim sim;
  sim.add(1, bpm_a, first_a, 40).points.mix_out_ms = 24000;
  sim.add(2, bpm_b, first_b, 40).points.mix_out_ms = 24000;
  sim.add(3, 120, 100, 40);
  sim.svc->set_settings(base_settings());
  sim.queue.add_end({1, 2, 3});
  start(sim);
  CHECK(sim.run_until(60, [](Sim& s) { return s.count_events("automix.transition_end") >= 1; }));

  const Tk& ta = sim.tracks[1];
  const Tk& tb = sim.tracks[2];
  const BeatGrid ga = grid_of(ta);
  // the incoming track's start fired on a bar line of the outgoing one
  const EngineEvent* play_b = nullptr;
  for (const EngineEvent& e : sim.fired)
    if (e.deck == 1 && e.slot == static_cast<int>(Op::PlayToggle)) { play_b = &e; break; }
  CHECK(play_b != nullptr);
  const double beat = ga.frame_to_beat(play_b->frame);
  const double off_samples = (beat - std::round(beat)) * ga.beat_frames_at(play_b->frame);
  CHECK(std::fabs(off_samples) < 3.0);
  CHECK(ga.bar_beat(std::round(beat)) == 1);
  // mix-out 24 s, 8 beats: the transition runs [mix-out - 8 beats, mix-out], starting on the bar at or before it
  const double expect_start = 24000 / 1000.0 * kSr - 8 * ga.beat_frames_at(0);
  CHECK(std::fabs(play_b->frame - expect_start) <= 2 * ga.beat_frames_at(0) + 3);  // the nearest bar line

  // beat alignment, measured once the incoming deck's speed has settled
  const int ib = first_playing(sim, 1);
  CHECK(ib > 0);
  const int settle = ib + static_cast<int>(block_settle_ms * kSr / 1000 / kBlock) + 1;
  const double mis = misalignment(sim.samples[static_cast<std::size_t>(settle)], ta, tb, 0, 1);
  g_worst_align = std::max(g_worst_align, std::fabs(mis));
  std::printf("automix: %.0f -> %.0f BPM (first beats %.0f / %.0f ms): incoming beat off by %.2f samples, start off the bar by %.2f samples\n",
              bpm_a, bpm_b, first_a, first_b, mis, off_samples);
  CHECK(std::fabs(mis) < 3.0);

  // the curve: equal power, gains from the faders (the engine's gain is fader squared)
  const double bf = ga.beat_frames_at(0);
  int checked = 0;
  double tempo_at_start = 0, bass_b_start = -1, bass_b_end = -1;
  // The moves are engine ramps, not steps: the gain tracks the ideal curve within a block or two, and no block moves it
  // by more than the curve's own slope allows (the old quarter-beat staircase moved it 3-4 times that at the steep end).
  const double max_step = 1.5707963 * kBlock / (8 * bf) * 1.25 + 2e-3;
  double prev_ga = -1, prev_gb = -1, worst_env = 0, worst_step = 0;
  for (const Sim::Sample& s : sim.samples) {
    const double p = (ga.frame_to_beat(s.d[0].position) - std::round(beat));
    if (s.d[1].playing && p > 0.5 && p < 7.5) {
      const double t = p / 8;
      const double ga_gain = s.d[0].fader * s.d[0].fader, gb_gain = s.d[1].fader * s.d[1].fader;
      worst_env = std::max(worst_env, std::max(std::fabs(ga_gain - std::cos(t * 1.5707963)), std::fabs(gb_gain - std::sin(t * 1.5707963))));
      if (prev_ga >= 0) worst_step = std::max(worst_step, std::max(std::fabs(ga_gain - prev_ga), std::fabs(gb_gain - prev_gb)));
      prev_ga = ga_gain;
      prev_gb = gb_gain;
      CHECK(std::fabs(ga_gain * ga_gain + gb_gain * gb_gain - 1.0) < 0.06);
      ++checked;
      if (bass_b_start < 0 && p < 1.5) bass_b_start = s.d[1].eq_low;
      if (p > 5.5) bass_b_end = s.d[1].eq_low;
      if (p > 0.6 && p < 1.0 && tempo_at_start == 0) tempo_at_start = s.d[1].tempo;
    }
  }
  CHECK(checked > 100);
  CHECK(worst_env < 0.03);
  CHECK(worst_step < max_step);
  std::printf("automix: envelope off the ideal curve by at most %.4f, largest block step %.4f (bound %.4f)\n", worst_env, worst_step, max_step);
  // bass swap: incoming bass cut at first, restored halfway
  CHECK(bass_b_start < 0.1 && bass_b_end > 0.4);
  // tempo: starts at the outgoing tempo, ends at its own, range restored
  const double ratio = bpm_a / bpm_b;
  CHECK(std::fabs(tempo_at_start - (ratio - 1.0)) < 0.01);
  sim.run(1.0);
  const DeckView b = sim.eng.deck(1), a = sim.eng.deck(0);
  CHECK(std::fabs(b.tempo) < 1e-6 && std::fabs(b.tempo_range - 0.06) < 1e-6);
  std::printf("a.playing=%d b.playing=%d a.eq=%g b.eq=%g\n", a.playing, b.playing, a.eq_low, b.eq_low);
  CHECK(!a.playing && b.playing && std::fabs(a.eq_low - 0.5) < 1e-6 && std::fabs(b.eq_low - 0.5) < 1e-6);
  CHECK(std::fabs(b.fader - 1.0) < 1e-3);
  const AutomixStatus st = sim.svc->status();
  CHECK(st.current_deck == 1 && st.current_track == 2 && st.state != "transition");
  CHECK(sim.queue.current() && sim.queue.current()->track_id == 2 && sim.queue.history(10).size() == 2);
  const AutomixEvent* ts = sim.find_event("automix.transition_start");
  CHECK(ts && ts->text.at("type") == "crossfade" && ts->num.at("synced") == 1.0);
}

void test_crossfade() {
  check_crossfade(120, 120, 100, 100);
  check_crossfade(120, 126, 123, 371);   // +5%: the incoming tempo starts at the outgoing one and ramps back
  check_crossfade(128, 124, 57, 233);
  check_crossfade(100, 107, 10, 480);    // 7%, awkward first beats
  std::printf("automix: worst beat misalignment over the cases: %.2f samples\n", g_worst_align);
}

// ---- 2. a big tempo gap never syncs -----------------------------------------------------------------------------------------
void test_gap_not_synced() {
  for (const char* fallback : {"cut", "echo_out", "fade"}) {
    Sim sim;
    sim.add(1, 120, 100, 40).points.mix_out_ms = 24000;
    sim.add(2, 140, 100, 40).points.mix_out_ms = 24000;  // 16.7% away
    AutomixSettings st = base_settings();
    st.unsynced_type = fallback;
    sim.svc->set_settings(st);
    sim.queue.add_end({1, 2});
    start(sim);
    double min_tempo = 0, max_tempo = 0, min_eq = 1;
    CHECK(sim.run_until(60, [&](Sim& s) {
      const DeckView b = s.eng.deck(1);
      min_tempo = std::min(min_tempo, b.tempo);
      max_tempo = std::max(max_tempo, b.tempo);
      if (b.playing) min_eq = std::min<double>(min_eq, b.eq_low);
      return s.count_events("automix.transition_end") >= 1;
    }));
    const AutomixEvent* ts = sim.find_event("automix.transition_start");
    CHECK(ts && ts->text.at("type") == fallback && ts->num.at("synced") == 0.0);
    CHECK(std::fabs(min_tempo) < 1e-9 && std::fabs(max_tempo) < 1e-9);  // its own tempo throughout
    CHECK(min_eq > 0.45);                                               // no bass swap
    const BeatGrid ga = grid_of(sim.tracks[1]);
    for (const EngineEvent& e : sim.fired)
      if (e.deck == 1 && e.slot == static_cast<int>(Op::PlayToggle)) {
        const double beat = ga.frame_to_beat(e.frame);
        CHECK(std::fabs(beat - std::round(beat)) * ga.beat_frames_at(0) < 3.0 || std::string(fallback) == "fade");
        if (std::string(fallback) != "fade") CHECK(ga.bar_beat(std::round(beat)) == 1);
        break;
      }
    sim.run(2.0);
    CHECK(!sim.eng.deck(0).playing && sim.eng.deck(1).playing);
    CHECK(sim.eng.deck(0).fx[kFxSlots - 1].kind == EffectKind::None);  // the echo is taken off again
    std::printf("automix: 16.7%% gap -> %s, not synced\n", fallback);
  }
}

// ---- 3. mix-out, max play time, per-entry overrides ------------------------------------------------------------------------------
double transition_start_seconds(Sim& sim, int deck_in = 1) {
  for (const EngineEvent& e : sim.fired)
    if (e.deck == deck_in && e.slot == static_cast<int>(Op::PlayToggle)) return static_cast<double>(e.clock_frame) / kSr;
  return -1;
}

void test_points() {
  {  // default mix-out: N bars before the last audible sound
    Sim sim;
    sim.add(1, 120, 100, 60);  // bar = 2 s
    sim.add(2, 120, 100, 60);
    AutomixSettings st = base_settings();
    st.mix_out_bars = 8;  // 16 s before the end: mix-out at 44 s
    sim.svc->set_settings(st);
    const automix::ResolvedTrack r = automix::resolve_track(sim.facts(1), AutomixPoints{}, st);
    CHECK(std::fabs(r.mix_out_ms - 44000) < 1 && std::fabs(r.mix_in_ms - 100) < 1 && r.mix_out_automatic);
    // a user main cue wins over the analysis load cue for the mix-in; explicit points win over both
    TrackFacts f = sim.facts(1);
    f.cues.main_cue_ms = 5000;
    CHECK(std::fabs(automix::resolve_track(f, AutomixPoints{}, st).mix_in_ms - 5000) < 1e-9);
    AutomixPoints ov;
    ov.mix_in_ms = 7000;
    ov.mix_out_ms = 30000;
    const auto r2 = automix::resolve_track(f, ov, st);
    CHECK(r2.mix_in_ms == 7000 && r2.mix_out_ms == 30000 && !r2.mix_out_automatic);
    // max play time caps the mix-out, counted from the mix-in
    ov.max_play_ms = 10000;
    const auto r3 = automix::resolve_track(f, ov, st);
    CHECK(r3.mix_out_ms == 17000 && r3.mix_out_limited_by_max_play);
    // a very short track is not given away at once
    sim.add(9, 120, 100, 20);
    st.mix_out_bars = 16;
    const auto r4 = automix::resolve_track(sim.facts(9), AutomixPoints{}, st);
    CHECK(r4.mix_out_ms > 10000 && r4.mix_out_ms < 20000);
  }
  {  // the transition starts where mix-out minus the length says, on a bar
    Sim sim;
    sim.add(1, 120, 100, 60).points.mix_out_ms = 30000;
    sim.add(2, 120, 100, 60);
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1, 2});
    start(sim);
    CHECK(sim.run_until(60, [](Sim& s) { return s.count_events("automix.transition_start") >= 1; }));
    const double t = transition_start_seconds(sim);  // 0.1 s into the file is where it began: engine seconds ~ file seconds - 0.1
    std::printf("automix: mix-out 30 s, 8 beats -> B starts at %.2f s of play time\n", t);
    CHECK(t > 25.5 && t < 26.5);  // 30 s - 4 s, snapped to the nearest bar line (2 s bars on a 0.1 s offset grid)
  }
  {  // max play time of the playing entry (override on the queue entry)
    Sim sim;
    sim.add(1, 120, 100, 60);
    sim.add(2, 120, 100, 60);
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1, 2});
    AutomixPoints ov;
    ov.max_play_ms = 12000;
    sim.queue.set_overrides(sim.queue.upcoming()[0].id, ov);
    start(sim);
    CHECK(sim.run_until(60, [](Sim& s) { return s.count_events("automix.transition_end") >= 1; }));
    const double t = transition_start_seconds(sim);
    std::printf("automix: max play 12 s -> B starts at %.2f s\n", t);
    CHECK(t > 7.5 && t < 8.2);  // 12 s - 4 s
    // track 2 (no limit) plays on well beyond that
    CHECK(sim.eng.deck(1).playing);
  }
  {  // per-track transition length and type
    Sim sim;
    Tk& t1 = sim.add(1, 120, 100, 60);
    t1.points.mix_out_ms = 30000;
    t1.points.transition_beats = 16;
    t1.points.transition_type = "cut";
    sim.add(2, 120, 100, 60);
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1, 2});
    start(sim);
    CHECK(sim.run_until(60, [](Sim& s) { return s.count_events("automix.transition_start") >= 1; }));
    CHECK(sim.find_event("automix.transition_start")->text.at("type") == "cut");
    const double t = transition_start_seconds(sim);
    CHECK(t > 29.0 && t < 31.0);  // a cut happens at the mix-out (nearest bar)
  }
}

// ---- 4. the user takes over -----------------------------------------------------------------------------------------------------------
void test_override() {
  for (const char* mode : {"yield", "hands_off"}) {
    Sim sim;
    sim.add(1, 120, 100, 40).points.mix_out_ms = 24000;
    sim.add(2, 120, 100, 40).points.mix_out_ms = 24000;
    AutomixSettings st = base_settings();
    st.override_mode = mode;
    sim.svc->set_settings(st);
    sim.queue.add_end({1, 2});
    start(sim);
    bool touched = false;
    float held_at_end = -1;
    CHECK(sim.run_until(60, [&](Sim& s) {
      const DeckView b = s.eng.deck(1), a = s.eng.deck(0);
      if (!touched && b.playing && a.fader < 0.8f && a.fader > 0.5f) {  // halfway through: the user pushes deck 1's fader up
        Command c;
        c.op = Op::Fader;
        c.deck = 0;
        c.f = 0.95f;
        s.eng.send(c);
        touched = true;
      }
      if (touched && a.playing) held_at_end = a.fader;
      return s.count_events("automix.transition_end") >= 1;
    }));
    CHECK(touched);
    if (std::string(mode) == "yield") {
      CHECK(sim.count_events("automix.override") >= 1);
      CHECK(held_at_end > 0.9f);                       // the fader stayed where the user put it
    } else {
      CHECK(sim.count_events("automix.override") == 0);
      CHECK(held_at_end < 0.5f);                       // the plan carried on regardless
    }
    sim.run(1.0);
    CHECK(sim.eng.deck(1).playing && !sim.eng.deck(0).playing);  // the transition still completed
    CHECK(std::fabs(sim.eng.deck(1).fader - 1.0) < 1e-3);        // the incoming fader finished its curve
    std::printf("automix: user fader touch, %s -> ok\n", mode);
  }
  {  // touching the transport (pausing the outgoing deck) abandons the transition; automix does not fight
    Sim sim;
    sim.add(1, 120, 100, 40).points.mix_out_ms = 24000;
    sim.add(2, 120, 100, 40).points.mix_out_ms = 24000;
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1, 2});
    start(sim);
    bool paused = false;
    std::uint64_t paused_at = 0;
    float held_a = -1, held_b = -1;
    sim.run_until(30, [&](Sim& s) {
      if (!paused && s.eng.deck(1).playing && s.eng.deck(0).fader < 0.8f) {
        Command c;
        c.op = Op::PlayToggle;
        c.deck = 0;
        s.eng.send(c);
        paused = true;
        paused_at = s.eng.clock_frames();
      }
      if (paused && held_a < 0 && s.eng.clock_frames() > paused_at + 44100) {  // abandoned: the engine ramps must not run on
        held_a = s.eng.deck(0).fader;
        held_b = s.eng.deck(1).fader;
      }
      return false;
    });
    CHECK(paused && held_a >= 0);
    CHECK(sim.eng.deck(0).fader == held_a && sim.eng.deck(1).fader == held_b);
    CHECK(!sim.eng.deck(0).playing && sim.eng.deck(1).playing);
    CHECK(sim.count_events("automix.transition_end") == 0 && sim.eng.scheduled_count() == 0);
    CHECK(sim.svc->status().current_deck == 1);
    std::puts("automix: user pauses the outgoing deck -> transition abandoned, nothing left scheduled");
  }
  {  // the crossfader decides the blend once the user moves it
    Sim sim;
    sim.add(1, 120, 100, 40).points.mix_out_ms = 24000;
    sim.add(2, 120, 100, 40).points.mix_out_ms = 24000;
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1, 2});
    start(sim);
    bool moved = false;
    sim.run_until(40, [&](Sim& s) {
      if (!moved && s.eng.deck(1).playing && s.eng.deck(0).fader < 0.8f) {
        Command c;
        c.op = Op::Crossfader;
        c.f = 0.8f;
        s.eng.send(c);
        moved = true;
      }
      return s.count_events("automix.transition_end") >= 1;
    });
    CHECK(moved && sim.count_events("automix.override") >= 1);
    CHECK(std::fabs(sim.eng.deck(0).fader - 1.0) < 1e-3 || !sim.eng.deck(0).playing);
  }
}

// ---- 5. empty queue, failing loads, short tracks, skip, pause, endless -----------------------------------------------------------------------------
void test_empty_and_failures() {
  {  // the only track plays to its end, then automix stops cleanly
    Sim sim;
    sim.add(1, 120, 100, 6);
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1});
    start(sim);
    CHECK(sim.run_until(20, [](Sim& s) { return s.count_events("automix.finished") >= 1; }));
    const AutomixStatus st = sim.svc->status();
    CHECK(!st.running && st.state == "stopped" && !sim.eng.deck(0).playing);
    CHECK(sim.eng.scheduled_count() == 0 && sim.queue.size() == 0 && !sim.queue.current());
    CHECK(sim.queue.history(5).size() == 1);
    std::puts("automix: empty queue -> the last track finishes, automix stops");
  }
  {  // on_empty = fade: the last track fades out at its mix-out
    Sim sim;
    sim.add(1, 120, 100, 40).points.mix_out_ms = 20000;
    AutomixSettings st = base_settings();
    st.on_empty = "fade";
    sim.svc->set_settings(st);
    sim.queue.add_end({1});
    start(sim);
    CHECK(sim.run_until(40, [](Sim& s) { return s.count_events("automix.finished") >= 1; }));
    CHECK(!sim.eng.deck(0).playing && sim.eng.deck(0).position < 21 * kSr);
  }
  {  // a track that fails to load is skipped and the next one plays
    Sim sim;
    sim.add(1, 120, 100, 30).points.mix_out_ms = 14000;
    sim.add(2, 120, 100, 30).fail = true;
    sim.add(3, 120, 100, 30);
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1, 2, 3});
    start(sim);
    CHECK(sim.run_until(60, [](Sim& s) { return s.count_events("automix.transition_end") >= 1; }));
    CHECK(sim.count_events("automix.entry_failed") == 1);
    CHECK(sim.eng.deck(1).track_id == 3 || sim.svc->status().current_track == 3);
    CHECK(sim.svc->status().current_track == 3);
    CHECK(sim.queue.history(10).size() == 2 && sim.queue.history(10)[0].track_id == 3);
    std::puts("automix: a failing load is skipped");
  }
  {  // a failing first track
    Sim sim;
    sim.add(1, 120, 100, 30).fail = true;
    sim.add(2, 120, 100, 30);
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1, 2});
    start(sim);
    sim.run(2);
    CHECK(sim.svc->status().current_track == 2 && sim.eng.deck(0).playing);
  }
  {  // very short tracks and a missing grid: every track still plays, in order
    Sim sim;
    sim.add(1, 120, 100, 14);
    sim.add(2, 125, 100, 9);
    sim.add(3, 0, 0, 12).grid = false;
    sim.add(4, 120, 100, 10);
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1, 2, 3, 4});
    start(sim);
    CHECK(sim.run_until(80, [](Sim& s) { return s.count_events("automix.finished") >= 1; }));
    const auto h = sim.queue.history(10);
    CHECK(h.size() == 4 && h[0].track_id == 4 && h[3].track_id == 1);
    CHECK(sim.count_events("automix.transition_end") == 3 && sim.eng.scheduled_count() == 0);
    std::puts("automix: short and grid-less tracks play through");
  }
}

void test_skip_pause_endless() {
  {  // skip: the next transition starts on the next bar, not at the planned time
    Sim sim;
    sim.add(1, 120, 100, 60);
    sim.add(2, 120, 100, 60);
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1, 2});
    start(sim);
    sim.run(3.0);
    CHECK(sim.count_events("automix.transition_start") == 0);
    CHECK(sim.svc->skip());
    CHECK(sim.run_until(10, [](Sim& s) { return s.count_events("automix.transition_start") >= 1; }));
    const double t = transition_start_seconds(sim);
    std::printf("automix: skip at 3.0 s -> next transition at %.2f s\n", t);
    CHECK(t > 3.5 && t < 6.5);
    const BeatGrid ga = grid_of(sim.tracks[1]);
    for (const EngineEvent& e : sim.fired)
      if (e.deck == 1 && e.slot == static_cast<int>(Op::PlayToggle)) {
        CHECK(ga.bar_beat(std::round(ga.frame_to_beat(e.frame))) == 1);
        break;
      }
  }
  {  // pause: nothing new starts, what plays goes on; resume: the transition happens
    Sim sim;
    sim.add(1, 120, 100, 40).points.mix_out_ms = 12000;
    sim.add(2, 120, 100, 40);
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1, 2});
    start(sim);
    sim.run(2.0);
    sim.svc->pause();
    sim.run(15.0);
    CHECK(sim.count_events("automix.transition_start") == 0 && sim.eng.deck(0).playing && sim.svc->status().paused);
    CHECK(sim.eng.scheduled_count() == 0);
    sim.svc->resume();
    CHECK(sim.run_until(20, [](Sim& s) { return s.count_events("automix.transition_end") >= 1; }));
  }
  {  // stop leaves the decks alone; start again continues from the queue
    Sim sim;
    sim.add(1, 120, 100, 40);
    sim.add(2, 120, 100, 40);
    sim.svc->set_settings(base_settings());
    sim.queue.add_end({1, 2});
    start(sim);
    sim.run(2.0);
    sim.svc->stop();
    sim.run(1.0);
    CHECK(!sim.svc->status().running && sim.eng.deck(0).playing && sim.queue.size() == 1);
    start(sim);
    sim.run(0.5);
    CHECK(sim.svc->status().current_track == 1 && sim.svc->status().running);  // adopted what plays
  }
  {  // endless: the queue is kept topped up from a source
    Sim sim;
    for (int i = 1; i <= 6; ++i) sim.add(i, 120, 100, 40);
    sim.source = {1, 2, 3, 4, 5, 6};
    AutomixSettings st = base_settings();
    st.endless = true;
    st.endless_source = "playlist:1";
    st.endless_keep = 3;
    sim.svc->set_settings(st);
    sim.queue.add_end({1});
    start(sim);
    sim.run(3.0);
    CHECK(sim.queue.size() == 3 && sim.svc->status().current_track == 1);
    CHECK(sim.queue.upcoming()[0].track_id == 1 || sim.queue.upcoming()[0].track_id == 2);
  }
}

}  // namespace


// ---- 7. linked mix points through the same code the application's host uses ----------------------------------------------
void test_links() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_automix_links";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  double start_without = -1, start_with = -1, b_pos_with = -1, b_pos_without = -1;
  for (const bool linked : {false, true}) {
    Library lib((dir / (linked ? "linked.db" : "plain.db")).string());
    const auto add_track = [&](const char* name) {
      NewTrack t;
      t.path = std::string("/music/") + name + ".mp3";
      t.title = name;
      t.bpm = 120;
      t.duration_ms = 60000;
      return lib.upsert_track(t);
    };
    const std::int64_t a = add_track("a"), b = add_track("b");
    if (linked) {
      MixLink l;
      l.from_track = a;
      l.from_ms = 20000;
      l.to_track = b;
      l.to_ms = 10000;
      CHECK(lib.add_mix_link(l) > 0);
    }
    Sim sim(&lib);
    sim.add(a, 120, 100, 60);
    sim.add(b, 120, 100, 60);
    sim.svc->set_settings(base_settings());  // 8 beats; the computed mix-out would be 4 bars (8 s) before the end
    sim.queue.add_end({a, b});
    start(sim);
    CHECK(sim.run_until(80, [](Sim& s) { return s.count_events("automix.transition_start") >= 1; }));
    const double t = transition_start_seconds(sim);
    const int i = first_playing(sim, 1);
    CHECK(i >= 0);
    const DeckView bd = sim.samples[static_cast<std::size_t>(i)].d[1];
    const double pos = bd.position / kSr;
    (linked ? start_with : start_without) = t;
    (linked ? b_pos_with : b_pos_without) = pos;
    CHECK(sim.run_until(80, [](Sim& s) { return s.count_events("automix.transition_end") >= 1; }));
    sim.run(0.5);
  }
  std::printf("automix: unlinked B starts at %.2f s (from %.2f s of B); linked at %.2f s (from %.2f s of B)\n", start_without, b_pos_without,
              start_with, b_pos_with);
  CHECK(start_without > 40.0);                       // the computed mix-out, near the end of a 60 s track
  CHECK(start_with > 15.0 && start_with < 17.0);     // mix-out 20 s, 8 beats (4 s) before it
  CHECK(b_pos_with > 9.5 && b_pos_with < 11.0);      // B comes in from its linked position, not from its start
  CHECK(b_pos_without < 2.0);
  fs::remove_all(dir, ec);
}

int main() {
  test_crossfade();
  test_gap_not_synced();
  test_points();
  test_override();
  test_empty_and_failures();
  test_skip_pause_endless();
  test_links();
  std::puts("test_automix ok");
  return 0;
}
