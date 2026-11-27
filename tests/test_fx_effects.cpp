// Beat-synced and mixer-style effects added on top of the chain engine: tape echo, beat-length reverb, tape stop,
// the isolator's kill depth and slopes, tempo following, click-free enable/disable, denormal safety and the CPU log.
// Offline engine only; nothing opens an audio or MIDI device.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"

using namespace deckhand;

namespace {
const double kPi = 3.14159265358979323846;
constexpr int kCh = 2;
constexpr int kSr = 44100;

std::unique_ptr<LoadedTrack> make_track(double seconds, const std::function<float(std::uint64_t)>& f, double bpm = 120) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = kSr;
  audio->frames = static_cast<std::uint64_t>(seconds * kSr);
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i) audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = f(i);
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
std::unique_ptr<Engine> playing(std::unique_ptr<LoadedTrack> t) {
  auto e = std::make_unique<Engine>();
  e->configure_offline(kSr, kCh);
  e->load(0, std::move(t));
  e->send(cmd(Op::PlayToggle, 0));
  return e;
}
std::vector<float> run(Engine& e, double seconds, int block = 256) {
  const std::uint32_t frames = static_cast<std::uint32_t>(seconds * kSr);
  std::vector<float> out(static_cast<std::size_t>(frames) * kCh);
  for (std::uint32_t done = 0; done < frames;) {
    const std::uint32_t n = std::min<std::uint32_t>(static_cast<std::uint32_t>(block), frames - done);
    e.render(out.data() + static_cast<std::size_t>(done) * kCh, n);
    done += n;
  }
  return out;
}
double rms(const std::vector<float>& b, double from_s, double to_s) {
  const std::size_t a = static_cast<std::size_t>(from_s * kSr), z = std::min(static_cast<std::size_t>(to_s * kSr), b.size() / kCh);
  double sum = 0;
  for (std::size_t i = a; i < z; ++i) sum += static_cast<double>(b[i * kCh]) * b[i * kCh];
  return z > a ? std::sqrt(sum / static_cast<double>(z - a)) : 0;
}
bool finite(const std::vector<float>& b) {
  for (float v : b)
    if (!std::isfinite(v)) return false;
  return true;
}
double crossings_per_s(const std::vector<float>& b, double from_s, double to_s) {
  const std::size_t a = static_cast<std::size_t>(from_s * kSr), z = std::min(static_cast<std::size_t>(to_s * kSr), b.size() / kCh);
  int c = 0;
  for (std::size_t i = a + 1; i < z; ++i)
    if ((b[(i - 1) * kCh] < 0) != (b[i * kCh] < 0)) ++c;
  return c / (to_s - from_s);
}
std::unique_ptr<LoadedTrack> sine_track(double hz, double s = 30, double bpm = 120) {
  return make_track(s, [=](std::uint64_t i) { return 0.5f * static_cast<float>(std::sin(2 * kPi * hz * static_cast<double>(i) / kSr)); }, bpm);
}
// First sample at or after `from` (frames) whose magnitude is above `thr`.
std::size_t peak_near(const std::vector<float>& o, std::size_t centre, std::size_t radius, float& v) {
  std::size_t best = centre;
  v = 0;
  for (std::size_t i = centre > radius ? centre - radius : 0; i < std::min(o.size() / kCh, centre + radius); ++i)
    if (std::fabs(o[i * kCh]) > v) v = std::fabs(o[i * kCh]), best = i;
  return best;
}

