// Transition and mixer effects: riser (build and on-the-beat release), band kill isolator, reverser, auto pan.
// Click-free engage/release, silence in gives silence out, band kill depths, tempo following, CPU log.
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


double rms_ch(const std::vector<float>& b, int ch, double from_s, double to_s) {
  const std::size_t a = static_cast<std::size_t>(from_s * kSr), z = std::min(static_cast<std::size_t>(to_s * kSr), b.size() / kCh);
  double sum = 0;
  for (std::size_t i = a; i < z; ++i) sum += static_cast<double>(b[i * kCh + static_cast<std::size_t>(ch)]) * b[i * kCh + static_cast<std::size_t>(ch)];
  return z > a ? std::sqrt(sum / static_cast<double>(z - a)) : 0;
}
float max_step(const std::vector<float>& v) {
  float s = 0;
  for (std::size_t i = 1; i < v.size() / kCh; ++i) s = std::max(s, std::fabs(v[i * kCh] - v[(i - 1) * kCh]));
  return s;
}

void test_registry() {
  for (EffectKind k : {EffectKind::Riser, EffectKind::BandKill, EffectKind::Reverser, EffectKind::AutoPan}) {
    const EffectInfo* i = effect_info(k);
    CHECK(i && i->slug[0] && effect_from_slug(i->slug) == k);
    bool listed = false;
    for (int n = 0; n < effect_count(); ++n) listed = listed || effect_info_at(n).kind == k;
    CHECK(listed);
  }
  CHECK(effect_info(EffectKind::Riser)->beat_synced && effect_info(EffectKind::Reverser)->beat_synced && effect_info(EffectKind::AutoPan)->beat_synced);
  CHECK(effect_count() == static_cast<int>(EffectKind::Count) - 2);
  CHECK(effect_info(EffectKind::Plugin) && std::string(effect_info(EffectKind::Plugin)->slug) == "plugin");
}

// ---- riser: tension builds over the set beats, release lands on a beat line and the dry signal is exact ----
void test_riser() {
  for (double bpm : {120.0, 90.0}) {
    const double beat = 60.0 / bpm;
    auto e = playing(sine_track(220, 30, bpm));
    auto ref = playing(sine_track(220, 30, bpm));
    const int id = e->fx_insert(0, EffectKind::Riser, -1, false);
    e->fx_set(0, id, FxField::Beats, 4.0);
    e->fx_set(0, id, FxField::Param, 1.0);
    auto pre = run(*e, 0.3), pre_ref = run(*ref, 0.3);
    e->fx_set(0, id, FxField::On, 1);
    const double t_on = 0.3;
    auto build = run(*e, 4 * beat + 0.05);
    const double t_off = t_on + 4 * beat + 0.05;
    e->fx_set(0, id, FxField::On, 0);
    auto rel = run(*e, 2.5 * beat + 0.2);
    auto rel_ref = run(*ref, 4 * beat + 0.05 + 2.5 * beat + 0.2);
    CHECK(finite(build) && finite(rel));
    const double early = crossings_per_s(build, 0.0, 0.25), late = crossings_per_s(build, 4 * beat - 0.3, 4 * beat);
    std::printf("riser %.0f bpm: crossings/s early %.0f, late %.0f; level early %.3f late %.3f\n", bpm, early, late, rms(build, 0.0, 0.25),
                rms(build, 4 * beat - 0.3, 4 * beat));
    CHECK(late > early * 1.8);  // the high-pass and the noise move the content up
    CHECK(rms(build, 4 * beat - 0.3, 4 * beat) != rms(build, 0.0, 0.25));
    // release: find where the output equals the dry signal for good
    const std::size_t off0 = static_cast<std::size_t>((4 * beat + 0.05) * kSr);
    double back = -1;
    for (std::size_t i = rel.size() / kCh; i-- > 0;) {
      const float dref = rel_ref[(off0 + i) * kCh];
      if (std::fabs(rel[i * kCh] - dref) > 2e-3f) {
        back = static_cast<double>(i + 1) / kSr;
        break;
      }
    }
    const double at = t_off + back;  // song time of the return to dry
    const double nb = std::ceil(t_off / beat) * beat;
    std::printf("riser %.0f bpm: released at %.3f s, dry again at %.3f s, next beat line %.3f s\n", bpm, t_off, at, nb);
    CHECK(back >= 0 && at >= nb - 0.001 && at <= nb + 0.03);
    float big = 0;
    for (std::size_t i = 0; i < pre.size(); ++i) big = std::max(big, std::fabs(pre[i] - pre_ref[i]));
    CHECK(big < 1e-4f);  // idle before engage: bit-exact dry
    // steps: tone moves at most 0.5*2*pi*220/44100 = 0.016 per frame; the effect adds its own content
    CHECK(max_step(build) < 0.35f && max_step(rel) < 0.35f);
  }
}

