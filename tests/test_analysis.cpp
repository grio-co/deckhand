// Track analysis tests on synthetic signals (and the demo tracks when present).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/analysis/analysis.hpp"
#include "deckhand/audio/track.hpp"

using namespace deckhand;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kRate = 44100;
constexpr int kDownbeatMinFraction = 0;  // percent; raised once the estimator is measured

struct Pcm {
  std::vector<float> d;
  std::uint64_t frames = 0;
  explicit Pcm(double seconds) : d(static_cast<std::size_t>(seconds * kRate) * 2, 0.f), frames(d.size() / 2) {}
  void add(double t, std::size_t i, float v) {
    const std::size_t idx = static_cast<std::size_t>(t * kRate) + i;
    if (idx < frames) {
      d[2 * idx] += v;
      d[2 * idx + 1] += v;
    }
  }
};

std::uint32_t g_seed = 12345;
float noise() {
  g_seed = g_seed * 1664525u + 1013904223u;
  return float(int32_t(g_seed)) / 2147483648.f;
}

void add_click(Pcm& p, double t, float amp) {
  const std::size_t n = kRate / 50;
  for (std::size_t i = 0; i < n; ++i)
    p.add(t, i, amp * float(std::sin(2 * kPi * 1500.0 * i / kRate) * std::exp(-double(i) / (0.003 * kRate))));
}
void add_kick(Pcm& p, double t, float amp) {
  const std::size_t n = kRate / 4;
  double ph = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double x = double(i) / kRate;
    const double f = 45.0 + 105.0 * std::exp(-x / 0.03);
    ph += 2 * kPi * f / kRate;
    float v = float(std::sin(ph) * std::exp(-x / 0.09));
    if (i < 100) v += 0.5f * noise() * float(1.0 - i / 100.0);
    p.add(t, i, amp * 0.8f * v);
  }
}
void add_hat(Pcm& p, double t, float amp) {
  const std::size_t n = kRate / 33;
  float prev = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const float n0 = noise();
    const float hp = n0 - prev;  // crude high-pass
    prev = n0;
    p.add(t, i, amp * hp * 0.3f * float(std::exp(-double(i) / (0.008 * kRate))));
  }
}
void add_note(Pcm& p, double t, double dur, double freq, float amp, int harmonics = 4) {
  const std::size_t n = static_cast<std::size_t>(dur * kRate);
  const std::size_t ramp = kRate / 100;
  for (std::size_t i = 0; i < n; ++i) {
    float env = 1.f;
    if (i < ramp) env = float(i) / ramp;
    else if (n - i < ramp) env = float(n - i) / ramp;
    float v = 0;
    for (int h = 1; h <= harmonics; ++h) {
      if (freq * h > 8000) break;
      v += float(std::sin(2 * kPi * freq * h * double(i) / kRate)) / float(h);
    }
    p.add(t, i, amp * env * v);
  }
}
double midi_hz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

double phase_err_ms(double first_ms, double period_ms, double offset_ms) {
  double d = std::fmod(first_ms - offset_ms, period_ms);
  if (d > period_ms / 2) d -= period_ms;
  if (d < -period_ms / 2) d += period_ms;
  return std::fabs(d);
}

Pcm make_beat_track(double bpm, double offset, double seconds, bool kick) {
  Pcm p(seconds);
  const double T = 60.0 / bpm;
  for (int k = 0; offset + k * T < seconds - 0.3; ++k) {
    const double t = offset + k * T;
    if (kick) {
      add_kick(p, t, 1.0f);
      add_hat(p, t + T / 2, 0.7f);
      if (k % 2 == 1) add_note(p, t, 0.12, 400.0, 0.1f, 2);  // off-beat clap-ish tone
    } else {
      add_click(p, t, 0.8f);
    }
  }
  return p;
}

void test_steady(double bpm, bool kick) {
  const double offset = 0.237;
  Pcm p = make_beat_track(bpm, offset, 30.0, kick);
  BeatGrid g = analyse_beats(p.d.data(), p.frames, kRate);
  std::printf("  %s %.0f BPM: ok=%d sections=%zu bpm=%.3f first=%.1f ms phase_err=%.1f ms\n",
              kick ? "kick " : "click", bpm, g.ok, g.sections.size(), g.sections.empty() ? 0 : g.sections[0].bpm,
              g.sections.empty() ? 0 : g.sections[0].first_beat_ms,
              g.sections.empty() ? 0 : phase_err_ms(g.sections[0].first_beat_ms, 60000.0 / bpm, offset * 1000));
  CHECK(g.ok);
  CHECK(g.sections.size() == 1);
  CHECK(std::fabs(g.sections[0].bpm - bpm) < 0.05);
  CHECK(phase_err_ms(g.sections[0].first_beat_ms, 60000.0 / bpm, offset * 1000) < 10.0);
  CHECK(g.sections[0].first_beat_ms >= 0 && g.sections[0].first_beat_ms < 60000.0 / bpm + 1);
  CHECK(g.beats_ms.size() > 20);
}

void test_downbeat() {
  const double bpm = 124.0, T = 60.0 / bpm, down0 = 1.3;
  Pcm p(34.0);
  for (int k = -8; down0 + k * T < 33.6; ++k) {
    const double t = down0 + k * T;
    if (t < 0) continue;
    const int beat = ((k % 4) + 4) % 4;  // 0 = downbeat
    add_kick(p, t, beat == 0 ? 1.0f : 0.6f);
    add_hat(p, t + T / 2, 0.5f);
    if (beat == 0) {
      const int bar = (k >= 0 ? k : k - 3) / 4;
      static const double bass[4] = {45, 48, 43, 50};  // midi notes, change every bar
      add_note(p, t, 4 * T * 0.95, midi_hz(bass[((bar % 4) + 4) % 4]), 0.35f, 3);
    }
  }
  BeatGrid g = analyse_beats(p.d.data(), p.frames, kRate);
  CHECK(g.ok);
  CHECK(!g.sections.empty());
  const double first = g.sections[0].first_beat_ms;
  const long idx = std::lround((first - down0 * 1000) / (T * 1000));
  const int expect = static_cast<int>(((idx % 4) + 4) % 4) + 1;
  std::printf("  downbeat: bar_beat=%d expect=%d conf=%.2f bpm=%.2f\n", g.sections[0].bar_beat, expect,
              g.downbeat_confidence, g.sections[0].bpm);
  CHECK(std::fabs(g.sections[0].bpm - bpm) < 0.05);
  CHECK(g.sections[0].bar_beat == expect);
  CHECK(g.downbeat_confidence > 0.1);
}


void add_snare(Pcm& p, double t, float amp) {
  const std::size_t n = kRate / 7;
  for (std::size_t i = 0; i < n; ++i) {
    const double x = double(i) / kRate;
    const float v = noise() * float(std::exp(-x / 0.045)) + 0.5f * float(std::sin(2 * kPi * 190.0 * x) * std::exp(-x / 0.06));
    p.add(t, i, amp * 0.5f * v);
  }
}
void add_crash(Pcm& p, double t, float amp) {
  const std::size_t n = kRate * 6 / 5;
  float prev = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const float n0 = noise();
    const float hp = n0 - prev;
    prev = n0;
    p.add(t, i, amp * 0.5f * hp * float(std::exp(-double(i) / (0.35 * kRate))));
  }
}

// Error (ms) of the first grid beat against a known beat time (mod beat period), and drift at the last beat.
struct GridErr {
  double first = 1e9, last = 1e9, worst = 1e9;
};
GridErr grid_error(const BeatGrid& g, double bpm, double offset_s, double end_s) {
  GridErr e;
  if (g.beats_ms.empty()) return e;
  const double T = 60000.0 / bpm, off = offset_s * 1000.0;
  e.worst = 0;
  bool have_first = false;
  for (double b : g.beats_ms) {
    const double k = std::round((b - off) / T);
    const double err = b - (off + k * T);
    if (b < off - 0.5 * T || b > end_s * 1000.0) continue;
    if (!have_first && b >= off - 5.0) { e.first = err; have_first = true; }
    e.last = err;
    e.worst = std::max(e.worst, std::fabs(err));
  }
  return e;
}

