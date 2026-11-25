// Offline tests for the microphone channel: routing and gain, high-pass and EQ, noise gate, talkover (depth, attack,
// release, voice activation), cue bus and monitor, recording and stream tap, and the capture-to-output clock-drift
// buffer (+-0.5 % for a minute, a stalled device). No audio device is opened: the input is injected with
// Engine::render_with_input / feed_input (and one null capture device, which delivers silence).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"

using namespace deckhand;

namespace {

const double kPi = 3.14159265358979323846;
constexpr double kSr = 48000;
constexpr int kCh = 4;  // master pair 0/1, headphone pair 2/3

Command cmd(Op op, float f = 0, int i = 0) {
  Command c;
  c.op = op;
  c.f = f;
  c.i = i;
  return c;
}

std::unique_ptr<Engine> engine() {
  auto e = std::make_unique<Engine>();
  e->configure_offline(static_cast<std::uint32_t>(kSr), kCh);
  return e;
}

// A track that holds `value` (DC) or a sine, 48 kHz so the deck does not resample.
std::unique_ptr<LoadedTrack> track(double seconds, double hz, float amp) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = static_cast<std::uint32_t>(kSr);
  audio->frames = static_cast<std::uint64_t>(seconds * kSr);
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i) {
    const float v = hz > 0 ? amp * static_cast<float>(std::sin(2 * kPi * hz * static_cast<double>(i) / kSr)) : amp;
    audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = v;
  }
  build_waveform(*audio);
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->bpm = 120;
  t->grid.push_back({0, 120, 1});
  return t;
}

// Sine generator for the injected microphone.
struct Src {
  double hz = 0, amp = 0, phase = 0;
  void fill(float* stereo, int n, double rate = kSr) {
    for (int i = 0; i < n; ++i) {
      const float v = amp > 0 ? static_cast<float>(amp * std::sin(phase)) : 0.0f;
      stereo[2 * i] = stereo[2 * i + 1] = v;
      phase += 2 * kPi * hz / rate;
      if (phase > 2 * kPi) phase -= 2 * kPi;
    }
  }
};

// Renders `seconds` with the microphone source; returns the interleaved output.
std::vector<float> run(Engine& e, double seconds, Src& src) {
  const std::size_t frames = static_cast<std::size_t>(seconds * kSr);
  std::vector<float> out(frames * kCh);
  std::vector<float> in(256 * 2);
  for (std::size_t done = 0; done < frames;) {
    const int n = static_cast<int>(std::min<std::size_t>(256, frames - done));
    src.fill(in.data(), n);
    e.render_with_input(out.data() + done * kCh, in.data(), static_cast<std::uint32_t>(n));
    done += static_cast<std::size_t>(n);
  }
  return out;
}

// Amplitude of the `hz` component of channel `ch` over [t0, t1) seconds.
double tone(const std::vector<float>& b, int ch, double hz, double t0, double t1) {
  const std::size_t i0 = static_cast<std::size_t>(t0 * kSr), i1 = std::min(static_cast<std::size_t>(t1 * kSr), b.size() / kCh);
  double re = 0, im = 0;
  for (std::size_t i = i0; i < i1; ++i) {
    const double w = 2 * kPi * hz * static_cast<double>(i) / kSr, x = b[i * kCh + static_cast<std::size_t>(ch)];
    re += x * std::cos(w);
    im += x * std::sin(w);
  }
  return 2 * std::hypot(re, im) / static_cast<double>(i1 - i0);
}
double peak(const std::vector<float>& b, int ch, double t0, double t1) {
  const std::size_t i0 = static_cast<std::size_t>(t0 * kSr), i1 = std::min(static_cast<std::size_t>(t1 * kSr), b.size() / kCh);
  double p = 0;
  for (std::size_t i = i0; i < i1; ++i) p = std::max(p, static_cast<double>(std::fabs(b[i * kCh + static_cast<std::size_t>(ch)])));
  return p;
}

