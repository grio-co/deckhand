// Offline engine tests: no audio device, rendered into memory.
#include <cmath>
#include <cstdio>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"

using namespace deckhand;

namespace {

const double kPi = 3.14159265358979323846;

std::unique_ptr<LoadedTrack> sine_track(double hz, double seconds, std::uint32_t sr = 44100, double bpm = 120) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = sr;
  audio->frames = static_cast<std::uint64_t>(seconds * sr);
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i) {
    const float v = 0.5f * static_cast<float>(std::sin(2 * kPi * hz * static_cast<double>(i) / sr));
    audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = v;
  }
  build_waveform(*audio);
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->bpm = bpm;
  t->grid.push_back({0, bpm, 1});
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

float rms(const std::vector<float>& buf, int channels, int ch, std::size_t from = 0) {
  double sum = 0;
  std::size_t n = 0;
  for (std::size_t i = from; i < buf.size() / static_cast<std::size_t>(channels); ++i, ++n) {
    const double v = buf[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch)];
    sum += v * v;
  }
  return n ? static_cast<float>(std::sqrt(sum / static_cast<double>(n))) : 0;
}

// Render `seconds` and return the buffer.
std::vector<float> run(Engine& e, double seconds, int channels = 4, std::uint32_t sr = 44100) {
  const std::uint32_t frames = static_cast<std::uint32_t>(seconds * sr);
  std::vector<float> out(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
  std::uint32_t done = 0;
  while (done < frames) {  // device-sized callbacks
    const std::uint32_t n = std::min<std::uint32_t>(256, frames - done);
    e.render(out.data() + static_cast<std::size_t>(done) * static_cast<std::size_t>(channels), n);
    done += n;
  }
  return out;
}

}  // namespace