void test_grid_drift(double bpm, double seconds, double offset) {
  Pcm p = make_beat_track(bpm, offset, seconds, true);
  const auto t0 = std::chrono::steady_clock::now();
  BeatGrid g = analyse_beats(p.d.data(), p.frames, kRate);
  const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const GridErr e = grid_error(g, bpm, offset, seconds - 0.3);
  std::printf("  drift %.2f BPM %.0f s: ok=%d bpm=%.3f first_err=%.2f last_err=%.2f worst=%.2f ms (%.2f s)\n", bpm,
              seconds, g.ok, g.bpm, e.first, e.last, e.worst, dt);
  CHECK(g.ok);
  CHECK(std::fabs(g.bpm - bpm) < 0.02);
  CHECK(std::fabs(e.first) < 6.0);
  CHECK(std::fabs(e.last) < 6.0);
  CHECK(e.worst < 8.0);
}

// Beat track whose beat-less lead-in is a pad (or silence/noise) before the kicks start.
void test_intro(const char* what, double bpm, double lead_s, int kind, bool hats = true, double expect_bpm = 0) {
  if (expect_bpm <= 0) expect_bpm = bpm;
  const double seconds = 60.0;
  Pcm p(seconds);
  const double T = 60.0 / bpm;
  if (kind == 1) {  // pad: slow-attack chord
    for (double t = 0; t < lead_s; t += 4.0) {
      add_note(p, t, std::min(4.0, lead_s - t), midi_hz(57), 0.25f, 3);
      add_note(p, t, std::min(4.0, lead_s - t), midi_hz(64), 0.2f, 3);
    }
  } else if (kind == 2) {  // low noise floor
    for (std::uint64_t i = 0; i < std::uint64_t(lead_s * kRate) && i < p.frames; ++i) {
      p.d[2 * i] = p.d[2 * i + 1] = 0.0004f * noise();
    }
  }
  for (int k = 0; lead_s + k * T < seconds - 0.4; ++k) {
    const double t = lead_s + k * T;
    add_kick(p, t, 1.0f);
    if (hats) {
      add_hat(p, t + T / 2, 0.6f);
      if (k % 2 == 1) add_note(p, t, 0.12, 400.0, 0.1f, 2);
    }
  }
  AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
  const GridErr e = grid_error(r.beats, expect_bpm, lead_s, seconds - 0.4);
  std::printf("  %s: bpm=%.3f (want %.2f) first_err=%.2f worst=%.2f ms, first_audible=%.1f cue=%.1f (kick at %.1f)\n", what,
              r.beats.bpm, expect_bpm, e.first, e.worst, r.first_audible_ms, r.suggested_cue_ms, lead_s * 1000);
  CHECK(r.beats.ok);
  CHECK(std::fabs(r.beats.bpm - expect_bpm) < 0.05);
  CHECK(e.worst < 8.0);
  CHECK(r.has_audible);
  CHECK(r.suggested_cue_ms >= 0 && r.suggested_cue_ms <= r.duration_ms);
  if (kind == 1) {
    CHECK(r.first_audible_ms < 150.0);  // the pad is sound: that is the start
  } else {
    CHECK(std::fabs(r.first_audible_ms - lead_s * 1000) < 25.0);
    CHECK(std::fabs(r.suggested_cue_ms - lead_s * 1000) < 25.0);  // the first kick, which is also a beat
  }
}

// ---- downbeat set -------------------------------------------------------------------------------------
// Pattern: kick on 1 (strong) and 3 (softer), snare on 2 and 4, off-beat hat, bass note per bar, crash on the first
// beat of every 4th bar. `start_pos` cuts the track so the audio starts at that bar position (0..3);
// `pickup` > 0 instead puts that many plain soft kicks (no accents) before bar 1.
struct DbCase {
  double bpm;
  int start_pos;
  int pickup;
  int mode = 0;  // 0 full accent pattern, 1 four-on-floor + snare 2/4, 2 identical kicks, bar-level bass + phrase crash only, 3 identical kicks, constant bass, phrase crash only, 4 identical kicks, bass and crash on phrase starts only
};
bool run_downbeat_case(const DbCase& c, double* conf_out = nullptr) {
  const double T = 60.0 / c.bpm, seconds = 70.0;
  Pcm p(seconds);
  std::uint32_t save = g_seed;
  g_seed = 777u + std::uint32_t(c.start_pos * 31 + c.pickup * 7 + int(c.bpm));
  static const double bass[4] = {45, 48, 43, 50};
  double first_audio = 0.3, bar0;  // time of the true first downbeat
  int beat0;                       // global beat index (0 = a downbeat) at the first audible beat
  if (c.pickup > 0) {
    beat0 = -c.pickup;
    bar0 = first_audio + c.pickup * T;
  } else {
    beat0 = c.start_pos;
    bar0 = first_audio - c.start_pos * T;
  }
  for (int g = beat0; first_audio + (g - beat0) * T < seconds - 0.5; ++g) {
    const double t = first_audio + (g - beat0) * T;
    if (g < 0) {  // pickup beats: plain, unaccented
      add_kick(p, t, 0.6f);
      add_hat(p, t + T / 2, 0.5f);
      continue;
    }
    const int pos = g % 4, bar = g / 4;
    if (c.mode == 0) {
      add_kick(p, t, pos == 0 ? 1.0f : (pos == 2 ? 0.75f : 0.0f));
      if (pos == 1 || pos == 3) add_snare(p, t, 0.8f);
    } else if (c.mode == 1) {
      add_kick(p, t, 0.9f);
      if (pos == 1 || pos == 3) add_snare(p, t, 0.8f);
    } else {
      add_kick(p, t, 0.9f);
    }
    if (c.mode == 4) {
      if (pos == 0 && bar % 4 == 0) {
        add_note(p, t, 16 * T * 0.95, midi_hz(bass[0]), 0.3f, 3);
        add_crash(p, t, 0.7f);
      }
      add_hat(p, t + T / 2, 0.5f);
      continue;
    }
    add_hat(p, t + T / 2, 0.5f);
    if (pos == 0) add_note(p, t, 4 * T * 0.95, midi_hz(bass[c.mode == 3 ? 0 : bar % 4]), 0.3f, 3);
    if (pos == 0 && bar % 4 == 0) add_crash(p, t, 0.7f);
  }
  g_seed = save;
  BeatGrid g = analyse_beats(p.d.data(), p.frames, kRate);
  if (conf_out) *conf_out = g.downbeat_confidence;
  if (!g.ok || g.beats_ms.size() <= std::size_t(g.downbeat_phase)) return false;
  const double D = g.beats_ms[std::size_t(g.downbeat_phase)];
  return phase_err_ms(D, 4 * T * 1000.0, bar0 * 1000.0) < 25.0;
}

void test_downbeat_set() {
  int total_all = 0, good_all = 0;
  for (int mode = 0; mode < 5; ++mode) {
  int total = 0, good = 0;
  for (double bpm : {100.0, 120.0, 128.0, 140.0}) {
    for (int pos = 0; pos < 4; ++pos) {
      for (int pick : {0, 1, 2, 3}) {
        if (pick > 0 && pos != 0) continue;
        if (pick == 0 || pos == 0) {
          DbCase c{bpm, pos, pick, mode};
          double conf = 0;
          const bool ok = run_downbeat_case(c, &conf);
          ++total;
          good += ok;
          if (!ok) std::printf("    MISS mode=%d bpm=%.0f start_pos=%d pickup=%d conf=%.2f\n", mode, bpm, pos, pick, conf);
        }
      }
    }
  }
  std::printf("  downbeat set mode %d: %d / %d correct\n", mode, good, total);
  total_all += total;
  good_all += good;
  }
  std::printf("  downbeat set overall: %d / %d correct\n", good_all, total_all);
  CHECK(good_all >= total_all * kDownbeatMinFraction / 100);
}