// Gain of the microphone path at `hz` (amplitude 0.3 in, level unity): output over input.
double path_gain(Engine& e, double hz) {
  Src s{hz, 0.3};
  run(e, 0.3, s);  // settle
  return tone(run(e, 0.5, s), 0, hz, 0.05, 0.5) / 0.3;
}

void test_routing_and_gain() {
  auto e = engine();
  Src s{1000, 0.3};
  // off by default: nothing comes through
  auto out = run(*e, 0.3, s);
  CHECK(peak(out, 0, 0.05, 0.3) < 1e-6 && peak(out, 1, 0.05, 0.3) < 1e-6);
  CHECK(!e->mixer().mic_on && e->mixer().mic_present);  // injected input counts as present

  e->send(cmd(Op::MicOn, 0, -1));  // toggle on
  run(*e, 0.1, s);
  out = run(*e, 0.5, s);
  const double a0 = tone(out, 0, 1000, 0.1, 0.5), a1 = tone(out, 1, 1000, 0.1, 0.5);
  std::printf("mic at unity: %.4f / %.4f (input 0.3), peak meter %.3f\n", a0, a1, e->mixer().mic_peak);
  CHECK(std::fabs(a0 - 0.3) < 0.003 && std::fabs(a1 - 0.3) < 0.003);
  CHECK(e->mixer().mic_on && std::fabs(e->mixer().mic_peak - 0.3) < 0.01);
  CHECK(peak(out, 2, 0.1, 0.5) < 1e-6 && peak(out, 3, 0.1, 0.5) < 1e-6);  // not on the cue bus

  // level: 0.4 -> gain 0.25 (the knob law shared with the other levels)
  e->send(cmd(Op::MicLevel, 0.4f));
  run(*e, 0.1, s);
  out = run(*e, 0.5, s);
  CHECK(std::fabs(tone(out, 0, 1000, 0.1, 0.5) - 0.075) < 0.002);
  e->send(cmd(Op::MicLevel, 9.0f));  // clamped
  run(*e, 0.1, s);
  CHECK(e->mixer().mic_level == 1.0f);
  e->send(cmd(Op::MicLevel, 0.8f));

  // master level scales the microphone like everything else
  e->send(cmd(Op::MasterLevel, 0.4f));
  run(*e, 0.1, s);
  out = run(*e, 0.5, s);
  CHECK(std::fabs(tone(out, 0, 1000, 0.1, 0.5) - 0.075) < 0.002);
  e->send(cmd(Op::MasterLevel, 0.8f));

  // off again: silent within a few milliseconds
  e->send(cmd(Op::MicOn, 0, 0));
  run(*e, 0.1, s);
  out = run(*e, 0.3, s);
  CHECK(peak(out, 0, 0.0, 0.3) < 1e-6);

  // monitor: the microphone also reaches the headphone pair, and only then
  e->send(cmd(Op::MicOn, 0, 1));
  run(*e, 0.1, s);
  out = run(*e, 0.3, s);
  CHECK(peak(out, 2, 0.05, 0.3) < 1e-6);
  e->send(cmd(Op::MicMonitor, 0, 1));
  run(*e, 0.1, s);
  out = run(*e, 0.5, s);
  CHECK(std::fabs(tone(out, 2, 1000, 0.1, 0.5) - 0.3) < 0.005 && std::fabs(tone(out, 3, 1000, 0.1, 0.5) - 0.3) < 0.005);
  CHECK(e->mixer().mic_monitor);
  e->send(cmd(Op::MicMonitor, 0, 0));
  run(*e, 0.1, s);
  out = run(*e, 0.2, s);
  CHECK(peak(out, 2, 0.05, 0.2) < 1e-6);

  // with music playing, the cue mix does not pick up the microphone through the master feed either
  e->load(0, track(30, 330, 0.4f));
  e->send(cmd(Op::PlayToggle));
  e->send(cmd(Op::HeadMix, 1.0f));  // headphones follow the master mix
  run(*e, 0.3, s);
  out = run(*e, 0.5, s);
  CHECK(tone(out, 2, 1000, 0.1, 0.5) < 0.005 && tone(out, 2, 330, 0.1, 0.5) > 0.2);
  CHECK(tone(out, 0, 1000, 0.1, 0.5) > 0.25);  // but the master has both
  std::puts("mic routing: ok");
}

