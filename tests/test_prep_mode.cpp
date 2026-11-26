// Preparation mode on the offline engine (no device, no sound): refusal while decks 3 / 4 are busy, the borrowed decks
// never reach the master, tempo follow, blend, swap at the next bar, and everything put back on exit.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "check.hpp"
#include "deckhand/app/next_tracks.hpp"
#include "deckhand/app/prep_mode.hpp"
#include "deckhand/audio/engine.hpp"

using namespace deckhand;

namespace {

constexpr double kSr = 44100;
constexpr std::uint32_t kBlock = 256;

struct Tk {
  double bpm = 120, first_ms = 100, dur_s = 60;
};

struct Host final : AutomixHost {
  Engine eng;
  std::map<std::int64_t, Tk> tracks;
  std::vector<EngineEvent> events;
  bool hold_loads = false;  // keep a load pending until release()
  std::vector<std::pair<int, std::int64_t>> held;

  Host() { eng.configure_offline(44100, 2); }

  IEngine& engine() override { return eng; }
  TrackFacts facts(std::int64_t id) override {
    TrackFacts f;
    const auto it = tracks.find(id);
    if (it == tracks.end()) return f;
    f.ok = true;
    f.id = id;
    f.bpm = it->second.bpm;
    f.grid.push_back({it->second.first_ms, it->second.bpm, 1});
    f.duration_ms = it->second.dur_s * 1000;
    return f;
  }
  void do_load(int deck, std::int64_t id) {
    const Tk& t = tracks.at(id);
    auto audio = std::make_shared<TrackAudio>();
    audio->sample_rate = 44100;
    audio->frames = static_cast<std::uint64_t>(t.dur_s * kSr);
    audio->pcm.assign(audio->frames * 2, 0.0f);
    const double bf = 60.0 / t.bpm * kSr, fb = t.first_ms / 1000 * kSr;
    for (double b = fb; b < static_cast<double>(audio->frames) - 500; b += bf) {
      const std::size_t s = static_cast<std::size_t>(b);
      for (int k = 0; k < 400; ++k) {
        const float v = static_cast<float>(0.7 * std::sin(6.2831853 * 1000.0 * k / kSr) * std::exp(-k / 60.0));
        audio->pcm[(s + static_cast<std::size_t>(k)) * 2] = v;
        audio->pcm[(s + static_cast<std::size_t>(k)) * 2 + 1] = v;
      }
    }
    build_waveform(*audio);
    auto lt = std::make_unique<LoadedTrack>();
    lt->audio = audio;
    lt->track_id = id;
    lt->bpm = t.bpm;
    lt->grid.push_back({fb, t.bpm, 1});
    lt->cue_point = fb;
    eng.load(deck, std::move(lt));
  }
  bool request_load(int deck, std::int64_t id) override {
    if (hold_loads) held.push_back({deck, id});
    else do_load(deck, id);
    return true;
  }
  void release() {
    for (const auto& h : held) do_load(h.first, h.second);
    held.clear();
    hold_loads = false;
  }
  LoadState load_state(int deck, std::int64_t id) override {
    const DeckView v = eng.deck(deck);
    return v.loaded && v.track_id == id ? LoadState::Ready : LoadState::Pending;
  }

  // One render block; returns the peak of the master output.
  float step(PrepMode* prep) {
    std::vector<float> buf(kBlock * 2);
    eng.render(buf.data(), kBlock);
    EngineEvent e;
    while (eng.poll(e)) {
      events.push_back(e);
      if (prep) prep->on_engine_event(e);
    }
    if (prep) prep->tick();
    eng.collect_garbage();
    float pk = 0;
    for (const float v : buf) pk = std::max(pk, std::fabs(v));
    return pk;
  }
  float run(double seconds, PrepMode* prep) {
    float pk = 0;
    const std::uint64_t end = eng.clock_frames() + static_cast<std::uint64_t>(seconds * kSr);
    while (eng.clock_frames() < end) pk = std::max(pk, step(prep));
    return pk;
  }
  bool run_until(double max_seconds, PrepMode* prep, const std::function<bool()>& done) {
    const std::uint64_t end = eng.clock_frames() + static_cast<std::uint64_t>(max_seconds * kSr);
    while (eng.clock_frames() < end) {
      step(prep);
      if (done()) return true;
    }
    return false;
  }
};

}  // namespace