// Kick track of `body_s` seconds starting at `start_s`, optional fade-in, plus `tail_s` of silence after.
Pcm make_start_track(double start_s, double body_s, double tail_s, double bpm = 124.0, double fade_s = 0.0) {
  Pcm p(start_s + body_s + tail_s);
  const double T = 60.0 / bpm;
  for (int k = 0; start_s + k * T < start_s + body_s - 0.3; ++k) {
    const double t = start_s + k * T;
    float amp = 1.0f;
    if (fade_s > 0) amp = float(std::min(1.0, (t - start_s) / fade_s)) * 0.98f + 0.02f;
    add_kick(p, t, amp);
    add_hat(p, t + T / 2, 0.6f * amp);
  }
  return p;
}

void test_audible() {
  // Digital silence lead-in of odd length and a silent tail.
  {
    Pcm p = make_start_track(3.173, 20.0, 5.0);
    AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
    std::printf("  silent lead/tail: first=%.1f last=%.1f cue=%.1f (kick 3173, body ends %.0f)\n", r.first_audible_ms,
                r.last_audible_ms, r.suggested_cue_ms, (3.173 + 20.0) * 1000);
    CHECK(r.has_audible);
    CHECK(std::fabs(r.first_audible_ms - 3173.0) < 15.0);
    CHECK(r.last_audible_ms > 3173 + 19000 && r.last_audible_ms < 3173 + 20000 + 700);
    CHECK(r.last_audible_ms >= r.first_audible_ms);
    CHECK(r.suggested_cue_ms >= r.first_audible_ms - 15.0 && r.suggested_cue_ms <= r.first_audible_ms + 60000.0 / 124.0);
    CHECK(r.suggested_cue_ms <= r.duration_ms);
  }
  // A lone click/pop long before the music, and a pop with DC offset and dither noise floor: not "the start".
  {
    Pcm p = make_start_track(6.0, 20.0, 1.0);
    for (std::size_t i = 0; i < p.frames; ++i) {
      p.d[2 * i] += 0.08f + 0.0001f * noise();  // DC offset + about -80 dBFS dither
      p.d[2 * i + 1] += 0.08f + 0.0001f * noise();
    }
    for (std::size_t i = 0; i < 3; ++i) p.d[2 * (22050 + i)] += 0.9f;  // 3-sample pop at 0.5 s
    for (std::size_t i = 0; i < 40; ++i) p.d[2 * (88200 + i) + 1] -= 0.3f;  // 1 ms thump at 2 s
    AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
    std::printf("  pop + DC + dither: first=%.1f cue=%.1f (kick 6000)\n", r.first_audible_ms, r.suggested_cue_ms);
    CHECK(std::fabs(r.first_audible_ms - 6000.0) < 15.0);
    CHECK(std::fabs(r.suggested_cue_ms - 6000.0) < 40.0);
  }
  // Noise-floor lead-in at about -50 dB under the music (tape hiss) is not sound.
  {
    Pcm p = make_start_track(4.0, 20.0, 0.0);
    for (std::size_t i = 0; i < std::size_t(4.0 * kRate); ++i) p.d[2 * i] = p.d[2 * i + 1] = 0.004f * noise();
    AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
    std::printf("  hiss lead-in: first=%.1f\n", r.first_audible_ms);
    CHECK(std::fabs(r.first_audible_ms - 4000.0) < 15.0);
  }
  // Fade-in over 10 s: the start is where it becomes clearly audible, early in the fade, not 0 and not the full level.
  {
    Pcm p = make_start_track(0.0, 30.0, 0.0, 124.0, 10.0);
    // Make it a real linear fade of everything, not just per-kick amplitude.
    for (std::size_t i = 0; i < std::size_t(10.0 * kRate); ++i) {
      const float g = float(i) / float(10.0 * kRate);
      p.d[2 * i] *= g;
      p.d[2 * i + 1] *= g;
    }
    AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
    std::printf("  10 s fade-in: first=%.1f cue=%.1f\n", r.first_audible_ms, r.suggested_cue_ms);
    CHECK(r.first_audible_ms > 100.0 && r.first_audible_ms < 2500.0);
  }
  // Quiet track (-40 dBFS peaks) still gets its start found; a faint hum floor does not count.
  {
    Pcm p = make_start_track(2.0, 15.0, 0.0);
    for (float& v : p.d) v *= 0.01f;
    AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
    std::printf("  quiet track: first=%.1f\n", r.first_audible_ms);
    CHECK(std::fabs(r.first_audible_ms - 2000.0) < 15.0);
  }
  // Nothing audible: silence, and pure dither.
  {
    Pcm p(10.0);
    AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
    CHECK(!r.has_audible && r.first_audible_ms == 0 && r.suggested_cue_ms == 0);
    for (float& v : p.d) v = 0.00005f * noise();
    r = analyse(p.d.data(), p.frames, kRate);
    CHECK(!r.has_audible && r.suggested_cue_ms == 0);
    AudibleRange ar = find_audible_range(nullptr, 0, kRate);
    CHECK(!ar.ok);
  }
  // Load cue lands on a confident downbeat when one is within a beat of the first sound.
  {
    const double bpm = 124.0, T = 60000.0 / bpm;
    Pcm p = make_start_track(1.337, 30.0, 0.0, bpm);
    AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
    const double D = r.beats.ok ? r.beats.beats_ms[std::size_t(r.beats.downbeat_phase)] : 0;
    std::printf("  cue on downbeat: first=%.1f cue=%.1f conf=%.2f\n", r.first_audible_ms, r.suggested_cue_ms, r.beats.confidence);
    CHECK(r.beats.ok && r.beats.confidence >= 0.5);
    CHECK(std::fabs(r.suggested_cue_ms - 1337.0) < 40.0);
    CHECK(phase_err_ms(r.suggested_cue_ms, 4 * T, D) < 1.0 || std::fabs(r.suggested_cue_ms - r.first_audible_ms) < 1.0);
  }
}


// ---- fluid grids ---------------------------------------------------------------------------------------
struct FluidStat {
  double max_err = 1e9, med_err = 1e9;
  std::size_t sections = 0;
  double beat_count_ratio = 0;
};
FluidStat fluid_stat(const BeatGrid& g, const std::vector<double>& truth_s) {
  FluidStat f;
  f.sections = g.sections.size();
  if (g.beats_ms.empty() || truth_s.empty()) return f;
  std::vector<double> errs;
  std::size_t gi = 0, in_range = 0;
  for (double t : truth_s) {
    const double ms = t * 1000.0;
    while (gi + 1 < g.beats_ms.size() && g.beats_ms[gi + 1] <= ms) ++gi;
    double e = std::fabs(g.beats_ms[gi] - ms);
    if (gi + 1 < g.beats_ms.size()) e = std::min(e, std::fabs(g.beats_ms[gi + 1] - ms));
    errs.push_back(e);
  }
  for (double b : g.beats_ms)
    if (b >= truth_s.front() * 1000.0 - 1.0 && b <= truth_s.back() * 1000.0 + 1.0) ++in_range;
  std::sort(errs.begin(), errs.end());
  f.max_err = errs.back();
  f.med_err = errs[errs.size() / 2];
  f.beat_count_ratio = double(in_range) / double(truth_s.size());
  return f;
}

// Beat times for a tempo function, starting at `t0`, until `end_s`. Optional per-beat jitter (uniform +-jit_ms).
template <class F>
std::vector<double> make_beats(double t0, double end_s, F bpm_at, double jit_ms, std::uint32_t seed) {
  std::vector<double> v;
  std::uint32_t save = g_seed;
  g_seed = seed;
  double t = t0;
  while (t < end_s - 0.5) {
    v.push_back(t + (jit_ms > 0 ? double(noise()) * jit_ms / 1000.0 : 0.0));
    t += 60.0 / bpm_at(t);
  }
  g_seed = save;
  return v;
}
Pcm render_beats(const std::vector<double>& beats, double seconds, bool groove, std::uint32_t seed) {
  Pcm p(seconds);
  std::uint32_t save = g_seed;
  g_seed = seed;
  for (std::size_t k = 0; k < beats.size(); ++k) {
    add_kick(p, beats[k], 1.0f);
    if (groove) {
      const double T = k + 1 < beats.size() ? beats[k + 1] - beats[k] : 0.5;
      add_hat(p, beats[k] + T / 2, 0.6f);
      if (k % 2 == 1) add_snare(p, beats[k], 0.5f);
    }
  }
  g_seed = save;
  return p;
}

