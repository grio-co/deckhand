// Offline engine tests for track preparation: grid replacement on a loaded deck, editable cues and loops, the
// metronome on the headphone bus.
#include <cmath>
#include <cstdio>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"

using namespace deckhand;

namespace {

const double kPi = 3.14159265358979323846;

std::unique_ptr<LoadedTrack> sine_track(double seconds, double bpm, double first_beat_frame = 0, std::uint32_t sr = 44100) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = sr;
  audio->frames = static_cast<std::uint64_t>(seconds * sr);
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i) {
    const float v = 0.4f * static_cast<float>(std::sin(2 * kPi * 330.0 * static_cast<double>(i) / sr));
    audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = v;
  }
  build_waveform(*audio);
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->bpm = bpm;
  t->grid.push_back({first_beat_frame, bpm, 1});
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

std::vector<float> run(Engine& e, std::uint32_t frames, int channels = 4) {
  std::vector<float> out(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
  std::uint32_t done = 0;
  while (done < frames) {
    const std::uint32_t n = std::min<std::uint32_t>(256, frames - done);
    e.render(out.data() + static_cast<std::size_t>(done) * static_cast<std::size_t>(channels), n);
    done += n;
  }
  return out;
}

// First sample at or after `from` whose channel value is not silent, or -1.
long first_sound(const std::vector<float>& buf, int channels, int ch, std::size_t from, float eps = 1e-5f) {
  for (std::size_t i = from; i < buf.size() / static_cast<std::size_t>(channels); ++i)
    if (std::fabs(buf[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch)]) > eps) return static_cast<long>(i);
  return -1;
}

}  // namespace