void test_filters() {
  auto e = engine();
  e->send(cmd(Op::MicOn, 0, 1));
  const double g40 = path_gain(*e, 40), g80 = path_gain(*e, 80), g1k = path_gain(*e, 1000), g8k = path_gain(*e, 8000);
  std::printf("high-pass: 40 Hz %.3f, 80 Hz %.3f, 1 kHz %.4f, 8 kHz %.4f\n", g40, g80, g1k, g8k);
  CHECK(std::fabs(g40 - 0.2425) < 0.01);   // 2nd-order Butterworth at 80 Hz: x^2 / sqrt(1 + x^4), x = f / 80
  CHECK(std::fabs(g80 - 0.7071) < 0.02);   // -3 dB at the corner
  CHECK(std::fabs(g1k - 1.0) < 0.01 && std::fabs(g8k - 1.0) < 0.01);

  const double lo_flat = path_gain(*e, 100), hi_flat = path_gain(*e, 8000);
  e->send(cmd(Op::MicEqLow, 1.0f));
  const double lo_up = path_gain(*e, 100) / lo_flat, lo_up_mid = path_gain(*e, 1000);
  e->send(cmd(Op::MicEqLow, 0.0f));
  const double lo_down = path_gain(*e, 100) / lo_flat;
  e->send(cmd(Op::MicEqLow, 0.5f));
  e->send(cmd(Op::MicEqHigh, 1.0f));
  const double hi_up = path_gain(*e, 8000) / hi_flat, hi_up_mid = path_gain(*e, 1000);
  e->send(cmd(Op::MicEqHigh, 0.0f));
  const double hi_down = path_gain(*e, 8000) / hi_flat;
  e->send(cmd(Op::MicEqHigh, 0.5f));
  std::printf("EQ: low +12 dB at 100 Hz x%.2f (1 kHz x%.2f), low min x%.2f; high +12 dB at 8 kHz x%.2f (1 kHz x%.2f), high min x%.2f\n",
              lo_up, lo_up_mid, lo_down, hi_up, hi_up_mid, hi_down);
  CHECK(lo_up > 2.4 && lo_up < 3.9 && lo_down > 0.25 && lo_down < 0.42);  // +-12 dB shelf, about 1 octave below its corner
  CHECK(hi_up > 3.2 && hi_up < 4.1 && hi_down > 0.24 && hi_down < 0.32);
  CHECK(lo_up_mid > 0.95 && lo_up_mid < 1.25 && hi_up_mid > 0.95 && hi_up_mid < 1.25);  // the mid range stays out of it
  { Src idle; run(*e, 0.01, idle); }
  CHECK(e->mixer().mic_eq_low == 0.5f && e->mixer().mic_eq_high == 0.5f);
  std::puts("mic high-pass and EQ: ok");
}

void test_gate() {
  auto e = engine();
  e->send(cmd(Op::MicOn, 0, 1));
  e->send(cmd(Op::MicGate, -40.0f));
  Src quiet{1000, 0.005};  // -46 dB: below the threshold
  run(*e, 0.2, quiet);
  auto out = run(*e, 0.5, quiet);
  CHECK(e->mixer().mic_gate_db == -40.0f);
  CHECK(peak(out, 0, 0.1, 0.5) < 2e-4);
  Src loud{1000, 0.1};
  run(*e, 0.3, loud);
  out = run(*e, 0.5, loud);
  CHECK(std::fabs(tone(out, 0, 1000, 0.1, 0.5) - 0.1) < 0.003);
  out = run(*e, 1.0, quiet);  // hold time, then the gate closes again
  CHECK(peak(out, 0, 0.6, 1.0) < 2e-4);
  e->send(cmd(Op::MicGate, -80.0f));  // off: quiet passes
  run(*e, 0.3, quiet);
  out = run(*e, 0.5, quiet);
  CHECK(std::fabs(tone(out, 0, 1000, 0.1, 0.5) - 0.005) < 0.0003);
  std::puts("mic gate: ok");
}