void run_fluid(const char* name, const std::vector<double>& beats, double seconds, bool fluid_opt, bool expect_steady,
               double max_bound, std::size_t min_sections, std::size_t max_sections, bool check) {
  Pcm p = render_beats(beats, seconds, true, 4242);
  AnalysisOptions o;
  o.fluid_grid = fluid_opt;
  const auto t0 = std::chrono::steady_clock::now();
  AnalysisResult r = analyse(p.d.data(), p.frames, kRate, o);
  const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const FluidStat f = fluid_stat(r.beats, beats);
  std::printf("  %-28s fluid_opt=%d: sections=%zu max=%.1f med=%.1f ms count_ratio=%.3f steady=%d stability=%.2f (%.2f s)\n", name,
              fluid_opt, f.sections, f.max_err, f.med_err, f.beat_count_ratio, r.beats.steady, r.beats.stability, dt);
  if (!check) return;
  CHECK(r.beats.ok);
  CHECK(f.max_err <= max_bound);
  CHECK(f.sections >= min_sections && f.sections <= max_sections);
  CHECK(f.beat_count_ratio > 0.98 && f.beat_count_ratio < 1.02);
  CHECK(r.beats.steady == expect_steady);
  CHECK(r.beats.stability >= 0 && r.beats.stability <= 1);
  // Bar position stays continuous: beat i is a downbeat iff (i % 4 == phase); sections' bar_beat must agree.
  std::size_t idx = 0;
  for (std::size_t i = 0; i < r.beats.sections.size(); ++i) {
    const TempoSection& ts = r.beats.sections[i];
    while (idx + 1 < r.beats.beats_ms.size() && std::fabs(r.beats.beats_ms[idx] - ts.first_beat_ms) > 0.5 &&
           r.beats.beats_ms[idx] < ts.first_beat_ms)
      ++idx;
    CHECK(std::fabs(r.beats.beats_ms[idx] - ts.first_beat_ms) < 0.5);
    CHECK(ts.bar_beat == int(((long(idx) - r.beats.downbeat_phase) % 4 + 4) % 4) + 1);
  }
}

void test_fluid(bool check) {
  const bool opts[] = {false, true};
  for (bool fo : opts) {
    const bool chk = check && fo;
    std::printf(" -- fluid_grid %s\n", fo ? "on" : "off");
    {
      auto b = make_beats(0.4, 240.0, [](double) { return 124.0; }, 0.0, 1);
      run_fluid("steady control 124", b, 240.0, fo, true, 3.0, 1, 1, chk);
    }
    {
      auto b = make_beats(0.4, 240.0, [](double t) { return 118.0 + 4.0 * t / 240.0; }, 8.0, 2);
      run_fluid("drummer 118->122 jitter8", b, 240.0, fo, false, 16.0, 3, 32, chk);
    }
    {
      auto b = make_beats(0.4, 150.0, [](double t) { return 100.0 + 20.0 * t / 150.0; }, 0.0, 3);
      run_fluid("accel 100->120", b, 150.0, fo, false, 10.0, 3, 32, chk);
    }
    {
      auto b = make_beats(0.4, 150.0, [](double t) { return 130.0 - 12.0 * t / 150.0; }, 0.0, 4);
      run_fluid("rit 130->118", b, 150.0, fo, false, 10.0, 3, 32, chk);
    }
    {
      auto b = make_beats(0.4, 60.0, [](double) { return 126.0; }, 0.0, 5);
      const double t2 = b.back() + 60.0 / 126.0;
      auto b2 = make_beats(t2, 120.0, [](double) { return 140.0; }, 0.0, 6);
      b.insert(b.end(), b2.begin(), b2.end());
      run_fluid("step 126->140", b, 120.0, fo, false, 10.0, 2, 4, chk);
    }
    {  // steady 124 BPM with a 12 s beat-less breakdown in the middle: must stay ONE section
      auto b = make_beats(0.4, 90.0, [](double) { return 124.0; }, 0.0, 8);
      std::vector<double> kept;
      for (double t : b)
        if (t < 30.0 || t > 42.0) kept.push_back(t);
      Pcm p = render_beats(kept, 90.0, true, 4242);
      for (double t = 30.0; t < 42.0; t += 4.0) {
        add_note(p, t, 4.0, midi_hz(57), 0.2f, 3);
        add_note(p, t, 4.0, midi_hz(64), 0.15f, 3);
      }
      AnalysisOptions o;
      o.fluid_grid = fo;
      AnalysisResult r = analyse(p.d.data(), p.frames, kRate, o);
      std::vector<double> ex;  // beats outside the breakdown
      const FluidStat f = fluid_stat(r.beats, kept);
      std::printf("  %-28s fluid_opt=%d: sections=%zu max=%.1f med=%.1f ms steady=%d\n", "steady + breakdown", fo, f.sections,
                  f.max_err, f.med_err, r.beats.steady);
      if (chk) {
        CHECK(r.beats.ok && f.sections == 1 && r.beats.steady);
        CHECK(f.max_err < 4.0);
      }
    }
    {
      auto b = make_beats(0.4, 120.0, [](double) { return 124.0; }, 0.0, 7);
      for (std::size_t k = 40; k < b.size(); ++k) b[k] -= 0.5 * 60.0 / 124.0;  // one half-beat dropped
      run_fluid("dropped half-beat", b, 120.0, fo, true, 10.0, 1, 3, chk);
    }
  }
}

void test_tempo_change() {
  Pcm p(42.0);
  std::vector<double> beats;
  for (int k = 0; k < 40; ++k) beats.push_back(0.25 + 0.5 * k);
  const double t_start = beats.back() + 0.5;  // 20.25
  for (int k = 0; t_start + k * 60.0 / 126.0 < 41.5; ++k) beats.push_back(t_start + k * 60.0 / 126.0);
  for (double t : beats) {
    add_kick(p, t, 1.0f);
    add_hat(p, t + 0.2, 0.6f);
  }
  BeatGrid g = analyse_beats(p.d.data(), p.frames, kRate);
  std::printf("  tempo change: sections=%zu", g.sections.size());
  for (const auto& s : g.sections) std::printf(" [%.1f ms, %.2f]", s.first_beat_ms, s.bpm);
  std::printf("\n");
  CHECK(g.ok);
  CHECK(g.sections.size() == 2);
  CHECK(std::fabs(g.sections[0].bpm - 120.0) < 0.1);
  CHECK(std::fabs(g.sections[1].bpm - 126.0) < 0.1);
  const double f = g.sections[1].first_beat_ms;
  CHECK(std::fabs(f - 20250) < 25 || std::fabs(f - (20250 + 60000.0 / 126.0)) < 25);
}

struct KeyCase {
  int tonic;
  bool minor;
};

void test_key_progression(int tonic, bool minor) {
  Pcm p(32.0);
  const int base = 60 + tonic;
  // Degrees (semitones above tonic) of the chord roots: I IV V I (i iv V i in minor).
  const int roots[8] = {0, 5, 7, 0, 0, 5, 7, 0};
  for (int c = 0; c < 8; ++c) {
    const double t = c * 4.0;
    const int r = roots[c];
    const bool major_chord = !minor || (r == 7);
    const int third = major_chord ? 4 : 3;
    add_note(p, t, 3.9, midi_hz(base + r), 0.2f);
    add_note(p, t, 3.9, midi_hz(base + r + third), 0.2f);
    add_note(p, t, 3.9, midi_hz(base + r + 7), 0.2f);
    add_note(p, t, 3.9, midi_hz(base + r - 12), 0.25f);
  }
  KeyResult k = detect_key(p.d.data(), p.frames, kRate);
  const int expect = tonic + (minor ? 12 : 0);
  std::printf("  key %s: got %s (%s) conf=%.2f\n", key_name(expect).c_str(), k.name.c_str(), k.wheel.c_str(),
              k.confidence);
  CHECK(k.ok);
  CHECK(k.key_index == expect);
}