int main() {
  // ---- grid replacement keeps the playhead and re-quantises --------------------------------------------------
  {
    Engine e;
    e.configure_offline(44100, 4);
    e.load(0, sine_track(30, 120));
    run(e, 512);
    e.send(cmd(Op::Seek, 0, 0, 0, 100000));
    e.send(cmd(Op::Quantize, 0, 0, 1));
    run(e, 512);
    const double pos = e.deck(0).position;
    CHECK(std::fabs(pos - 100000) < 1);
    const std::uint32_t rev0 = e.deck(0).meta_rev;

    TrackMeta m;
    m.bpm = 100;
    m.grid.push_back({11025, 100, 1});  // 0.25 s offset, slower tempo: beats every 26460 frames
    m.memory_cues = {50000, 150000};
    e.set_track_meta(0, 0, m);
    run(e, 512);
    DeckView v = e.deck(0);
    CHECK(v.loaded && v.meta_rev == rev0 + 1);
    CHECK(std::fabs(v.position - pos) < 1);  // not moved, not reloaded
    CHECK(std::fabs(v.bpm - 100) < 1e-6);   // the new grid's tempo is in effect

    // Quantised hot cue lands on a beat of the NEW grid: 11025 + k * 26460 nearest to 100000 -> k = 3 (90405) or 4 (116865).
    e.send(cmd(Op::HotCueDown, 0, 0, 2));
    run(e, 512);
    v = e.deck(0);
    CHECK(v.hot_cue[2] >= 0);
    const double k = (v.hot_cue[2] - 11025) / 26460.0;
    CHECK(std::fabs(k - std::round(k)) < 1e-6);
    CHECK(std::fabs(v.hot_cue[2] - 90405) < 1);

    // The new memory cues are the ones CueCall walks.
    e.send(cmd(Op::CueCall, 0, 0, 1));
    run(e, 512);
    CHECK(std::fabs(e.deck(0).position - 150000) < 600);
    e.send(cmd(Op::CueCall, 0, 0, -1));
    run(e, 512);
    CHECK(std::fabs(e.deck(0).position - 50000) < 600);

    // A replacement for another track id is ignored.
    TrackMeta other;
    other.bpm = 60;
    other.grid.push_back({0, 60, 1});
    e.set_track_meta(0, 77, other);
    run(e, 512);
    CHECK(e.deck(0).meta_rev == rev0 + 1);
  }

  // ---- cues and loops set at exact frames ---------------------------------------------------------------------
  {
    Engine e;
    e.configure_offline(44100, 4);
    e.load(0, sine_track(30, 120));
    run(e, 512);
    e.send(cmd(Op::HotCueSetAt, 0, 0, 3, 12345));
    e.send(cmd(Op::HotCueEndAt, 0, 0, 3, 40000));
    e.send(cmd(Op::CueSetAt, 0, 0, 0, 777));
    run(e, 512);
    DeckView v = e.deck(0);
    CHECK(v.hot_cue[3] == 12345 && v.hot_cue_end[3] == 40000 && v.cue == 777);
    e.send(cmd(Op::HotCueSetAt, 0, 0, 3, 20000));  // moving keeps the loop length
    run(e, 512);
    v = e.deck(0);
    CHECK(v.hot_cue[3] == 20000 && v.hot_cue_end[3] == 20000 + (40000 - 12345));
    e.send(cmd(Op::HotCueSetAt, 0, 0, 3, -1));  // deleting
    run(e, 512);
    CHECK(e.deck(0).hot_cue[3] < 0 && e.deck(0).hot_cue_end[3] < 0);
    e.send(cmd(Op::LoopSetIn, 0, 0, 0, 30000));
    e.send(cmd(Op::LoopSetOut, 0, 0, 0, 52050));
    run(e, 512);
    v = e.deck(0);
    CHECK(v.loop_in == 30000 && v.loop_out == 52050 && v.loop_active);
  }

  // ---- metronome: clicks on beats, headphone bus only ---------------------------------------------------------
  {
    Engine e;
    e.configure_offline(44100, 4);
    e.load(0, sine_track(30, 120, 4410));  // beats at 4410 + k * 22050
    run(e, 256);
    e.send(cmd(Op::HeadMix, 0, 0.0f));
    e.send(cmd(Op::HeadLevel, 0, 1.0f));
    e.send(cmd(Op::MetronomeLevel, 0, 1.0f));
    e.send(cmd(Op::Metronome, 0, 0, 1));
    e.send(cmd(Op::PlayToggle, 0));
    const std::uint32_t N = 44100 * 3;
    auto out = run(e, N);
    const double p_end = e.deck(0).position;
    CHECK(e.deck(0).metronome == 1);
    CHECK(e.mixer().metro_level == 1.0f);
    // Steady 1.0x playback: sample i read the position p_end - (N - i). Beat B is read at i = N - (p_end - B).
    int found = 0;
    for (int k = 1; k <= 3; ++k) {  // beats 1..3 (after the start-up slew)
      const double B = 4410.0 + k * 22050.0;
      const double expect = static_cast<double>(N) - (p_end - B);
      if (expect < 1 || expect >= N - 2000) continue;
      // the click's first sample is exactly 0 (sine phase 0), so its audible start is one sample later
      const long first = first_sound(out, 4, 2, static_cast<std::size_t>(expect) - 400);
      CHECK(first >= 0);
      CHECK(std::labs(first - 1 - static_cast<long>(std::lround(expect))) <= 1);
      CHECK(first_sound(out, 4, 3, static_cast<std::size_t>(expect) - 400) == first);  // both headphone channels
      ++found;
    }
    CHECK(found >= 2);
    // Nothing between clicks on the headphone bus (the deck itself is not cued), and the master has only the music.
    const double B1 = 4410.0 + 22050.0, B2 = B1 + 22050.0;
    const long gap_a = static_cast<long>(N - (p_end - B1)) + 4000, gap_b = static_cast<long>(N - (p_end - B2)) - 100;
    CHECK(gap_b > gap_a);
    for (long i = gap_a; i < gap_b; ++i) CHECK(std::fabs(out[static_cast<std::size_t>(i) * 4 + 2]) < 1e-6f);
    // The click is not on the master: master equals a run without the metronome.
    Engine f;
    f.configure_offline(44100, 4);
    f.load(0, sine_track(30, 120, 4410));
    run(f, 256);
    f.send(cmd(Op::HeadMix, 0, 0.0f));
    f.send(cmd(Op::HeadLevel, 0, 1.0f));
    f.send(cmd(Op::PlayToggle, 0));
    auto ref = run(f, N);
    CHECK(first_sound(ref, 4, 2, 22050) < 0);  // metronome off: headphones silent (once the mix knob settled)
    double diff = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) diff = std::max(diff, static_cast<double>(std::fabs(out[i * 4] - ref[i * 4])));
    CHECK(diff < 1e-6);
    // Toggle off: silence again.
    e.send(cmd(Op::Metronome, 0, 0, 0));
    out = run(e, 44100);
    CHECK(first_sound(out, 4, 2, 0) < 0);
  }

  // ---- metronome follows a replaced grid -------------------------------------------------------------------------
  {
    Engine e;
    e.configure_offline(44100, 4);
    e.load(0, sine_track(30, 120, 0));
    run(e, 256);
    e.send(cmd(Op::HeadMix, 0, 0.0f));
    e.send(cmd(Op::HeadLevel, 0, 1.0f));
    e.send(cmd(Op::MetronomeLevel, 0, 1.0f));
    e.send(cmd(Op::Metronome, 0, 0, 1));
    e.send(cmd(Op::PlayToggle, 0));
    run(e, 44100);
    TrackMeta m;
    m.bpm = 120;
    m.grid.push_back({10000, 120, 1});  // beats at 10000 + k * 22050
    e.set_track_meta(0, 0, m);
    run(e, 64);
    const double p0 = e.deck(0).position;
    auto out = run(e, 44100);
    const double p1 = e.deck(0).position;
    const double B = 10000.0 + 22050.0 * std::ceil((p0 - 10000.0) / 22050.0 + 1e-9);
    const double expect = 44100.0 - (p1 - B);
    CHECK(expect > 0 && expect < 44100 - 500);
    const long first = first_sound(out, 4, 2, 0);
    CHECK(first >= 0 && std::labs(first - 1 - static_cast<long>(std::lround(expect))) <= 1);
  }

  // ---- pre-listen player: headphones only -----------------------------------------------------------------------
  {
    Engine e;
    e.configure_offline(44100, 4);
    CHECK(!e.preview().loaded);
    e.send(cmd(Op::HeadMix, 0, 0.0f));
    e.send(cmd(Op::HeadLevel, 0, 1.0f));
    e.send(cmd(Op::MasterLevel, 0, 1.0f));
    e.preview_load(sine_track(10, 120));
    run(e, 512);
    PreviewView p = e.preview();
    CHECK(p.loaded && !p.playing && p.position == 0 && p.length == 441000 && !p.on_master);
    auto silent = run(e, 22050);
    CHECK(first_sound(silent, 4, 2, 4410) < 0);  // loaded but not playing: nothing is heard
    e.send(cmd(Op::PreviewPlay, 0, 0, 1));
    auto out = run(e, 44100);
    CHECK(e.preview().playing && e.preview().position > 40000);
    CHECK(first_sound(out, 4, 2, 4410) >= 0 && first_sound(out, 4, 3, 4410) >= 0);   // headphone pair
    CHECK(first_sound(out, 4, 0, 0) < 0 && first_sound(out, 4, 1, 0) < 0);            // master pair: never
    // the level scales it
    auto peak = [&](const std::vector<float>& b, int ch) {
      float m = 0;
      for (std::size_t i = 22050; i < b.size() / 4; ++i) m = std::max(m, std::fabs(b[i * 4 + static_cast<std::size_t>(ch)]));
      return m;
    };
    const float loud = peak(out, 2);
    e.send(cmd(Op::PreviewLevel, 0, 0.35f));
    out = run(e, 44100);
    CHECK(peak(out, 2) < loud * 0.7f && peak(out, 2) > loud * 0.3f);
    // it keeps its place across a seek, pauses, and stops with a fade
    e.send(cmd(Op::PreviewSeek, 0, 0, 0, 300000));
    run(e, 256);
    CHECK(std::fabs(e.preview().position - 300000) < 600);
    e.send(cmd(Op::PreviewPlay, 0, 0, 0));
    run(e, 4410);
    const double held = e.preview().position;
    run(e, 4410);
    CHECK(e.preview().position == held && !e.preview().playing);
    CHECK(first_sound(run(e, 8820), 4, 2, 2205) < 0);
    // a deck's track and the preview are independent
    e.load(0, sine_track(10, 120));
    e.send(cmd(Op::PreviewPlay, 0, 0, 1));
    run(e, 2048);
    CHECK(e.deck(0).loaded && !e.deck(0).playing && e.preview().playing);
    e.send(cmd(Op::PreviewStop, 0));
    run(e, 4410);
    CHECK(!e.preview().loaded && !e.preview().playing);
    // a new preview replaces the old one, paused
    e.preview_load(sine_track(5, 100));
    run(e, 256);
    CHECK(e.preview().loaded && e.preview().length == 220500 && e.preview().position == 0);
    // replacing one that is playing: it fades out instead of stopping dead, each time
    e.send(cmd(Op::PreviewPlay, 0, 0, 1));
    auto before = run(e, 22050);
    for (int again = 0; again < 2; ++again) {
      e.preview_load(sine_track(5, 100));
      auto after = run(e, 441);
      float first = 0;
      for (std::size_t i = 0; i < 441; ++i) first = std::max(first, std::fabs(after[i * 4 + 2]));
      CHECK(first > 0.03f);
      CHECK(std::fabs(after[2] - before[before.size() - 2]) < 0.03f);  // no step at the swap
      CHECK(!e.preview().playing && e.preview().position == 0);
      e.send(cmd(Op::PreviewPlay, 0, 0, 1));
      before = run(e, 8820);  // long enough for the earlier fade to end
    }
    e.send(cmd(Op::PreviewPlay, 0, 0, 0));
    run(e, 8820);
    CHECK(first_sound(run(e, 8820), 4, 2, 2205) < 0);
    // routing to the master is an explicit switch
    e.send(cmd(Op::PreviewMaster, 0, 0, 1));
    e.send(cmd(Op::PreviewPlay, 0, 0, 1));
    out = run(e, 22050);
    CHECK(e.preview().on_master);
    CHECK(first_sound(out, 4, 0, 4410) >= 0);
    // and the end of the track stops it
    e.send(cmd(Op::PreviewSeek, 0, 0, 0, 220000));
    run(e, 4410);
    CHECK(!e.preview().playing);
    // stereo-only device: no headphone pair, so it is silent unless routed to the master (the app refuses to start it)
    Engine st;
    st.configure_offline(44100, 2);
    st.preview_load(sine_track(5, 120));
    st.send(cmd(Op::PreviewPlay, 0, 0, 1));
    auto so = run(st, 22050, 2);
    CHECK(first_sound(so, 2, 0, 2205) < 0);
  }

  // ---- cue-only: a deck that never reaches the master, whatever the faders do ------------------------------------
  {
    Engine e;
    e.configure_offline(44100, 4);
    e.load(2, sine_track(30, 120));
    run(e, 512);
    e.send(cmd(Op::Pfl, 2, 0, 1));
    e.send(cmd(Op::Fader, 2, 1.0f));
    e.cue_only(2, 1);
    e.send(cmd(Op::PlayToggle, 2));
    CHECK(run(e, 256).size() > 0);  // the commands are applied at the start of a block
    bool leaked = false, cue_heard = false;
    float fader = 0, xf = 0;
    for (int block = 0; block < 400; ++block) {  // a controller sweeping the fader and the crossfader, in every block
      fader = std::fmod(fader + 0.37f, 1.0f);
      xf = std::fmod(xf + 0.23f, 1.0f);
      e.send(cmd(Op::Fader, 2, fader));
      e.send(cmd(Op::Crossfader, 0, xf));
      const std::vector<float> out = run(e, 64);
      if (first_sound(out, 4, 0, 0, 0.0f) >= 0 || first_sound(out, 4, 1, 0, 0.0f) >= 0) leaked = true;  // exact zero
      if (first_sound(out, 4, 2, 0) >= 0) cue_heard = true;
    }
    CHECK(!leaked && cue_heard);
    CHECK(e.deck(2).cue_only == 1);
    e.cue_only(2, 0);  // back to normal: the master hears it again
    e.send(cmd(Op::Fader, 2, 1.0f));
    e.send(cmd(Op::Crossfader, 0, 0.0f));  // deck 3 sits on the left side
    run(e, 512);
    const std::vector<float> after = run(e, 2048);
    CHECK(first_sound(after, 4, 0, 0) >= 0 && e.deck(2).cue_only == 0);
  }
  std::puts("test_engine_prep: ok");
  return 0;
}