int main() {
  Host h;
  h.tracks[1] = {120, 100, 60};
  h.tracks[2] = {126, 250, 60};
  h.tracks[3] = {100, 100, 60};
  PrepMode prep(h);
  std::string refusal, message, error;

  // Refusals: a headphone output is needed (the offline engine has two channels), another reason can block, busy decks.
  CHECK(!prep.enter(refusal, message) && refusal == "no_cue_output" && !prep.active());
  prep.set_cue_output_check([] { return true; });
  bool blocked = true;
  prep.set_blocker([&] { return blocked ? std::string("automix is running") : std::string(); });
  CHECK(!prep.enter(refusal, message) && refusal == "blocked" && message == "automix is running");
  blocked = false;
  h.do_load(2, 3);  // deck 3 holds a track
  h.run(0.05, nullptr);
  CHECK(h.eng.deck(2).loaded);
  CHECK(!prep.enter(refusal, message) && refusal == "busy_deck" && message.find("deck 3") != std::string::npos);
  {
    Command c;
    c.op = Op::Eject;
    c.deck = 2;
    h.eng.send(c);
    c.op = Op::Fader;
    c.deck = 3;
    c.f = 0.8f;
    h.eng.send(c);
    h.run(0.05, nullptr);
  }
  CHECK(!h.eng.deck(2).loaded && std::fabs(h.eng.deck(3).fader - 0.8f) < 1e-4);
  h.do_load(3, 3);
  {
    Command c;
    c.op = Op::PlayToggle;
    c.deck = 3;
    h.eng.send(c);
    h.run(0.05, nullptr);
  }
  CHECK(h.eng.deck(3).playing);
  CHECK(!prep.enter(refusal, message) && refusal == "busy_deck" && message.find("deck 4") != std::string::npos);
  {
    Command c;
    c.op = Op::PlayToggle;
    c.deck = 3;
    h.eng.send(c);
    c.op = Op::Eject;
    h.eng.send(c);
    h.run(0.05, nullptr);
  }
  CHECK(!h.eng.deck(3).loaded);
  const float fader_before = h.eng.deck(3).fader;  // 0.8, set by hand above
  CHECK(std::fabs(fader_before - 0.8f) < 1e-4 && !h.eng.deck(3).pfl);

  // Enter: faders to zero, cue on, nothing of it on the master.
  CHECK(prep.enter(refusal, message) && prep.active());
  h.run(0.05, &prep);
  for (int d : {2, 3}) CHECK(h.eng.deck(d).fader == 0.0f && h.eng.deck(d).pfl);
  CHECK(prep.load(0, 1, error) && prep.load(1, 2, error));
  CHECK(h.run_until(2, &prep, [&] { return !prep.status().loading[0] && !prep.status().loading[1] && prep.status().loaded[0] && prep.status().loaded[1]; }));
  h.run(0.05, &prep);
  PrepStatus st = prep.status();
  CHECK(st.track[0] == 1 && st.track[1] == 2 && std::fabs(st.native_bpm[1] - 126) < 1e-9);
  CHECK(std::fabs(h.eng.deck(3).bpm - 120) < 0.3);  // B follows A: 126 pulled down to 120
  prep.play(0, 1);
  prep.play(1, 1);
  const float master_peak = h.run(1.0, &prep);
  CHECK(h.eng.deck(2).playing && h.eng.deck(3).playing);
  CHECK(master_peak == 0.0f);  // two playing decks, silent master
  // A fader moved by a controller is pulled back (and counted).
  {
    Command c;
    c.op = Op::Fader;
    c.deck = 2;
    c.f = 1.0f;
    h.eng.send(c);
  }
  h.run(0.2, &prep);  // (the engine plays the block that carries the fader move before the guard can answer)
  CHECK(h.eng.deck(2).fader == 0.0f && prep.status().guard_hits >= 1);
  // Tempo follow off: B returns to its own tempo.
  prep.set_tempo_match(false);
  h.run(0.05, &prep);
  CHECK(std::fabs(h.eng.deck(3).bpm - 126) < 0.3);
  prep.set_tempo_match(true);
  h.run(0.05, &prep);
  CHECK(std::fabs(h.eng.deck(3).bpm - 120) < 0.3);
  // Blend: all A, all B, both at full level in the middle.
  prep.set_blend(0.5);
  h.run(0.05, &prep);
  CHECK(h.eng.deck(2).pfl && h.eng.deck(3).pfl && std::fabs(h.eng.deck(2).auto_gain_db - h.eng.deck(3).auto_gain_db) < 0.1f);
  prep.set_blend(1.0);
  h.run(0.05, &prep);
  CHECK(!h.eng.deck(2).pfl && h.eng.deck(3).pfl);
  prep.set_blend(0.0);
  h.run(0.05, &prep);
  CHECK(h.eng.deck(2).pfl && !h.eng.deck(3).pfl);

  // Swap at the next bar line of the deck in the headphones: B is not playing yet, so the swap starts it.
  prep.play(1, 0);
  prep.seek(1, 250);
  h.run(0.05, &prep);
  CHECK(!h.eng.deck(3).playing);
  CHECK(prep.swap_at_next_bar(error) && prep.status().swap_armed);
  CHECK(!prep.swap_at_next_bar(error) && !error.empty());  // one at a time
  const double a_before = h.eng.deck(2).position;
  CHECK(h.run_until(5, &prep, [&] { return !prep.status().swap_armed; }));
  const double waited = (h.eng.deck(2).position - a_before) / kSr;
  CHECK(waited > 0 && waited <= 60.0 / 120 * 4 + 0.1);  // never longer than one bar of A
  h.run(0.05, &prep);
  st = prep.status();
  CHECK(st.blend == 1.0 && h.eng.deck(3).playing && h.eng.deck(3).pfl && !h.eng.deck(2).pfl);

  // Leaving puts everything back.
  prep.exit();
  h.run(0.1, &prep);
  CHECK(!prep.active() && !h.eng.deck(2).loaded && !h.eng.deck(3).loaded);
  CHECK(std::fabs(h.eng.deck(3).fader - 0.8f) < 1e-4 && std::fabs(h.eng.deck(2).fader - 1.0f) < 1e-4);
  CHECK(!h.eng.deck(2).pfl && !h.eng.deck(3).pfl && h.eng.deck(3).tempo == 0 && h.eng.deck(2).auto_gain_db == 0);
  CHECK(!prep.load(0, 1, error) && error.find("off") != std::string::npos);

  // Leaving while a load is still in flight: the track that lands is cleared, the faders come back.
  CHECK(prep.enter(refusal, message));
  h.hold_loads = true;
  CHECK(prep.load(1, 1, error));
  h.run(0.05, &prep);
  prep.exit();
  CHECK(!prep.active());
  h.run(0.05, &prep);
  CHECK(h.eng.deck(3).fader == 0.0f);  // still held at zero until the load has landed
  h.release();
  CHECK(h.run_until(2, &prep, [&] { return !h.eng.deck(3).loaded && std::fabs(h.eng.deck(3).fader - 0.8f) < 1e-4; }));
  CHECK(!h.eng.deck(3).pfl);
  CHECK(prep.enter(refusal, message));  // and it can be entered again
  prep.exit();
  h.run(0.05, &prep);

  // The deck on air (what the suggestions follow): the playing deck that is loudest at the master.
  CHECK(master_deck(h.eng) == -1);
  h.do_load(0, 1);
  h.do_load(1, 2);
  h.run(0.05, nullptr);
  CHECK(master_deck(h.eng) == 0);  // loaded, nothing plays: the first loaded deck
  {
    Command c;
    c.op = Op::PlayToggle;
    for (int d : {0, 1}) {
      c.deck = static_cast<std::uint8_t>(d);
      h.eng.send(c);
    }
    c.op = Op::Crossfader;
    c.f = 0.0f;
    h.eng.send(c);
    h.run(0.05, nullptr);
    CHECK(master_deck(h.eng) == 0);
    c.f = 1.0f;
    h.eng.send(c);
    h.run(0.05, nullptr);
    CHECK(master_deck(h.eng) == 1);
  }

  std::puts("test_prep_mode ok");
  return 0;
}