void test_key_scale(int tonic, bool minor) {
  Pcm p(32.0);
  const int base = 60 + tonic;
  static const int maj[8] = {0, 2, 4, 5, 7, 9, 11, 12};
  static const int min[8] = {0, 2, 3, 5, 7, 8, 10, 12};
  const int* sc = minor ? min : maj;
  double t = 0;
  for (int rep = 0; rep < 4; ++rep) {
    // tonic pedal under an up-and-down scale
    add_note(p, t, 8.0, midi_hz(base - 12), 0.2f);
    for (int i = 0; i < 8; ++i) add_note(p, t + i * 0.5, 0.48, midi_hz(base + sc[i]), 0.25f);
    for (int i = 6; i >= 0; --i) add_note(p, t + 4.0 + (6 - i) * 0.5, 0.48, midi_hz(base + sc[i]), 0.25f);
    add_note(p, t + 7.5, 0.5, midi_hz(base), 0.3f);
    t += 8.0;
  }
  KeyResult k = detect_key(p.d.data(), p.frames, kRate);
  const int expect = tonic + (minor ? 12 : 0);
  std::printf("  scale %s: got %s conf=%.2f\n", key_name(expect).c_str(), k.name.c_str(), k.confidence);
  CHECK(k.ok);
  CHECK(k.key_index == expect);
}

void test_loudness() {
  Pcm p(10.0);
  for (std::size_t i = 0; i < p.frames; ++i) {
    const float v = 0.1f * float(std::sin(2 * kPi * 1000.0 * double(i) / kRate));
    p.d[2 * i] = v;
    p.d[2 * i + 1] = v;
  }
  LoudnessResult l = measure_loudness(p.d.data(), p.frames, kRate);
  std::printf("  loudness: %.2f LUFS peak %.3f (%.1f dBFS)\n", l.integrated_lufs, l.sample_peak, l.sample_peak_dbfs);
  CHECK(l.ok);
  CHECK(std::fabs(l.integrated_lufs - (-20.0)) < 1.0);
  CHECK(std::fabs(l.sample_peak - 0.1) < 0.001);
  CHECK(std::fabs(auto_gain_db(l, -14.0, -1.0) - 6.0) < 1.0);
}

void test_degenerate() {
  Pcm silence(10.0);
  AnalysisResult r = analyse(silence.d.data(), silence.frames, kRate);
  CHECK(!r.ok);
  CHECK(!r.beats.ok && !r.key.ok && !r.loudness.ok);
  CHECK(!r.error.empty());
  Pcm clip = make_beat_track(120.0, 0.1, 1.0, false);
  r = analyse(clip.d.data(), clip.frames, kRate);
  CHECK(!r.ok);
  CHECK(!r.beats.ok && !r.key.ok);
  CHECK(r.beats.sections.empty());
  r = analyse(nullptr, 0, kRate);
  CHECK(!r.ok);
  Pcm noise_only(10.0);
  for (float& v : noise_only.d) v = 0.1f * noise();
  r = analyse(noise_only.d.data(), noise_only.frames, kRate);  // must not crash
  std::printf("  noise: ok=%d beats=%d key=%d\n", r.ok, r.beats.ok, r.key.ok);
}

void test_speed() {
  Pcm p = make_beat_track(128.0, 0.1, 360.0, true);
  for (std::size_t i = 0; i < p.frames; i += 1) {  // pad with a chord drone so key has content
    (void)i;
    break;
  }
  AnalysisOptions no_phr;
  no_phr.phrases = false;
  const auto t00 = std::chrono::steady_clock::now();
  AnalysisResult r0 = analyse(p.d.data(), p.frames, kRate, no_phr);
  const double s0 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t00).count();
  const auto t0 = std::chrono::steady_clock::now();
  AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  phrases off: %.3f s, on: %.3f s (phrases ok=%d, %zu sections)\n", s0, s, r.phrases.ok, r.phrases.sections.size());
  CHECK(r0.beats.ok);
  const double track_s = static_cast<double>(p.frames) / kRate;
  std::printf("  6-minute track: %.3f s (%.0fx real time), bpm %.2f, sections %zu, ok=%d\n", s, track_s / std::max(s, 1e-9),
              r.beats.bpm, r.beats.sections.size(), r.ok);
  CHECK(r.beats.ok);
  CHECK(std::fabs(r.beats.bpm - 128.0) < 0.05);
  // Speed is a sanity bound, not a benchmark: analysis must stay at least 30x faster than real time (12 s for this
  // 6-minute track). Machines and compilers differ by 3x or more (3.3 s was measured under MSVC Release on a desktop PC), a
  // shared CI runner is slower still, and an accidental O(n^2) would take minutes, so this still catches that.
  CHECK(s < track_s / 30.0);
}

// ---------------------------------------------------------------- phrases and cue suggestions
// Synthetic arrangements built from parts. Real music is much harder (vocals, fills, gradual changes, sections that
// differ only in melody); these numbers show the plumbing works and the heuristics are sane, not how they do on a library.

enum Part { kKick = 1, kHat = 2, kBass = 4, kPad = 8, kRiser = 16, kLead = 32, kClap = 64 };

struct SecSpec {
  const char* label;  // expected rich label
  int bars;
  int parts;
};

struct Layout {
  std::vector<double> start;  // seconds, bar starts (size = bars + 1)
  std::vector<double> beat;   // beat length of each bar, seconds
};

Layout make_layout(double bpm0, double bpm1, double first_bar_s, int bars) {
  Layout L;
  double t = first_bar_s;
  for (int b = 0; b < bars; ++b) {
    const double bpm = bpm0 + (bpm1 - bpm0) * double(b) / std::max(1, bars - 1);
    L.start.push_back(t);
    L.beat.push_back(60.0 / bpm);
    t += 4 * 60.0 / bpm;
  }
  L.start.push_back(t);
  return L;
}

void add_riser(Pcm& p, double t0, double dur, float amp) {
  const std::size_t n = static_cast<std::size_t>(dur * kRate);
  float prev = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const float x = float(i) / float(n);
    const float n0 = noise();
    p.add(t0, i, amp * x * x * (n0 - 0.7f * prev));
    prev = n0;
  }
}

void render_bar(Pcm& p, double t0, double T, int parts, int bar_in_sec, int sec_bars, int chord) {
  static const int chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}};
  static const int roots[4] = {45, 41, 48, 43};
  const bool kick = parts & kKick;
  for (int k = 0; k < 4; ++k) {
    const double t = t0 + k * T;
    if (kick) add_kick(p, t, 0.9f);
    if (parts & kHat) add_hat(p, t + T / 2, 0.6f);
    if ((parts & kClap) && (k == 1 || k == 3)) add_snare(p, t, 0.5f);
    if (parts & kBass) add_note(p, t + T / 2, T * 0.4, midi_hz(roots[chord]), 0.28f, 2);
    if (parts & kLead) {
      add_note(p, t, T * 0.2, midi_hz(chords[chord][(k + bar_in_sec) % 3] + 24), 0.07f, 2);
      add_note(p, t + T / 2, T * 0.2, midi_hz(chords[chord][(k + bar_in_sec + 1) % 3] + 24), 0.07f, 2);
    }
  }
  if (parts & kPad) {
    const float amp = (parts & kKick) ? 0.05f : 0.11f;
    for (int n = 0; n < 3; ++n) add_note(p, t0, 4 * T * 0.99, midi_hz(chords[chord][n]), amp, 3);
  }
  if ((parts & kRiser) && bar_in_sec == 0) add_riser(p, t0, 4 * T * sec_bars, 0.3f);
}

struct Rendered {
  Pcm pcm;
  Layout L;
  AnalysisResult r;
  std::vector<SecSpec> secs;
  std::vector<int> start_bar;  // of each section
  explicit Rendered(double seconds) : pcm(seconds) {}
};