void test_silence_and_clicks() {
  const EffectKind kinds[] = {EffectKind::Riser, EffectKind::BandKill, EffectKind::Reverser, EffectKind::AutoPan};
  for (EffectKind k : kinds) {
    // silence in: nothing out once released and any tail is over
    {
      auto e = playing(make_track(30, [](std::uint64_t) { return 0.0f; }));
      const int id = e->fx_insert(0, k, -1, false);
      e->fx_set(0, id, FxField::On, 1);
      run(*e, 1.3);
      e->fx_set(0, id, FxField::On, 0);
      run(*e, 1.5);
      const auto tail = run(*e, 1.0);
      CHECK(finite(tail) && rms(tail, 0, 1.0) < 1e-5);
      if (k != EffectKind::Riser) CHECK(rms(run(*e, 0.5), 0, 0.5) < 1e-6);
    }
    // click-free engage / release cycles
    auto e = playing(sine_track(440, 30));
    const int id = e->fx_insert(0, k, -1, false);
    float step = 0;
    for (int rep = 0; rep < 4; ++rep) {
      e->fx_set(0, id, FxField::On, 1);
      const auto a = run(*e, 0.45 + 0.07 * rep);
      e->fx_set(0, id, FxField::On, 0);
      const auto b = run(*e, 0.9);
      CHECK(finite(a) && finite(b));
      for (const auto* v : {&a, &b}) {
        const float ms = max_step(*v);
        if (ms > 0.4f) {
          std::size_t at = 0;
          for (std::size_t i = 1; i < v->size() / kCh; ++i)
            if (std::fabs((*v)[i * kCh] - (*v)[(i - 1) * kCh]) == ms) at = i;
          std::printf("  big step %.3f in %s of cycle %d at frame %zu (%.4f s)\n", ms, v == &a ? "engaged part" : "release part", rep, at, static_cast<double>(at) / kSr);
          for (std::size_t i = at - 3; i < at + 3; ++i) std::printf("    [%zu] %.4f\n", i, (*v)[i * kCh]);
        }
      }
      step = std::max({step, max_step(a), max_step(b)});
    }
    std::printf("%s: largest sample step across 4 engage/release cycles %.3f\n", effect_info(k)->name, step);
    CHECK(step < 0.4f);
    e->fx_set(0, id, FxField::On, 1);
    for (double beats : {0.0625, 16.0, 0.25}) {
      e->fx_set(0, id, FxField::Beats, beats);
      e->fx_set(0, id, FxField::Param, beats > 1 ? 1.0 : 0.0);
      e->fx_set(0, id, FxField::Param2, beats > 1 ? 0.0 : 1.0);
      e->fx_set(0, id, FxField::Param3, 1.0);
      e->send(cmd(Op::Tempo, 0, beats > 1 ? 1.0f : -1.0f));
      CHECK(finite(run(*e, 0.5)));
    }
    const double ns = fx_bench_ns_per_block(k, 512, kSr, 200);
    std::printf("cpu: %-10s %.1f us per 512-frame block (budget %.0f us)\n", effect_info(k)->name, ns / 1000.0, 512.0 / kSr * 1e6);
    CHECK(ns < 512.0 / kSr * 1e9 * 0.25);
  }
}