// Gain of the music (a DC track, so a sample-exact read) over time after a talkover change.
void test_talkover_timing() {
  auto e = engine();
  e->load(0, track(60, 0, 0.5f));
  e->send(cmd(Op::PlayToggle));
  Src none;
  run(*e, 0.3, none);
  const auto ref = run(*e, 0.1, none);
  const double base = ref[ref.size() / 2 / kCh * kCh];
  CHECK(std::fabs(base - 0.5) < 1e-3);
  const auto at = [&](const std::vector<float>& b, double t) { return b[static_cast<std::size_t>(t * kSr) * kCh] / base; };

  const double depth_db = -12.0, d = std::pow(10.0, depth_db / 20.0);
  e->send(cmd(Op::MicTalkoverDepth, static_cast<float>(depth_db)));
  e->send(cmd(Op::MicTalkover, 0, 2));  // always
  const auto down = run(*e, 1.0, none);
  const double tau_a = kMicTalkoverAttack, tau_r = kMicTalkoverRelease;
  for (const double t : {0.01, 0.025, 0.05, 0.1}) {
    const double expect = 1 - (1 - d) * (1 - std::exp(-t / tau_a));
    std::printf("attack t=%.3f s: gain %.4f (expected %.4f)\n", t, at(down, t), expect);
    CHECK(std::fabs(at(down, t) - expect) < 0.01);
  }
  CHECK(std::fabs(at(down, 0.99) - d) < 0.005);
  CHECK(e->mixer().mic_ducking && std::fabs(e->mixer().mic_duck_db - depth_db) < 0.2 && e->mixer().mic_talkover == 2);

  e->send(cmd(Op::MicTalkover, 0, 0));
  const auto up = run(*e, 4.0, none);
  for (const double t : {0.1, 0.25, 0.5, 1.0, 2.0}) {
    const double expect = 1 - (1 - d) * std::exp(-t / tau_r);
    std::printf("release t=%.2f s: gain %.4f (expected %.4f)\n", t, at(up, t), expect);
    CHECK(std::fabs(at(up, t) - expect) < 0.01);
  }
  CHECK(at(up, 3.99) > 0.995 && !e->mixer().mic_ducking && e->mixer().mic_talkover == 0);

  // another depth, and the clamps
  e->send(cmd(Op::MicTalkoverDepth, -24.0f));
  e->send(cmd(Op::MicTalkover, 0, 2));
  auto deep = run(*e, 1.0, none);
  CHECK(std::fabs(at(deep, 0.99) - std::pow(10.0, -24.0 / 20)) < 0.003);
  e->send(cmd(Op::MicTalkoverDepth, -90.0f));
  run(*e, 0.01, none);
  CHECK(e->mixer().mic_depth_db == -24.0f);
  e->send(cmd(Op::MicTalkoverDepth, 6.0f));
  e->send(cmd(Op::MicTalkover, 0, 2));
  deep = run(*e, 5.0, none);  // recovers from the -24 dB of before while "always" holds a 0 dB depth
  CHECK(e->mixer().mic_depth_db == 0.0f && std::fabs(at(deep, 4.99) - 1.0) < 0.003);  // 0 dB: no ducking
  e->send(cmd(Op::MicTalkover, 0, -1));  // toggle from "always" goes off
  run(*e, 0.01, none);
  CHECK(e->mixer().mic_talkover == 0);
  e->send(cmd(Op::MicTalkover, 0, -1));
  run(*e, 0.01, none);
  CHECK(e->mixer().mic_talkover == 1);
  std::puts("talkover timing: ok");
}