std::vector<SecSpec> classic(int unit, int drop_len, int breakdown_len) {
  return {{"intro", unit, kKick | kHat},
          {"build", unit, kKick | kHat | kPad | kRiser},
          {"drop", drop_len, kKick | kHat | kBass | kPad | kLead | kClap},
          {"breakdown", breakdown_len, kPad},
          {"drop", drop_len, kKick | kHat | kBass | kPad | kLead | kClap},
          {"outro", unit, kKick | kHat}};
}

// pickup_beats: kick-only beats before the first downbeat (audio starts at 0).
std::unique_ptr<Rendered> render_arrangement(double bpm0, double bpm1, const std::vector<SecSpec>& secs, int pickup_beats,
                                             double lead_s) {
  int bars = 0;
  for (const auto& s : secs) bars += s.bars;
  const double T0 = 60.0 / bpm0;
  const double first = pickup_beats > 0 ? pickup_beats * T0 : lead_s;
  Layout L = make_layout(bpm0, bpm1, first, bars);
  auto R = std::make_unique<Rendered>(L.start.back() + 1.0);
  for (int k = 0; k < pickup_beats; ++k) {
    add_kick(R->pcm, k * T0, 0.9f);
    add_hat(R->pcm, k * T0 + T0 / 2, 0.6f);
  }
  int bar = 0;
  for (const auto& s : secs) {
    R->start_bar.push_back(bar);
    for (int b = 0; b < s.bars; ++b, ++bar)
      render_bar(R->pcm, L.start[static_cast<std::size_t>(bar)], L.beat[static_cast<std::size_t>(bar)], s.parts, b, s.bars, bar % 4);
  }
  R->L = std::move(L);
  R->secs = secs;
  return R;
}

struct PhraseScore {
  int bound_total = 0, bound_hit = 0, extra = 0, label_total = 0, label_hit = 0, kind_hit = 0;
};

PhraseScore score_phrases(const char* name, const Rendered& R) {
  PhraseScore sc;
  const PhraseAnalysis& pa = R.r.phrases;
  for (std::size_t i = 1; i < R.secs.size(); ++i) {
    const std::size_t bar = static_cast<std::size_t>(R.start_bar[i]);
    const double want = R.L.start[bar] * 1000.0, beat_ms = R.L.beat[bar] * 1000.0;
    ++sc.bound_total;
    for (const auto& s : pa.sections)
      if (std::fabs(s.start_ms - want) <= beat_ms) {
        ++sc.bound_hit;
        break;
      }
  }
  sc.extra = std::max(0, int(pa.sections.size()) - 1 - sc.bound_hit);
  for (std::size_t i = 0; i < R.secs.size(); ++i) {
    const std::size_t a = static_cast<std::size_t>(R.start_bar[i]);
    const std::size_t b = a + static_cast<std::size_t>(R.secs[i].bars);
    const double mid = 0.5 * (R.L.start[a] + R.L.start[b]) * 1000.0;
    for (const auto& s : pa.sections)
      if (mid >= s.start_ms && mid < s.end_ms) {
        ++sc.label_total;
        if (s.label == R.secs[i].label) ++sc.label_hit;
        break;
      }
  }
  std::printf("  %-28s boundaries %d/%d within a beat, %d extra; labels %d/%d; sections=%zu%s%s\n", name, sc.bound_hit,
              sc.bound_total, sc.extra, sc.label_hit, sc.label_total, pa.sections.size(), pa.gridded ? "" : " (ungridded)",
              R.r.beats.steady ? "" : " (fluid grid)");
  for (const auto& s : pa.sections)
    std::printf("      %7.2f-%7.2f s  bars %3d..%3d  e%-2d %-9s (%.2f) kind=%s\n", s.start_ms / 1000, s.end_ms / 1000, s.start_bar,
                s.end_bar, s.energy, s.label.c_str(), s.label_confidence, s.kind.c_str());
  return sc;
}

const CueSuggestion* find_cue(const std::vector<CueSuggestion>& v, const char* type) {
  for (const auto& c : v)
    if (c.type == type) return &c;
  return nullptr;
}

void check_cue_invariants(const AnalysisResult& r, const std::vector<CueSuggestion>& cues) {
  const double bar_ms = r.phrases.bar_ms;
  int prev_rank = 0;
  for (std::size_t i = 0; i < cues.size(); ++i) {
    CHECK(cues[i].rank == prev_rank + 1);  // ranks consecutive from 1, list sorted by rank
    prev_rank = cues[i].rank;
    CHECK(cues[i].position_ms >= r.first_audible_ms - 1.0 && cues[i].position_ms < r.last_audible_ms);
    for (std::size_t j = i + 1; j < cues.size(); ++j) CHECK(std::fabs(cues[i].position_ms - cues[j].position_ms) >= bar_ms);
  }
}

// Classic arrangement; returns the score. `expect_all` boundaries must be found.
std::unique_ptr<Rendered> run_phrase_track(const char* name, double bpm0, double bpm1, const std::vector<SecSpec>& secs,
                                           int pickup_beats, double lead_s, PhraseScore* out_score) {
  auto R = render_arrangement(bpm0, bpm1, secs, pickup_beats, lead_s);
  R->r = analyse(R->pcm.d.data(), R->pcm.frames, kRate);
  *out_score = score_phrases(name, *R);
  return R;
}

void phrase_basic_checks(const Rendered& R, const PhraseScore& sc, int min_hits) {
  const PhraseAnalysis& pa = R.r.phrases;
  CHECK(pa.ok);
  CHECK(sc.bound_hit >= min_hits);
  // energy ordering: every drop section outranks every breakdown section
  int drop_min = 11, brk_max = 0;
  for (std::size_t i = 0; i < R.secs.size(); ++i) {
    const std::size_t a = static_cast<std::size_t>(R.start_bar[i]);
    const double mid = 0.5 * (R.L.start[a] + R.L.start[a + static_cast<std::size_t>(R.secs[i].bars)]) * 1000.0;
    for (const auto& s : pa.sections)
      if (mid >= s.start_ms && mid < s.end_ms) {
        if (std::string(R.secs[i].label) == "drop") drop_min = std::min(drop_min, s.energy);
        if (std::string(R.secs[i].label) == "breakdown") brk_max = std::max(brk_max, s.energy);
      }
  }
  CHECK(drop_min > brk_max);
  for (const auto& s : pa.sections) CHECK(s.energy >= 1 && s.energy <= 10);
}