// ---- band kill ----
double kill_level(double hz, float lo, float mid, float hi) {
  auto e = playing(sine_track(hz, 20));
  const int id = e->fx_insert(0, EffectKind::BandKill, -1, true);
  e->fx_set(0, id, FxField::Param, lo);
  e->fx_set(0, id, FxField::Param2, mid);
  e->fx_set(0, id, FxField::Param3, hi);
  return rms(run(*e, 1.5), 0.8, 1.5);
}
void test_band_kill() {
  const double ref = rms(run(*playing(sine_track(1000, 20)), 1.5), 0.8, 1.5);
  const double hz[3] = {40, 1000, 12000};
  for (int b = 0; b < 3; ++b) {
    float g[3] = {0.5f, 0.5f, 0.5f};
    g[b] = 0;
    const double killed = kill_level(hz[b], g[0], g[1], g[2]);
    const double unity = kill_level(hz[b], 0.5f, 0.5f, 0.5f);
    std::printf("band kill band %d at %.0f Hz: unity %.4f, killed %.6f (reference %.4f, %.0f dB)\n", b, hz[b], unity, killed, ref,
                20 * std::log10(std::max(killed, 1e-9) / ref));
    CHECK(killed < ref * 0.008);  // better than -42 dB at the test tones (the skirts of a 24 dB/oct crossover)
    CHECK(unity > ref * 0.89 && unity < ref * 1.12);
  }
  // smooth crossover: unity at the crossover frequencies is flat, and killing the lows costs a tone at the crossover about 6 dB
  for (double f : {160.0, 3500.0}) {
    const double u = kill_level(f, 0.5f, 0.5f, 0.5f);
    CHECK(u > ref * 0.89 && u < ref * 1.12);
  }
  const double half = kill_level(160, 0, 0.5f, 0.5f);
  CHECK(half > ref * 0.3 && half < ref * 0.75);
  // boost: fully up is +6 dB in its band
  const double boost = kill_level(1000, 0.5f, 1.0f, 0.5f);
  CHECK(boost > ref * 1.8 && boost < ref * 2.2);
  // the law is dB-linear below unity: 0.27 is about -30 dB
  const double mid_cut = kill_level(1000, 0.5f, 0.27f, 0.5f);
  CHECK(mid_cut > ref * 0.01 && mid_cut < ref * 0.08);
}

// ---- reverser ----
void test_reverser() {
  for (double bpm : {120.0, 90.0}) {
    const double bf = 60.0 / bpm * kSr;
    auto t = make_track(30, [=](std::uint64_t i) { return 0.8f * static_cast<float>(std::fmod(static_cast<double>(i), bf) / bf); }, bpm);
    auto e = playing(std::move(t));
    const int id = e->fx_insert(0, EffectKind::Reverser, -1, false);
    e->fx_set(0, id, FxField::Beats, 1.0);
    run(*e, 0.2);
    e->fx_set(0, id, FxField::On, 1);
    const auto o = run(*e, 5 * 60.0 / bpm);
    const double L = bf;
    // second to fourth slices after engaging: out(p) ~ 0.8 * (1 - p / L)
    for (int s = 3; s <= 4; ++s) {
      for (double fr : {0.25, 0.5, 0.75}) {
        const double at = (s * L + fr * L) / kSr - 0.2;
        const std::size_t idx = static_cast<std::size_t>(at * kSr) + static_cast<std::size_t>(0.2 * kSr);
        const float v = o[std::min(idx - static_cast<std::size_t>(0.2 * kSr), o.size() / kCh - 1) * kCh];
        const float want = 0.8f * (1.0f - static_cast<float>(fr));
        if (std::fabs(v - want) > 0.06f) std::printf("reverser %.0f bpm slice %d at %.2f: got %.3f want %.3f\n", bpm, s, fr, v, want);
        CHECK(std::fabs(v - want) < 0.06f);
      }
    }
    CHECK(max_step(o) < 0.5f);
  }
}