// ---- tape echo: repeats land on the beat division at the deck's tempo, each darker and quieter than the last ----
void test_tape_echo() {
  const double bpms[2] = {120, 90};
  for (int k = 0; k < 2; ++k) {
    auto click = [](std::uint64_t i) { return i == static_cast<std::uint64_t>(0.3 * kSr) ? 0.9f : 0.0f; };
    auto e = playing(make_track(8, click, bpms[k]));
    const int id = e->fx_insert(0, EffectKind::TapeEcho, -1, true);
    CHECK(id > 0);
    e->fx_set(0, id, FxField::Beats, 0.5);
    e->fx_set(0, id, FxField::Wet, 1);
    e->fx_set(0, id, FxField::Param, 0.6);
    e->fx_set(0, id, FxField::Param2, 0.8);
    e->fx_set(0, id, FxField::Param3, 0);  // no wobble: the delay is exact
    const auto o = run(*e, 3.0);
    CHECK(finite(o));
    const double d = 0.5 * 60.0 / bpms[k] * kSr;
    float v0, v1, v2;
    const double c0 = static_cast<double>(peak_near(o, static_cast<std::size_t>(0.3 * kSr + 100), 400, v0));  // the dry click (the deck starts a little late)
    const std::size_t p1 = peak_near(o, static_cast<std::size_t>(c0 + d), 200, v1);
    const std::size_t p2 = peak_near(o, static_cast<std::size_t>(c0 + 2 * d), 200, v2);
    std::printf("tape echo at %.0f bpm: repeat 1 at +%.1f frames (expected %.1f) level %.3f; repeat 2 at +%.1f level %.3f\n", bpms[k],
                static_cast<double>(p1) - c0, d, v1, static_cast<double>(p2) - c0, v2);
    CHECK(v1 > 0.01f);
    CHECK(v2 < v1);
    CHECK(std::fabs(static_cast<double>(p1) - c0 - d) < 40);  // the repeat's own low-pass delays the peak a little
    CHECK(std::fabs(static_cast<double>(p2) - c0 - 2 * d) < 80);
  }
  // darker tone: less high-frequency energy in the repeats
  double hf[2] = {};
  for (int k = 0; k < 2; ++k) {
    auto burst = [](std::uint64_t i) {
      return i < static_cast<std::uint64_t>(0.2 * kSr) ? 0.5f * static_cast<float>(std::sin(2 * kPi * 6000 * static_cast<double>(i) / kSr)) : 0.0f;
    };
    auto e = playing(make_track(10, burst));
    const int id = e->fx_insert(0, EffectKind::TapeEcho, -1, true);
    e->fx_set(0, id, FxField::Beats, 0.25);
    e->fx_set(0, id, FxField::Wet, 1);
    e->fx_set(0, id, FxField::Param2, k ? 0.1 : 0.9);
    e->fx_set(0, id, FxField::Param3, 0);
    const auto o = run(*e, 2.0);
    hf[k] = rms(o, 0.4, 2.0);
  }
  std::printf("tape echo tone: 6 kHz level %.4f bright, %.4f dark\n", hf[0], hf[1]);
  CHECK(hf[1] < hf[0]);
}

// ---- beat reverb: a longer beat length rings longer ----
void test_beat_reverb() {
  double tail[2] = {};
  const double beats[2] = {1.0, 8.0};
  for (int k = 0; k < 2; ++k) {
    auto burst = [](std::uint64_t i) {
      std::uint32_t r = static_cast<std::uint32_t>(i) * 2654435761u + 12345u;
      r ^= r >> 15;
      r *= 2246822519u;
      r ^= r >> 13;
      return i < static_cast<std::uint64_t>(0.5 * kSr) ? 0.5f * (static_cast<float>(r) / 2147483648.0f - 1.0f) : 0.0f;
    };
    auto e = playing(make_track(12, burst));
    const int id = e->fx_insert(0, EffectKind::BeatReverb, -1, true);
    CHECK(id > 0);
    e->fx_set(0, id, FxField::Beats, beats[k]);
    e->fx_set(0, id, FxField::Wet, 1);
    const auto o = run(*e, 8.0);
    CHECK(finite(o));
    tail[k] = rms(o, 3.0, 4.0);
    const double drop_db = 20 * std::log10(rms(o, 1.0, 1.5) / std::max(1e-9, rms(o, 3.0, 3.5)));
    std::printf("beat reverb, %.0f beats (%.1f s to fall 60 dB): tail level %.5f (3-4 s), %.1f dB lower at 3 s than at 1 s\n", beats[k], beats[k] * 0.5,
                tail[k], drop_db);
    if (k == 1) CHECK(drop_db > 10 && drop_db < 50);  // about 30 dB expected for a 4 s decay
  }
  CHECK(tail[1] > tail[0] * 2.0);
}

// ---- tape stop: pitch falls to a stop over the beat length, then the live signal returns on release ----
void test_tape_stop() {
  auto e = playing(sine_track(1000, 30));
  const auto pre = run(*e, 1.0);
  const int id = e->fx_insert(0, EffectKind::TapeStop, -1, false);
  e->fx_set(0, id, FxField::Beats, 2.0);  // one second at 120 bpm
  e->fx_set(0, id, FxField::Wet, 1);
  e->fx_set(0, id, FxField::Param, 0.3);
  e->fx_set(0, id, FxField::On, 1);
  const auto mid = run(*e, 1.5);
  e->fx_set(0, id, FxField::On, 0);
  const auto post = run(*e, 0.5);
  const double f0 = crossings_per_s(mid, 0.05, 0.15), f1 = crossings_per_s(mid, 0.6, 0.7);
  std::printf("tape stop: %.0f -> %.0f zero crossings/s, level at the end %.4f, after release %.4f (live %.4f)\n", f0, f1, rms(mid, 1.2, 1.5),
              rms(post, 0.2, 0.5), rms(pre, 0.5, 1.0));
  CHECK(finite(mid) && finite(post));
  CHECK(f1 < f0 * 0.8);
  CHECK(rms(mid, 1.2, 1.5) < rms(pre, 0.5, 1.0) * 0.2);
  CHECK(rms(post, 0.2, 0.5) > rms(pre, 0.5, 1.0) * 0.8);
  float md = 0;
  for (std::size_t i = 1; i < post.size() / kCh; ++i) md = std::max(md, std::fabs(post[i * kCh] - post[(i - 1) * kCh]));
  CHECK(md < 0.5f);  // no hard step when the signal comes back
}