// One classic arrangement (intro, build, drop, breakdown, drop, outro) of the given section lengths, checked against where
// the sections really start. Returns the rendering so later tests can reuse it instead of synthesizing it again.
std::unique_ptr<Rendered> test_phrases_classic(double bpm, int unit, int drop_len, int bk_len, int mix_out_bars, PhraseScore* total) {
  char nm[96];
  std::snprintf(nm, sizeof nm, "classic %d bars @%.2f", unit * 3 + drop_len * 2 + bk_len, bpm);
  PhraseScore sc;
  auto R = run_phrase_track(nm, bpm, bpm, classic(unit, drop_len, bk_len), 0, 0.5, &sc);
  total->bound_total += sc.bound_total;
  total->bound_hit += sc.bound_hit;
  total->label_total += sc.label_total;
  total->label_hit += sc.label_hit;
  total->extra += sc.extra;
  phrase_basic_checks(*R, sc, 4);
  const PhraseAnalysis& pa = R->r.phrases;
  CHECK(pa.sections.front().label == "intro");
  CueOptions base;
  base.mix_out_bars = mix_out_bars;
  const auto cues = suggest_cues(R->r, base);
  check_cue_invariants(R->r, cues);
  const auto cues2 = suggest_cues(R->r, base);
  CHECK(cues.size() == cues2.size());
  for (std::size_t i = 0; i < cues.size(); ++i) CHECK(cues[i].position_ms == cues2[i].position_ms && cues[i].type == cues2[i].type);
  const double beat_ms = R->L.beat[0] * 1000.0;
  auto at_bar = [&](int bar) { return R->L.start[static_cast<std::size_t>(bar)] * 1000.0; };
  const int drop1 = 2 * unit, brk = drop1 + drop_len, drop2 = brk + bk_len, outro = drop2 + drop_len;
  const CueSuggestion* mi = find_cue(cues, "mix_in");
  const CueSuggestion* fd = find_cue(cues, "first_drop");
  const CueSuggestion* bk = find_cue(cues, "breakdown");
  const CueSuggestion* ld = find_cue(cues, "last_drop");
  const CueSuggestion* mo = find_cue(cues, "mix_out");
  std::printf("      cues:");
  for (const auto& c : cues) std::printf(" %s%s@%.1fs#%d", c.memory ? "m:" : "", c.type.c_str(), c.position_ms / 1000, c.rank);
  std::printf("\n");
  CHECK(mi && std::fabs(mi->position_ms - at_bar(0)) <= beat_ms);
  CHECK(fd && std::fabs(fd->position_ms - at_bar(drop1)) <= beat_ms);
  CHECK(bk && std::fabs(bk->position_ms - at_bar(brk)) <= beat_ms);
  CHECK(ld && std::fabs(ld->position_ms - at_bar(drop2)) <= beat_ms);
  CHECK(mo && std::fabs(mo->position_ms - at_bar(outro)) <= beat_ms);
  CHECK(mo->position_ms > ld->position_ms && fd->position_ms > mi->position_ms);
  // top-N: asking for 3 gives the 3 best-ranked hot cues
  CueOptions o3 = base;
  o3.max_hot_cues = 3;
  const auto c3 = suggest_cues(R->r, o3);
  int hot = 0;
  for (const auto& c : c3) hot += c.memory ? 0 : 1;
  CHECK(hot == 3);
  for (int i = 0; i < 3; ++i) CHECK(c3[static_cast<std::size_t>(i)].type == cues[static_cast<std::size_t>(i)].type);
  // mix-out distance option: 32 bars before the end must still start >= 32 bars before the last audible bar
  CueOptions o32;
  o32.mix_out_bars = 32;
  const auto c32 = suggest_cues(R->r, o32);
  const CueSuggestion* mo32 = find_cue(c32, "mix_out");
  CHECK(mo32 && mo32->position_ms <= R->L.start.back() * 1000.0 - 32 * 4 * R->L.beat[0] * 1000.0 + beat_ms);
  return R;
}

void test_phrase_habit(const std::unique_ptr<Rendered>& Rp) {
  const std::unique_ptr<Rendered>& R = Rp;  // the classic arrangement synthesized for the section tests
  const PhraseAnalysis& pa = R->r.phrases;
  CueOptions off;
  const auto base = suggest_cues(R->r, off);
  const CueSuggestion* b_drop = find_cue(base, "first_drop");
  CHECK(b_drop != nullptr);
  const double bar_ms = pa.bar_ms;
  // a user who sets a cue 4 bars before the first drop on six tracks (same structure here)
  std::vector<HabitSample> samples;
  for (int i = 0; i < 6; ++i) {
    HabitSample hs;
    hs.phrases = pa;
    hs.cue_ms = {b_drop->position_ms - 4 * bar_ms};
    samples.push_back(hs);
  }
  const CueHabit h = build_cue_habit(samples);
  std::printf("  habit: samples=%d drop_samples=%d mean_bars_before_drop=%.2f hist[-4]=%d\n", h.samples, h.drop_samples,
              h.mean_bars_before_drop, h.bar_offset_hist[12]);
  CHECK(h.drop_samples == 6 && std::fabs(h.mean_bars_before_drop - 4.0) < 0.01);
  CHECK(h.bar_offset_hist[12] == 6);  // offset -4 -> index 12
  CueOptions on;
  on.use_habit = true;
  on.habit = h;
  const auto biased = suggest_cues(R->r, on);
  check_cue_invariants(R->r, biased);
  const CueSuggestion* n_drop = find_cue(biased, "first_drop");
  CHECK(n_drop && std::fabs(n_drop->position_ms - (b_drop->position_ms - 4 * bar_ms)) < 1.0);
  CHECK(n_drop->bar_shift == -4 && std::fabs(n_drop->anchor_ms - b_drop->position_ms) < 1.0);
  // the whole histogram mode (-4) also moves the breakdown, while load and mix-in stay inside the audible range
  const CueSuggestion* n_bk = find_cue(biased, "breakdown");
  const CueSuggestion* b_bk = find_cue(base, "breakdown");
  CHECK(n_bk && b_bk && std::fabs(n_bk->position_ms - (b_bk->position_ms - 4 * bar_ms)) < 1.0);
  const CueSuggestion* n_in = find_cue(biased, "mix_in");
  CHECK(n_in && n_in->position_ms >= R->r.first_audible_ms - 1.0);
  // too few samples: no bias
  CueHabit weak = h;
  weak.samples = 2;
  weak.drop_samples = 2;
  CueOptions w;
  w.use_habit = true;
  w.habit = weak;
  const auto wb = suggest_cues(R->r, w);
  CHECK(find_cue(wb, "first_drop")->position_ms == b_drop->position_ms);
}

void test_phrase_ambient() {
  Pcm p(150.0);
  for (int i = 0; i < 3; ++i) {
    const double t0 = i * 50.0;
    const int chord[3][4] = {{57, 60, 64, 0}, {50, 57, 62, 65}, {45, 52, 57, 0}};
    for (int n = 0; n < 4; ++n)
      if (chord[i][n]) add_note(p, t0, 50.0, midi_hz(chord[i][n]), i == 1 ? 0.12f : 0.06f, 3);
  }
  AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
  const PhraseAnalysis& pa = r.phrases;
  std::printf("  beatless ambient 150 s: ok=%d ungridded=%d sections=%zu\n", pa.ok, pa.time_based, pa.sections.size());
  for (const auto& s : pa.sections)
    std::printf("      %7.2f-%7.2f s e%-2d %s (%.2f)\n", s.start_ms / 1000, s.end_ms / 1000, s.energy, s.label.c_str(), s.label_confidence);
  CHECK(pa.ok && pa.time_based && !pa.gridded);
  int hits = 0;
  for (double want : {50000.0, 100000.0})
    for (const auto& s : pa.sections)
      if (std::fabs(s.start_ms - want) <= 3000.0) {
        ++hits;
        break;
      }
  std::printf("  ambient boundaries within 3 s: %d/2\n", hits);
  CHECK(hits >= 1);
  const auto cues = suggest_cues(r);
  for (const auto& c : cues) CHECK(c.type != "mix_in");
  check_cue_invariants(r, cues);
}

void test_phrase_short() {
  Pcm p = make_beat_track(128.0, 0.1, 20.0, true);
  AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
  std::printf("  20 s clip: phrases ok=%d sections=%zu\n", r.phrases.ok, r.phrases.sections.size());
  CHECK(!r.phrases.ok && r.phrases.sections.empty());
  CHECK(suggest_cues(r).empty());
  AnalysisOptions off;
  off.phrases = false;
  Pcm q = make_beat_track(128.0, 0.1, 60.0, true);
  AnalysisResult r2 = analyse(q.d.data(), q.frames, kRate, off);
  CHECK(!r2.phrases.ok && r2.phrases.sections.empty());
}