// ---- auto pan ----
void test_auto_pan() {
  const double bpm = 120, beat = 0.5;
  auto make = [&](float depth) {
    auto e = playing(sine_track(440, 30, bpm));
    const int id = e->fx_insert(0, EffectKind::AutoPan, -1, true);
    e->fx_set(0, id, FxField::Beats, 1.0);
    e->fx_set(0, id, FxField::Param, depth);
    e->fx_set(0, id, FxField::Param2, 0.2);
    return e;
  };
  auto o = run(*make(1.0f), 2.0);
  // first half of each beat the right side is louder, the second half the left
  const double l1 = rms_ch(o, 0, 1.0 + 0.05, 1.0 + 0.2), r1 = rms_ch(o, 1, 1.0 + 0.05, 1.0 + 0.2);
  const double l2 = rms_ch(o, 0, 1.0 + 0.30, 1.0 + 0.45), r2 = rms_ch(o, 1, 1.0 + 0.30, 1.0 + 0.45);
  std::printf("auto pan: first half L %.3f R %.3f, second half L %.3f R %.3f\n", l1, r1, l2, r2);
  CHECK(r1 > l1 * 2 && l2 > r2 * 2);
  const auto flat = run(*make(0.0f), 1.0);
  CHECK(std::fabs(rms_ch(flat, 0, 0.3, 0.9) - rms_ch(flat, 1, 0.3, 0.9)) < 1e-3);
  // tempo following: at 90 bpm the swing takes 0.667 s per cycle: a half cycle later the sides have swapped
  auto e = playing(sine_track(440, 30, 90));
  const int id = e->fx_insert(0, EffectKind::AutoPan, -1, true);
  e->fx_set(0, id, FxField::Param, 1.0);
  e->fx_set(0, id, FxField::Param2, 0.2);
  const auto o90 = run(*e, 2.0);
  const double b = 60.0 / 90.0;
  CHECK(rms_ch(o90, 1, b + 0.05, b + 0.25) > rms_ch(o90, 0, b + 0.05, b + 0.25) * 2);
  CHECK(rms_ch(o90, 0, b + 0.40, b + 0.60) > rms_ch(o90, 1, b + 0.40, b + 0.60) * 2);
  (void)beat;
}

}  // namespace

// ---- the beat position handed to a chain is continuous across chunks and render calls -----------------------
// Auto Pan on a constant signal: the left/right ratio gives the LFO phase back. It must not depend on how the host cuts
// the stream into render calls (a chunk's beat position used to be taken after the deck had moved on, by a chunk-size
// dependent amount), and it must advance smoothly across chunk boundaries.
void test_beat_position_continuity() {
  auto lfo_of = [](int block) {
    auto e = playing(make_track(30, [](std::uint64_t) { return 0.5f; }, 120));
    const int id = e->fx_insert(0, EffectKind::AutoPan, -1, true);
    e->fx_set(0, id, FxField::Beats, 1.0);
    e->fx_set(0, id, FxField::Param, 1.0);
    e->fx_set(0, id, FxField::Param2, 0.0);
    e->fx_set(0, id, FxField::Wet, 1.0);
    const auto o = run(*e, 3.0, block);
    std::vector<double> v(o.size() / kCh);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = std::atan2(static_cast<double>(o[i * kCh + 1]), static_cast<double>(o[i * kCh])) / 0.78539816339 - 1.0;
    return v;
  };
  const auto ref = lfo_of(512);
  double smooth = 0;  // largest step between neighbouring frames (a sine of one beat: at most about 2*pi/22050 per frame)
  for (std::size_t i = 22051; i < ref.size(); ++i) smooth = std::max(smooth, std::fabs(ref[i] - ref[i - 1]));
  CHECK(smooth < 0.0005);
  for (int block : {700, 300, 1000, 97}) {
    const auto v = lfo_of(block);
    double worst = 0, step = 0;
    for (std::size_t i = 22050; i < v.size() && i < ref.size(); ++i) {
      worst = std::max(worst, std::fabs(v[i] - ref[i]));
      step = std::max(step, std::fabs(v[i] - v[i - 1]));
    }
    std::printf("beat position, render calls of %d frames: worst difference to 512-frame calls %.5f, largest step %.5f\n", block, worst, step);
    CHECK(worst < 0.002);
    CHECK(step < 0.0005);
  }
}

int main() {
  test_registry();
  test_riser();
  test_band_kill();
  test_reverser();
  test_auto_pan();
  test_silence_and_clicks();
  test_beat_position_continuity();
  std::printf("fx transitions ok\n");
  return 0;
}