// Voice-activated talkover with the microphone above / below the threshold, music measured by its own frequency.
void test_talkover_voice() {
  auto e = engine();
  e->load(0, track(60, 440, 0.5f));
  e->send(cmd(Op::PlayToggle));
  e->send(cmd(Op::MicOn, 0, 1));
  e->send(cmd(Op::MicTalkoverDepth, -12.0f));
  Src none;
  run(*e, 0.3, none);
  const double ref = tone(run(*e, 0.5, none), 0, 440, 0.0, 0.5);
  CHECK(std::fabs(ref - 0.5) < 0.02);
  const double d = std::pow(10.0, -12.0 / 20);

  e->send(cmd(Op::MicTalkover, 0, 1));
  auto out = run(*e, 0.5, none);  // armed, nobody speaks
  CHECK(std::fabs(tone(out, 0, 440, 0.1, 0.5) / ref - 1.0) < 0.02 && !e->mixer().mic_ducking);

  Src whisper{3000, 0.005};  // -46 dB: under the -40 dB threshold
  out = run(*e, 0.8, whisper);
  CHECK(std::fabs(tone(out, 0, 440, 0.3, 0.8) / ref - 1.0) < 0.02);

  Src voice{3000, 0.2};
  run(*e, 0.3, voice);
  out = run(*e, 0.5, voice);
  const double ducked = tone(out, 0, 440, 0.0, 0.5) / ref;
  std::printf("voice-activated talkover: music at %.4f of its level (expected %.4f)\n", ducked, d);
  CHECK(std::fabs(ducked - d) < 0.012 && e->mixer().mic_ducking);
  CHECK(tone(out, 0, 3000, 0.0, 0.5) > 0.18);  // the voice itself is not ducked

  out = run(*e, 0.2, none);  // voice stops: still held down for a moment
  CHECK(tone(out, 0, 440, 0.0, 0.2) / ref < 0.45);
  out = run(*e, 4.0, none);
  CHECK(tone(out, 0, 440, 3.0, 4.0) / ref > 0.98 && !e->mixer().mic_ducking);

  // the microphone switched off ducks nothing (no signal), talkover "always" ducks regardless
  e->send(cmd(Op::MicOn, 0, 0));
  out = run(*e, 4.0, voice);  // (the duck of the first moments releases after the detector's hold)
  CHECK(std::fabs(tone(out, 0, 440, 3.0, 4.0) / ref - 1.0) < 0.02);
  std::puts("voice-activated talkover: ok");
}

// ---- recording and the stream tap ----------------------------------------------------------------------------
double wav_peak(const std::string& path, int ch) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  CHECK(f);
  std::vector<unsigned char> b(static_cast<std::size_t>(std::filesystem::file_size(path)));
  CHECK(std::fread(b.data(), 1, b.size(), f) == b.size());
  std::fclose(f);
  CHECK(b.size() > 80 && std::memcmp(b.data(), "RIFF", 4) == 0);
  double p = 0;
  for (std::size_t i = 80 + static_cast<std::size_t>(ch) * 3; i + 3 <= b.size(); i += 6) {
    std::int32_t v = b[i] | (b[i + 1] << 8) | (b[i + 2] << 16);
    if (v & 0x800000) v -= 0x1000000;
    p = std::max(p, std::fabs(v / 8388607.0));
  }
  return p;
}

double stream_tone(Engine& e, double hz) {
  std::vector<float> buf(static_cast<std::size_t>(kSr) * 4 * 2);
  std::size_t got = 0;
  while (std::size_t n = e.stream_read(buf.data() + got * 2, 4096)) got += n;
  CHECK(got > 24000);
  double re = 0, im = 0;
  const std::size_t from = 4800;
  for (std::size_t i = from; i < got; ++i) {
    const double w = 2 * kPi * hz * static_cast<double>(i) / kSr;
    re += buf[i * 2] * std::cos(w);
    im += buf[i * 2] * std::sin(w);
  }
  return 2 * std::hypot(re, im) / static_cast<double>(got - from);
}