// The default run keeps every arrangement shape at 64 to 68 bars and faster tempi (minutes of audio were too slow for CI);
// DH_FULL_PHRASES=1 runs the original full-length matrix (128-bar arrangements at 124, 128 and 135.55 BPM and so on).
void test_phrases() {
  const bool full = std::getenv("DH_FULL_PHRASES") != nullptr;
  using Clock = std::chrono::steady_clock;
  const auto lap = [](const char* what, Clock::time_point& t) {
    const auto now = Clock::now();
    std::printf("      [%s: %.1f s]\n", what, std::chrono::duration<double>(now - t).count());
    t = now;
  };
  auto t = Clock::now();
  PhraseScore total;
  std::unique_ptr<Rendered> kept;  // synthesized once, shared with the habit test
  // The full matrix (every tempo, plus the 64-bar arrangement) behind DH_FULL_PHRASES; the default run keeps one tempo from
  // each end of the range the section labels are tuned for (the synthetic build reads as a drop at 150 BPM and above) and
  // shares the 128 BPM rendering with the habit test.
  for (double bpm : full ? std::vector<double>{124.0, 128.0, 135.55} : std::vector<double>{128.0, 135.55}) {
    auto R = test_phrases_classic(bpm, 16, 32, 16, 16, &total);
    if (bpm == 128.0) kept = std::move(R);
    lap("classic 128 bars", t);
  }
  PhraseScore sc;
  if (full) {
    auto R = run_phrase_track("8-bar phrases (64 bars) @128", 128.0, 128.0, classic(8, 16, 8), 0, 0.5, &sc);
    phrase_basic_checks(*R, sc, 3);
    const auto cues = suggest_cues(R->r);
    check_cue_invariants(R->r, cues);
    lap("8-bar phrases", t);
  }
  {
    // 12-bar breakdown: the second drop starts after intro, build, drop and the odd-length breakdown
    const int u = 16, d = 32;
    auto R = run_phrase_track("12-bar breakdown", 126.0, 126.0, classic(u, d, 12), 0, 0.5, &sc);
    phrase_basic_checks(*R, sc, 4);
    const auto cues = suggest_cues(R->r);
    check_cue_invariants(R->r, cues);
    const CueSuggestion* ld = find_cue(cues, "last_drop");
    CHECK(ld && std::fabs(ld->position_ms - R->L.start[static_cast<std::size_t>(2 * u + d + 12)] * 1000.0) <= R->L.beat[0] * 1000.0);
    lap("12-bar breakdown", t);
  }
  {
    const double bpm = 128.0;
    const int u = 16, d = 32;
    auto R = run_phrase_track("3-beat pickup", bpm, bpm, classic(u, d, u), 3, 0, &sc);
    phrase_basic_checks(*R, sc, 4);
    const PhraseAnalysis& pa = R->r.phrases;
    std::printf("      pickup_ms=%.0f origin=%.0f (expected %.0f)\n", pa.pickup_ms, pa.origin_ms, R->L.start[0] * 1000);
    const auto cues = suggest_cues(R->r);
    check_cue_invariants(R->r, cues);
    const CueSuggestion* ld = find_cue(cues, "load");
    const CueSuggestion* mi = find_cue(cues, "mix_in");
    const CueSuggestion* any = ld ? ld : mi;
    CHECK(any && std::fabs(any->position_ms - R->L.start[0] * 1000.0) <= R->L.beat[0] * 1000.0);
    lap("pickup", t);
  }
  {
    auto R = run_phrase_track("tempo ramp 122->128", 122.0, 128.0, classic(8, 16, 8), 0, 0.5, &sc);
    std::printf("      grid steady=%d sections=%zu\n", R->r.beats.steady, R->r.beats.sections.size());
    phrase_basic_checks(*R, sc, 3);
    check_cue_invariants(R->r, suggest_cues(R->r));
    lap("tempo ramp", t);
  }
  test_phrase_ambient();
  lap("ambient", t);
  test_phrase_short();
  lap("short clip", t);
  test_phrase_habit(kept);
  lap("habit", t);
  std::printf("  classic arrangements overall: boundaries %d/%d within a beat (%d extra), labels %d/%d\n", total.bound_hit,
              total.bound_total, total.extra, total.label_hit, total.label_total);
}

void demo_track(const std::string& path, double expect_bpm) {
  std::string err;
  auto audio = decode_file(path, err);
  if (!audio || audio->frames == 0) return;
  const auto t0 = std::chrono::steady_clock::now();
  AnalysisResult r = analyse(audio->pcm.data(), audio->frames, audio->sample_rate);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  %s (%.1f s, %.2f s analysis): ok=%d bpm=%.2f (expect ~%.2f) sections=%zu", path.c_str(),
              r.duration_ms / 1000, s, r.ok, r.beats.bpm, expect_bpm, r.beats.sections.size());
  for (const auto& sec : r.beats.sections) std::printf(" [%.1f ms %.2f bar_beat %d]", sec.first_beat_ms, sec.bpm, sec.bar_beat);
  std::printf("\n    downbeat conf %.2f, beat conf %.2f, key %s %s (conf %.2f, tuning %.0f c), %.1f LUFS, peak %.1f dBFS\n",
              r.beats.downbeat_confidence, r.beats.confidence, r.key.name.c_str(), r.key.wheel.c_str(), r.key.confidence,
              r.key.tuning_cents, r.loudness.integrated_lufs, r.loudness.sample_peak_dbfs);
  // Real music, no ground truth here: print the structure for eyeballing (not asserted).
  for (const auto& s : r.phrases.sections)
    std::printf("    phrase %6.1f-%6.1f s bars %3d..%3d e%-2d %-9s (%.2f) boundary %.2f\n", s.start_ms / 1000, s.end_ms / 1000,
                s.start_bar, s.end_bar, s.energy, s.label.c_str(), s.label_confidence, s.boundary_confidence);
}

}  // namespace

int main() {
  if (std::getenv("DH_ONLY_PHRASES")) {  // quick iteration on the phrase tests
    test_phrases();
    return 0;
  }
  std::printf("steady tempo\n");
  for (double bpm : {90.0, 120.0, 128.0, 140.0, 174.0}) {
    test_steady(bpm, false);
    test_steady(bpm, true);
  }
  std::printf("downbeat\n");
  test_downbeat();
  std::printf("grid drift\n");
  test_grid_drift(128.0, 360.0, 0.237);
  test_grid_drift(127.93, 360.0, 0.411);
  test_grid_drift(174.02, 360.0, 0.052);
  test_grid_drift(93.37, 360.0, 1.337);
  std::printf("intros\n");
  test_intro("pad intro 16 s @128", 128.0, 16.0, 1);
  test_intro("silent 1.337 s @124", 124.0, 1.337, 0);
  test_intro("noise-floor 2.5 s @128", 128.0, 2.5, 2);
  // Octave policy: tempi are folded into 78..176, so 70 (kicks only) is reported as 140; 87 and 174 stay put.
  test_intro("70 BPM kicks only (folds to 140)", 70.0, 0.5, 0, false, 140.0);
  test_intro("70 BPM kicks+hats (folds to 140)", 70.0, 0.5, 0, true, 140.0);
  test_intro("140 BPM", 140.0, 0.5, 0);
  test_intro("87 BPM kicks only", 87.0, 0.5, 0, false);
  test_intro("87 BPM kicks+hats", 87.0, 0.5, 0, true);
  test_intro("174 BPM kicks only", 174.0, 0.5, 0, false);
  test_intro("174 BPM kicks+hats", 174.0, 0.5, 0, true);
  std::printf("audible range / load cue\n");
  test_audible();
  std::printf("downbeat set\n");
  test_downbeat_set();
  std::printf("fluid grids\n");
  test_fluid(true);
  std::printf("tempo change\n");
  test_tempo_change();
  std::printf("key\n");
  for (int tonic : {0, 6, 8, 10}) test_key_progression(tonic, false);
  for (int tonic : {9, 4, 1, 3}) test_key_progression(tonic, true);
  test_key_scale(2, false);
  test_key_scale(9, true);
  CHECK(key_name(21) == "Am" && key_wheel(21) == "8A");
  CHECK(key_name(6) == "F#" && key_wheel(6) == "2B");
  CHECK(key_wheel(0) == "8B" && key_wheel(14) == "7A");
  std::printf("loudness\n");
  test_loudness();
  std::printf("degenerate\n");
  test_degenerate();
  std::printf("speed\n");
  test_speed();
  std::printf("phrases and cue suggestions\n");
  test_phrases();
  std::printf("demo tracks\n");
  demo_track("/Users/grio/Music/PioneerDJ/Demo Tracks/Demo Track 1.mp3", 128.0);
  demo_track("/Users/grio/Music/PioneerDJ/Demo Tracks/Demo Track 2.mp3", 120.0);
  std::printf("analysis tests passed\n");
  return 0;
}