int main() {
  Engine e;
  e.configure_offline(44100, 4);
  e.load(0, sine_track(440, 20));
  run(e, 0.1);
  CHECK(e.deck(0).loaded && !e.deck(0).playing);

  // Stopped deck is silent; playing deck is audible on master, not on headphones (cue off, mix = cue).
  auto out = run(e, 0.2);
  CHECK(rms(out, 4, 0) < 1e-5f);
  e.send(cmd(Op::HeadMix, 0, 0.0f));
  e.send(cmd(Op::PlayToggle, 0));
  out = run(e, 1.0);
  CHECK(e.deck(0).playing);
  CHECK(rms(out, 4, 0, 4410) > 0.3f);
  CHECK(rms(out, 4, 2, 4410) < 1e-4f);
  // Position advanced by about one second.
  CHECK(std::fabs(e.deck(0).position - 44100.0) < 600.0);

  // Headphone cue routes the deck to channels 3-4 even with the fader down.
  e.send(cmd(Op::Pfl, 0, 0, 1));
  e.send(cmd(Op::Fader, 0, 0.0f));
  out = run(e, 0.5);
  CHECK(rms(out, 4, 0, 4410) < 1e-3f);
  CHECK(rms(out, 4, 2, 4410) > 0.3f);
  e.send(cmd(Op::Fader, 0, 1.0f));

  // Crossfader fully right silences deck 1 (left side).
  e.send(cmd(Op::Crossfader, 0, 1.0f));
  out = run(e, 0.5);
  CHECK(rms(out, 4, 0, 4410) < 1e-3f);
  e.send(cmd(Op::Crossfader, 0, 0.5f));

  // Tempo +6 % moves the playhead faster.
  e.send(cmd(Op::Tempo, 0, 1.0f));
  run(e, 0.2);
  const double p0 = e.deck(0).position;
  run(e, 1.0);
  CHECK(std::fabs((e.deck(0).position - p0) - 44100.0 * 1.06) < 300.0);
  CHECK(std::fabs(e.deck(0).bpm - 127.2) < 0.2);
  e.send(cmd(Op::Tempo, 0, 0.0f));

  // EQ: a 440 Hz tone drops a lot with mid cut, barely with high cut.
  out = run(e, 0.5);
  const float flat = rms(out, 4, 0, 4410);
  e.send(cmd(Op::EqHigh, 0, 0.0f));
  out = run(e, 0.5);
  CHECK(rms(out, 4, 0, 8820) > flat * 0.8f);
  e.send(cmd(Op::EqHigh, 0, 0.5f));
  e.send(cmd(Op::EqMid, 0, 0.0f));
  e.send(cmd(Op::EqLow, 0, 0.0f));
  out = run(e, 0.5);
  CHECK(rms(out, 4, 0, 8820) < flat * 0.35f);
  e.send(cmd(Op::EqMid, 0, 0.5f));
  e.send(cmd(Op::EqLow, 0, 0.5f));
  // High-pass filter knob fully right removes a 440 Hz tone.
  e.send(cmd(Op::Filter, 0, 1.0f));
  out = run(e, 0.5);
  CHECK(rms(out, 4, 0, 8820) < flat * 0.2f);
  e.send(cmd(Op::Filter, 0, 0.5f));

  // Cue: while playing, CUE returns to the cue point and stops.
  e.send(cmd(Op::CueDown, 0));
  e.send(cmd(Op::CueUp, 0));
  run(e, 0.1);
  CHECK(!e.deck(0).playing && e.deck(0).position < 1.0 && e.deck(0).at_cue);
  // Holding CUE at the cue point previews; release returns.
  e.send(cmd(Op::CueDown, 0));
  run(e, 0.5);
  CHECK(e.deck(0).playing && e.deck(0).position > 20000.0);
  e.send(cmd(Op::CueUp, 0));
  run(e, 0.1);
  CHECK(!e.deck(0).playing && e.deck(0).position < 1.0);

  // Hot cue: set while playing, jump back to it later; the set is reported as an event.
  e.send(cmd(Op::PlayToggle, 0));
  run(e, 1.0);
  e.send(cmd(Op::HotCueDown, 0, 0, 2));
  e.send(cmd(Op::HotCueUp, 0, 0, 2));
  run(e, 1.0);
  const double set_at = e.deck(0).hot_cue[2];
  CHECK(set_at > 40000.0 && set_at < 48000.0);
  EngineEvent ev;
  bool saw_set = false;
  while (e.poll(ev)) saw_set = saw_set || (ev.type == EventType::HotCueSet && ev.slot == 2);
  CHECK(saw_set);
  e.send(cmd(Op::HotCueDown, 0, 0, 2));
  e.send(cmd(Op::HotCueUp, 0, 0, 2));
  run(e, 0.1);
  CHECK(e.deck(0).playing && std::fabs(e.deck(0).position - (set_at + 4410.0)) < 600.0);

  // 4-beat loop at 120 BPM is 2 s long: after 5 s the playhead is still inside it.
  e.send(cmd(Op::LoopToggle, 0, 4.0f));
  run(e, 0.05);
  const DeckView lv = e.deck(0);
  CHECK(lv.loop_active && std::fabs((lv.loop_out - lv.loop_in) - 88200.0) < 2.0);
  run(e, 5.0);
  CHECK(e.deck(0).position >= lv.loop_in && e.deck(0).position <= lv.loop_out);
  e.send(cmd(Op::CueCall, 0, 0, -1));  // halve
  run(e, 0.05);
  CHECK(std::fabs((e.deck(0).loop_out - e.deck(0).loop_in) - 44100.0) < 2.0);
  e.send(cmd(Op::LoopToggle, 0, 4.0f));
  run(e, 0.05);
  CHECK(!e.deck(0).loop_active);

  // Scratch: touching holds the record; turning one revolution moves 1.8 s of audio.
  e.send(cmd(Op::JogTouch, 0, 0, 1));
  run(e, 0.3);
  const double held = e.deck(0).position;
  run(e, 0.3);
  CHECK(std::fabs(e.deck(0).position - held) < 50.0);
  for (int i = 0; i < 100; ++i) {
    e.send(cmd(Op::JogTurn, 0, 0, 1, 0.01));
    run(e, 0.005);
  }
  run(e, 0.2);
  CHECK(std::fabs((e.deck(0).position - held) - 1.8 * 44100.0) < 400.0);
  e.send(cmd(Op::JogTouch, 0, 0, 0));
  run(e, 0.5);
  CHECK(e.deck(0).position > held + 1.8 * 44100.0 + 15000.0);  // playing again

  // Sync: deck 2 at 100 BPM follows deck 1 at 120 BPM and lands on its beat phase.
  e.load(1, sine_track(220, 30, 48000, 100));
  run(e, 0.1);
  e.send(cmd(Op::PlayToggle, 1));
  run(e, 0.37);
  e.send(cmd(Op::SyncToggle, 1));
  run(e, 1.0);
  CHECK(e.deck(1).sync && std::fabs(e.deck(1).bpm - 120.0) < 0.1);
  const DeckView a = e.deck(0), b = e.deck(1);
  const double beat_a = 60.0 / 120.0 * 44100.0, beat_b = 60.0 / 100.0 * 48000.0;
  double phase = std::fmod(a.position / beat_a, 1.0) - std::fmod(b.position / beat_b, 1.0);
  if (phase > 0.5) phase -= 1;
  if (phase < -0.5) phase += 1;
  CHECK(std::fabs(phase) < 0.02);

  // A deck stops at the end of its track and reports it.
  e.load(2, sine_track(330, 0.5));
  run(e, 0.1);
  e.send(cmd(Op::PlayToggle, 2));
  run(e, 1.0);
  CHECK(!e.deck(2).playing);
  bool ended = false;
  while (e.poll(ev)) ended = ended || (ev.type == EventType::TrackEnded && ev.deck == 2);
  CHECK(ended);

  // Output never exceeds full scale even with everything boosted.
  e.send(cmd(Op::Trim, 0, 1.0f));
  e.send(cmd(Op::Trim, 1, 1.0f));
  e.send(cmd(Op::MasterLevel, 0, 1.0f));
  out = run(e, 1.0);
  float peak = 0;
  for (float v : out) peak = std::max(peak, std::fabs(v));
  CHECK(peak <= 1.0f);

  e.collect_garbage();

  // Tempo estimate: clicks at 128 BPM starting 0.2 s in.
  {
    TrackAudio click;
    click.sample_rate = 44100;
    click.frames = 44100 * 60;
    click.pcm.assign(click.frames * 2, 0.0f);
    const double period = 60.0 / 128.0;
    for (double t = 0.2; t < 59.9; t += period) {
      const std::size_t start = static_cast<std::size_t>(t * 44100);
      for (std::size_t i = 0; i < 1200; ++i) {
        const float v = 0.8f * static_cast<float>(std::sin(2 * kPi * 60.0 * static_cast<double>(i) / 44100.0)) *
                        (1.0f - static_cast<float>(i) / 1200.0f);
        click.pcm[(start + i) * 2] = click.pcm[(start + i) * 2 + 1] = v;
      }
    }
    build_waveform(click);
    double bpm = 0, first = 0;
    CHECK(estimate_tempo(click, bpm, first));
    std::printf("estimated %.2f BPM, first beat %.3f s\n", bpm, first);
    CHECK(std::fabs(bpm - 128.0) < 0.05);
    double off = std::fmod(first - 0.2 + period * 4, period);
    if (off > period / 2) off -= period;
    CHECK(std::fabs(off) < 0.015);
  }
  std::puts("engine ok");
  return 0;
}