void test_taps() {
  const std::string dir = (std::filesystem::temp_directory_path() / "deckhand_test_mic").string();
  std::filesystem::create_directories(dir);
  auto e = engine();
  e->send(cmd(Op::MicOn, 0, 1));
  Src s{1000, 0.3};
  run(*e, 0.2, s);
  std::string err;

  // recording and stream tap include the microphone by default
  CHECK(e->record_open(dir + "/with.wav", false, err));
  e->stream_enable(true);
  run(*e, 1.0, s);
  e->record_stop();
  run(*e, 0.05, s);
  e->record_close();
  const double wp = wav_peak(dir + "/with.wav", 0);
  const double st = stream_tone(*e, 1000);
  std::printf("mic in recording: peak %.4f; in stream tap: %.4f\n", wp, st);
  CHECK(wp > 0.29 && wp < 0.31 && std::fabs(st - 0.3) < 0.005);
  e->stream_enable(false);

  // ... and can be left out; the music is then not ducked either (clean music-only tap)
  e->load(0, track(60, 440, 0.5f));
  e->send(cmd(Op::PlayToggle));
  e->send(cmd(Op::MicTalkoverDepth, -12.0f));
  e->send(cmd(Op::MicTalkover, 0, 2));
  run(*e, 1.0, s);
  CHECK(e->record_open(dir + "/without.wav", false, err, false));
  e->stream_enable(true, false, false);
  const auto out = run(*e, 1.0, s);
  e->record_stop();
  run(*e, 0.05, s);
  e->record_close();
  const double wp2 = wav_peak(dir + "/without.wav", 0);
  const double st_music = stream_tone(*e, 440), main_music = tone(out, 0, 440, 0.2, 1.0);
  std::printf("without mic: recording peak %.3f (music 0.5), stream music %.3f, master music (ducked) %.3f\n", wp2, st_music, main_music);
  CHECK(wp2 > 0.45 && wp2 < 0.52);                      // music at full level, no 1 kHz voice (it would add up to 0.8)
  CHECK(std::fabs(st_music - 0.5) < 0.01);
  CHECK(std::fabs(main_music - 0.5 * std::pow(10.0, -12.0 / 20)) < 0.01);
  std::filesystem::remove_all(dir);
  std::puts("mic in recording and stream tap: ok");
}

// ---- the clock-drift buffer ------------------------------------------------------------------------------------
struct DriftResult {
  std::uint64_t underruns = 0, overruns = 0;
  double min_ms = 1e9, max_ms = 0, mean_early = 0, mean_late = 0, freq = 0, min_win_peak = 1e9;
};

