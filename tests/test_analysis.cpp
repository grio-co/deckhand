// Track analysis tests on synthetic signals (and the demo tracks when present).
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/analysis/analysis.hpp"
#include "deckhand/audio/track.hpp"

using namespace deckhand;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kRate = 44100;

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
  const auto t0 = std::chrono::steady_clock::now();
  AnalysisResult r = analyse(p.d.data(), p.frames, kRate);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  6-minute track: %.3f s, bpm %.2f, sections %zu, ok=%d\n", s, r.beats.bpm, r.beats.sections.size(), r.ok);
  CHECK(r.beats.ok);
  CHECK(std::fabs(r.beats.bpm - 128.0) < 0.05);
  CHECK(s < 3.0);
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
}

}  // namespace

int main() {
  std::printf("steady tempo\n");
  for (double bpm : {90.0, 120.0, 128.0, 140.0, 174.0}) {
    test_steady(bpm, false);
    test_steady(bpm, true);
  }
  std::printf("downbeat\n");
  test_downbeat();
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
  std::printf("demo tracks\n");
  demo_track("/Users/grio/Music/PioneerDJ/Demo Tracks/Demo Track 1.mp3", 128.0);
  demo_track("/Users/grio/Music/PioneerDJ/Demo Tracks/Demo Track 2.mp3", 120.0);
  std::printf("analysis tests passed\n");
  return 0;
}