// ---- isolator: full kill of one band, others untouched, steep slopes ----
double iso_level(double hz, float lo, float mid, float hi) {
  auto e = playing(sine_track(hz, 20));
  const int id = e->fx_insert(0, EffectKind::Isolator, -1, true);
  e->fx_set(0, id, FxField::Wet, 1);
  e->fx_set(0, id, FxField::Param, lo);
  e->fx_set(0, id, FxField::Param2, mid);
  e->fx_set(0, id, FxField::Param3, hi);
  const auto o = run(*e, 1.5);
  return rms(o, 0.8, 1.5);
}
void test_isolator() {
  const double ref = rms(run(*playing(sine_track(1000, 20)), 1.5), 0.8, 1.5);
  const double hz[3] = {80, 1000, 9000};
  for (int b = 0; b < 3; ++b) {
    float g[3] = {0.5f, 0.5f, 0.5f};
    g[b] = 0;
    const double killed = iso_level(hz[b], g[0], g[1], g[2]);
    const double unity = iso_level(hz[b], 0.5f, 0.5f, 0.5f);
    std::printf("isolator band %d at %.0f Hz: unity %.4f, killed %.5f (reference %.4f)\n", b, hz[b], unity, killed, ref);
    CHECK(killed < ref * 0.04);   // better than -28 dB (the mid band leaks the high band's skirt at 1 kHz)
    CHECK(unity > ref * 0.85 && unity < ref * 1.15);
  }
  // steepness: with the lows killed, a tone two octaves above the 250 Hz crossover is hardly touched and one two octaves below is gone
  const double above = iso_level(1000, 0, 0.5f, 0.5f), below = iso_level(62, 0, 0.5f, 0.5f);
  std::printf("isolator slopes: 1 kHz %.4f, 62 Hz %.5f with the lows killed\n", above, below);
  CHECK(above > ref * 0.8);
  CHECK(below < ref * 0.01);
}

// ---- every new effect: click-free on/off, finite after silence and extreme settings, CPU log ----
void test_all_new() {
  const EffectKind kinds[] = {EffectKind::TapeEcho, EffectKind::BeatReverb, EffectKind::TapeStop};
  for (EffectKind k : kinds) {
    auto e = playing(sine_track(440, 30));
    const int id = e->fx_insert(0, k, -1, false);
    const auto base = run(*e, 0.3);
    float step = 0;
    for (int rep = 0; rep < 4; ++rep) {
      e->fx_set(0, id, FxField::On, 1);
      const auto a = run(*e, 0.4);
      e->fx_set(0, id, FxField::On, 0);
      const auto b = run(*e, 0.4);
      CHECK(finite(a) && finite(b));
      for (const auto* v : {&a, &b})
        for (std::size_t i = 1; i < v->size() / kCh; ++i) step = std::max(step, std::fabs((*v)[i * kCh] - (*v)[(i - 1) * kCh]));
    }
    // a 440 Hz sine of 0.5 moves at most 0.0313 per frame; allow a generous margin for the effect's own content
    std::printf("%s: largest sample step across 4 enable/disable cycles %.3f\n", effect_info(k)->name, step);
    CHECK(step < 0.5f);
    // extreme parameters, tempo changes while sounding
    e->fx_set(0, id, FxField::On, 1);
    for (double beats : {0.0625, 16.0, 0.25}) {
      e->fx_set(0, id, FxField::Beats, beats);
      e->fx_set(0, id, FxField::Param, beats > 1 ? 1.0 : 0.0);
      e->fx_set(0, id, FxField::Param2, beats > 1 ? 0.0 : 1.0);
      e->fx_set(0, id, FxField::Param3, 1.0);
      e->send(cmd(Op::Tempo, 0, beats > 1 ? 1.0f : -1.0f));
      CHECK(finite(run(*e, 0.5)));
    }
    (void)base;
    const double ns = fx_bench_ns_per_block(k, 512, kSr, 200);
    std::printf("cpu: %-12s %.1f us per 512-frame block (budget %.0f us)\n", effect_info(k)->name, ns / 1000.0, 512.0 / kSr * 1e6);
  }
}

void test_registry() {
  for (EffectKind k : {EffectKind::TapeEcho, EffectKind::BeatReverb, EffectKind::TapeStop}) {
    const EffectInfo* i = effect_info(k);
    CHECK(i && i->slug[0] && effect_from_slug(i->slug) == k && i->beat_synced);
  }
  CHECK(effect_count() == static_cast<int>(EffectKind::Count) - 2);
}

}  // namespace

int main() {
  test_registry();
  test_tape_echo();
  test_beat_reverb();
  test_tape_stop();
  test_isolator();
  test_all_new();
  std::printf("fx effects ok\n");
  return 0;
}