// The capture device nominally runs at 48000 Hz but delivers `real_rate` frames per second (a 480-frame burst each
// 480 / real_rate seconds); the output callback asks for 256 frames each 256 / 48000 s. `stall_at` / `stall_len`:
// the device stops delivering for a while (hot-unplug) and then comes back.
DriftResult drift(double real_rate, double seconds, double stall_at = -1, double stall_len = 0, bool* output_finite = nullptr) {
  auto e = engine();
  e->configure_offline_input(static_cast<std::uint32_t>(kSr));
  e->send(cmd(Op::MicOn, 0, 1));
  const int burst = 480, block = 256;
  double phase = 0, t_in = 0, t_out = 0;
  std::vector<float> in(burst * 2), out(static_cast<std::size_t>(block) * kCh);
  DriftResult r;
  const std::size_t blocks = static_cast<std::size_t>(seconds * kSr / block);
  std::vector<float> mic(blocks * block);
  std::size_t early_n = 0, late_n = 0;
  bool finite = true;
  std::uint64_t reported_underruns = 0;
  for (std::size_t b = 0; b < blocks; ++b) {
    const double t_end = t_out + block / kSr;
    const bool stalled = stall_at >= 0 && t_out >= stall_at && t_out < stall_at + stall_len;
    while (t_in + burst / real_rate <= t_end) {
      if (!stalled) {
        for (int i = 0; i < burst; ++i) {
          in[2 * i] = in[2 * i + 1] = static_cast<float>(0.3 * std::sin(phase));
          phase += 2 * kPi * 1000.0 / real_rate;
        }
        e->feed_input(in.data(), burst);
      }
      t_in += burst / real_rate;
    }
    e->render(out.data(), block);
    for (int i = 0; i < block; ++i) {
      mic[b * block + static_cast<std::size_t>(i)] = out[static_cast<std::size_t>(i) * kCh];
      if (!std::isfinite(out[static_cast<std::size_t>(i) * kCh])) finite = false;
    }
    t_out = t_end;
    const InputStatus s = e->input_status();
    if (t_out > 5.0 && stall_at < 0) {
      r.min_ms = std::min(r.min_ms, s.latency_ms);
      r.max_ms = std::max(r.max_ms, s.latency_ms);
    }
    if (t_out > 5 && t_out <= 15) {
      r.mean_early += s.latency_ms;
      ++early_n;
    }
    if (t_out > seconds - 10) {
      r.mean_late += s.latency_ms;
      ++late_n;
    }
    reported_underruns = s.underruns;
    r.overruns = s.overruns;
  }
  r.underruns = reported_underruns;
  if (early_n) r.mean_early /= static_cast<double>(early_n);
  if (late_n) r.mean_late /= static_cast<double>(late_n);
  if (output_finite) *output_finite = finite;
  // continuity: the peak in every 10 ms window of the last half of the run
  const std::size_t w = 480;
  for (std::size_t i = mic.size() / 2; i + w <= mic.size(); i += w) {
    float p = 0;
    for (std::size_t k = 0; k < w; ++k) p = std::max(p, std::fabs(mic[i + k]));
    r.min_win_peak = std::min<double>(r.min_win_peak, p);
  }
  // pitch of the last 10 s from positive-going zero crossings
  double first = -1, last = -1;
  int count = 0;
  for (std::size_t i = mic.size() - static_cast<std::size_t>(10 * kSr) + 1; i < mic.size(); ++i) {
    const float a = mic[i - 1], c = mic[i];
    if (a < 0 && c >= 0) {
      const double t = static_cast<double>(i - 1) + static_cast<double>(-a) / static_cast<double>(c - a);
      if (first < 0) first = t;
      last = t;
      ++count;
    }
  }
  r.freq = count > 1 ? (count - 1) * kSr / (last - first) : 0;
  return r;
}

void test_drift() {
  for (const double ppm : {+5000.0, -5000.0, 0.0, +200.0}) {
    const double real = kSr * (1 + ppm * 1e-6);
    const DriftResult r = drift(real, 60.0);
    std::printf("capture clock %+5.0f ppm, 60 s: underruns %llu, overruns %llu, buffer %.1f..%.1f ms (mean %.1f -> %.1f ms), "
                "min 10 ms peak %.3f, output pitch %.3f Hz\n",
                ppm, static_cast<unsigned long long>(r.underruns), static_cast<unsigned long long>(r.overruns), r.min_ms, r.max_ms,
                r.mean_early, r.mean_late, r.min_win_peak, r.freq);
    CHECK(r.underruns == 0 && r.overruns == 0);          // no dropouts
    CHECK(r.min_ms > 5 && r.max_ms < 70);                 // bounded latency (target 30 ms, bursts of 10 ms)
    CHECK(std::fabs(r.mean_late - r.mean_early) < 8);     // the buffer does not creep
    CHECK(r.min_win_peak > 0.27);                         // the signal never dips: no gaps
    CHECK(std::fabs(r.freq - 1000.0) < 0.5);              // resampled onto the output clock, not just buffered
  }
  // the capture device stalls for 300 ms (unplugged) and returns: the output never glitches, the channel recovers
  {
    bool finite = false;
    const DriftResult r = drift(kSr, 12.0, 4.0, 0.3, &finite);
    std::printf("device stall 0.3 s: underruns %llu, finite output %d, signal back (min peak %.3f)\n",
                static_cast<unsigned long long>(r.underruns), finite ? 1 : 0, r.min_win_peak);
    CHECK(finite && r.underruns == 1);
    CHECK(r.min_win_peak > 0.27);  // the scan starts after the stall: the signal is back
  }
  std::puts("mic clock drift: ok");
}

// The music is not touched by a capture device coming and going.
void test_stall_keeps_music() {
  auto e = engine();
  e->load(0, track(60, 440, 0.5f));
  e->send(cmd(Op::PlayToggle));
  e->configure_offline_input(static_cast<std::uint32_t>(kSr));
  e->send(cmd(Op::MicOn, 0, 1));
  std::vector<float> in(480 * 2), out(256 * kCh);
  double phase = 0, t_in = 0, t_out = 0, worst_step = 0, last = 0;
  for (std::size_t b = 0; b < static_cast<std::size_t>(8.0 * kSr / 256); ++b) {
    const double t_end = t_out + 256 / kSr;
    const bool stalled = t_out >= 3.0 && t_out < 3.4;
    while (t_in + 480 / kSr <= t_end) {
      if (!stalled) {
        for (int i = 0; i < 480; ++i) {
          in[2 * i] = in[2 * i + 1] = static_cast<float>(0.3 * std::sin(phase));
          phase += 2 * kPi * 1000.0 / kSr;
        }
        e->feed_input(in.data(), 480);
      }
      t_in += 480 / kSr;
    }
    e->render(out.data(), 256);
    t_out = t_end;
    // the music pair alone: remove the microphone by looking at a master built without it is not possible here, so
    // compare the step size of the whole output: a click would show as a step far above the two sines' slopes
    for (int i = 0; i < 256; ++i) {
      const double v = out[static_cast<std::size_t>(i) * kCh];
      if (t_out > 1.0) worst_step = std::max(worst_step, std::fabs(v - last));
      last = v;
    }
  }
  // 0.5 sin(440) slope <= 0.03 per sample, 0.3 sin(1000) <= 0.04: together well under 0.1
  std::printf("stall: largest sample step %.4f\n", worst_step);
  CHECK(worst_step < 0.1);
  CHECK(e->input_status().underruns == 1);
}

// A null capture device (no hardware, silence) through the real open / close path.
void test_null_device() {
  auto e = engine();
  InputConfig c;
  c.null_backend = true;
  c.channel = 0;
  std::string err;
  CHECK(!e->input_status().open);
  CHECK(e->open_input(c, err));
  InputStatus st = e->input_status();
  CHECK(st.open && !st.lost && st.sample_rate > 0 && !st.name.empty());
  std::vector<float> out(256 * kCh);
  e->send(cmd(Op::MicOn, 0, 1));
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    e->render(out.data(), 256);
  }
  CHECK(e->mixer().mic_present);
  c.channel = 5;  // the null device has two channels
  CHECK(!e->open_input(c, err) && err.find("channel") != std::string::npos);
  CHECK(!e->input_status().open && e->input_status().lost);  // it keeps retrying quietly
  e->close_input();
  CHECK(!e->input_status().open && !e->input_status().lost);
  e->render(out.data(), 256);
  CHECK(!e->mixer().mic_present);
  c.channel = 0;
  CHECK(e->open_input(c, err) && e->input_status().open);
  e->close_input();
  std::puts("null capture device: ok");
}

}  // namespace

int main() {
  test_routing_and_gain();
  test_filters();
  test_gate();
  test_talkover_timing();
  test_talkover_voice();
  test_taps();
  test_drift();
  test_stall_keeps_music();
  test_null_device();
  std::puts("engine mic ok");
  return 0;
}
