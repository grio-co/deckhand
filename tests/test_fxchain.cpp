// Effect chains: ordering, bypass, click-free edits, tails, beat sync, presets, the legacy fixed-slot view, CPU cost,
// allocation-free processing, and the same edit sequence through the in-process Engine and the engine process
// (RemoteEngine on the null device; nothing here opens a real audio device or plays sound).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <new>
#include <thread>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/ipc.hpp"
#include "deckhand/audio/remote_engine.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

// ---- allocation counter: counts operator new calls while armed (the audio thread here is the test thread) ----
namespace {
std::atomic<bool> g_armed{false};
std::atomic<long> g_allocs{0};
}  // namespace
void* operator new(std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

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
float tone_mix(std::uint64_t i) {
  const double t = static_cast<double>(i) / kSr;
  return static_cast<float>(0.2 * std::sin(2 * kPi * 110 * t) + 0.15 * std::sin(2 * kPi * 1300 * t) + 0.1 * std::sin(2 * kPi * 6000 * t));
}
std::unique_ptr<LoadedTrack> mix_track(double s = 60) { return make_track(s, tone_mix); }
std::unique_ptr<LoadedTrack> sine_track(double hz, double s = 60) {
  return make_track(s, [=](std::uint64_t i) { return 0.5f * static_cast<float>(std::sin(2 * kPi * hz * static_cast<double>(i) / kSr)); });
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

std::unique_ptr<Engine> engine() {
  auto e = std::make_unique<Engine>();
  e->configure_offline(kSr, kCh);
  return e;
}
std::unique_ptr<Engine> playing(std::unique_ptr<LoadedTrack> t) {
  auto e = engine();
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
void append(std::vector<float>& a, const std::vector<float>& b) { a.insert(a.end(), b.begin(), b.end()); }
double rms(const std::vector<float>& b, double from_s, double to_s) {
  const std::size_t a = static_cast<std::size_t>(from_s * kSr), z = std::min(static_cast<std::size_t>(to_s * kSr), b.size() / kCh);
  double sum = 0;
  for (std::size_t i = a; i < z; ++i) sum += static_cast<double>(b[i * kCh]) * b[i * kCh];
  return z > a ? std::sqrt(sum / static_cast<double>(z - a)) : 0;
}
float peak(const std::vector<float>& b, double from_s, double to_s) {
  const std::size_t a = static_cast<std::size_t>(from_s * kSr), z = std::min(static_cast<std::size_t>(to_s * kSr), b.size() / kCh);
  float m = 0;
  for (std::size_t i = a; i < z; ++i) m = std::max(m, std::fabs(b[i * kCh]));
  return m;
}
float max_delta(const std::vector<float>& b, std::size_t from = 0) {
  float m = 0;
  for (std::size_t i = from + 1; i < b.size() / kCh; ++i) m = std::max(m, std::fabs(b[i * kCh] - b[(i - 1) * kCh]));
  return m;
}
double max_diff(const std::vector<float>& a, const std::vector<float>& b, double from_s) {
  double m = 0;
  for (std::size_t i = static_cast<std::size_t>(from_s * kSr) * kCh; i < std::min(a.size(), b.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}
bool finite(const std::vector<float>& b) {
  for (float v : b)
    if (!std::isfinite(v)) return false;
  return true;
}
std::string sig(const FxChainInfo& c) { return fx_chain_to_json(c) + (c.exists ? "|e" : "|-"); }

// ---------------------------------------------------------------------------------------------------------
void test_edits_and_presets(IEngine& e, bool remote) {
  const int T = 0;
  CHECK(!e.fx_chain(T).exists);
  const int a = e.fx_insert(T, EffectKind::Isolator);
  const int b = e.fx_insert(T, EffectKind::Echo);
  const int c = e.fx_insert(T, EffectKind::Filter, 0);  // at the front
  CHECK(a > 0 && b > a && c > b);
  FxChainInfo ci = e.fx_chain(T);
  CHECK(ci.count == 3 && ci.unit[0].id == c && ci.unit[1].id == a && ci.unit[2].id == b);
  CHECK(ci.unit[0].kind == EffectKind::Filter && ci.unit[2].kind == EffectKind::Echo);
  CHECK(e.fx_move(T, c, 2));
  ci = e.fx_chain(T);
  CHECK(ci.unit[0].id == a && ci.unit[1].id == b && ci.unit[2].id == c);
  CHECK(e.fx_set(T, b, FxField::Param, 0.8) && e.fx_set(T, b, FxField::Beats, 0.25) && e.fx_set(T, b, FxField::On, 1));
  CHECK(e.fx_set(T, a, FxField::Param3, 0.25) && e.fx_set(T, b, FxField::Seconds, 0.3));
  ci = e.fx_chain(T);
  CHECK(std::fabs(ci.unit[1].param - 0.8f) < 1e-6 && ci.unit[1].on && std::fabs(ci.unit[1].beats - 0.25f) < 1e-6);
  CHECK(std::fabs(ci.unit[0].param3 - 0.25f) < 1e-6 && std::fabs(ci.unit[1].seconds - 0.3) < 1e-9);
  CHECK(e.fx_replace(T, b, EffectKind::Phaser) == b);
  CHECK(e.fx_chain(T).unit[1].kind == EffectKind::Phaser);
  CHECK(e.fx_chain_bypass(T, true) && e.fx_chain(T).bypass);
  CHECK(e.fx_chain_wet(T, 0.5) && std::fabs(e.fx_chain(T).wet - 0.5f) < 1e-6);
  CHECK(!e.fx_remove(T, 9999) && !e.fx_set(T, 9999, FxField::Wet, 1) && !e.fx_set(T, a, FxField::Count, 1));
  CHECK(e.fx_insert(99, EffectKind::Echo) == 0);                     // no such chain
  CHECK(e.fx_insert(T, static_cast<EffectKind>(250)) == 0);          // no such effect
  CHECK(e.fx_insert(T, EffectKind::None) == 0);
  for (int i = 3; i < kFxChainMax; ++i) CHECK(e.fx_insert(T, EffectKind::Crusher) > 0);
  CHECK(e.fx_insert(T, EffectKind::Crusher) == 0);  // the ninth is refused
  CHECK(e.fx_chain(T).count == kFxChainMax);
  // presets
  const std::string js = e.fx_chain_save(T);
  CHECK(js.find("\"isolator\"") != std::string::npos && js.find("\"phaser\"") != std::string::npos);
  CHECK(e.fx_chain_clear(T) && e.fx_chain(T).count == 0);
  std::string err;
  CHECK(e.fx_chain_load(2, js, &err));
  FxChainInfo l = e.fx_chain(2), orig;
  CHECK(fx_chain_from_json(js, orig, err));
  CHECK(l.count == kFxChainMax && fx_chain_to_json(l) == js);
  CHECK(!e.fx_chain_load(2, "{\"v\":1,\"fx\":[{\"type\":\"warp_drive\"}]}", &err) && err.find("warp_drive") != std::string::npos);
  CHECK(!e.fx_chain_load(2, "not json", &err));
  CHECK(!e.fx_chain_load(2, "{\"v\":1,\"fx\":[]} trailing", &err));
  std::string nine = "{\"v\":1,\"fx\":[";
  for (int i = 0; i < 9; ++i) nine += std::string(i ? "," : "") + "{\"type\":\"echo\"}";
  CHECK(!e.fx_chain_load(2, nine + "]}", &err));
  CHECK(e.fx_chain(2).count == kFxChainMax);  // a failed load leaves the chain alone
  CHECK(e.fx_chain_load(2, "{\"v\":1,\"bypass\":true,\"wet\":0.25,\"fx\":[{\"type\":\"build_up\",\"on\":true,\"param\":0.9}]}"));
  l = e.fx_chain(2);
  CHECK(l.count == 1 && l.unit[0].kind == EffectKind::BuildUp && l.unit[0].on && l.bypass && std::fabs(l.wet - 0.25f) < 1e-6);
  // every addressable bus
  CHECK(e.fx_insert(kFxMaster, EffectKind::Reverb) > 0 && e.fx_insert(kFxTargetMic, EffectKind::Phaser) > 0);
  CHECK(e.fx_insert(kFxTargetSampler, EffectKind::Echo) > 0 && e.fx_insert(fx_target_stem(1, 3), EffectKind::Isolator) > 0);
  CHECK(e.fx_chain(fx_target_stem(1, 3)).count == 1 && e.fx_chain(fx_target_stem(1, 2)).count == 0);
  CHECK(e.fx_insert(7, EffectKind::Echo) == 0 && e.fx_insert(fx_target_stem(3, 7) + 1, EffectKind::Echo) == 0);
  // legacy fixed slots are a view onto the deck's default chain
  const int T1 = 1;
  e.send(cmd(Op::FxSelect, T1, 0, 1, static_cast<int>(EffectKind::Echo)));
  const int user = e.fx_insert(T1, EffectKind::Isolator);
  e.send(cmd(Op::FxSelect, T1, 0, 0, static_cast<int>(EffectKind::Reverb)));
  ci = e.fx_chain(T1);
  CHECK(ci.count == 3 && ci.unit[0].legacy_slot == 0 && ci.unit[1].legacy_slot == 1 && ci.unit[2].id == user);
  e.send(cmd(Op::FxSelect, T1, 0, 1, 0));
  CHECK(e.fx_chain(T1).count == 2);
  (void)remote;
}

// ---------------------------------------------------------------------------------------------------------
void test_ordering_bypass() {
  auto build = [&](bool filter_first) {
    auto e = playing(mix_track());
    const int f = e->fx_insert(0, EffectKind::Filter, -1, true);
    const int c = e->fx_insert(0, EffectKind::Crusher, filter_first ? -1 : 0, true);
    CHECK(f && c);
    e->fx_set(0, f, FxField::Param, 0.15);
    e->fx_set(0, c, FxField::Param, 0.9);
    return e;
  };
  auto ab = build(true), ba = build(false);
  const auto oa = run(*ab, 1.5), ob = run(*ba, 1.5);
  CHECK(finite(oa) && finite(ob));
  const double d = max_diff(oa, ob, 0.5);
  std::printf("ordering: max |A->B - B->A| = %.4f\n", d);
  CHECK(d > 0.01);
  // bypass is bit-transparent
  auto ref = playing(mix_track());
  auto byp = build(true);
  byp->fx_chain_bypass(0, true);
  const auto r0 = run(*ref, 1.0), r1 = run(*byp, 1.0);
  CHECK(std::memcmp(r0.data() + 4410 * kCh, r1.data() + 4410 * kCh, (r0.size() - 4410 * kCh) * sizeof(float)) == 0);
  byp->fx_chain_bypass(0, false);
  const auto r2 = run(*byp, 0.5), r3 = run(*ref, 0.5);
  CHECK(max_diff(r2, r3, 0.1) > 0.01);  // and comes back
  // an empty chain and wet = 0 are transparent too
  auto empty = playing(mix_track());
  empty->fx_insert(0, EffectKind::Echo);
  empty->fx_chain_clear(0);
  auto ref2 = playing(mix_track());
  const auto e0 = run(*empty, 0.6), e1 = run(*ref2, 0.6);
  CHECK(std::memcmp(e0.data() + 8000 * kCh, e1.data() + 8000 * kCh, (e0.size() - 8000 * kCh) * sizeof(float)) == 0);
  // an isolator at unity keeps the level
  auto iso = playing(mix_track());
  iso->fx_insert(0, EffectKind::Isolator, -1, true);
  auto ref3 = playing(mix_track());
  const auto i0 = run(*iso, 1.0), i1 = run(*ref3, 1.0);
  std::printf("isolator at unity: rms %.4f vs dry %.4f\n", rms(i0, 0.3, 1.0), rms(i1, 0.3, 1.0));
  CHECK(std::fabs(rms(i0, 0.3, 1.0) / rms(i1, 0.3, 1.0) - 1.0) < 0.03);  // flat magnitude (the crossover shifts phase)
}

void test_new_effects() {
  auto level = [&](EffectKind k, std::function<void(IEngine&, int)> setup, std::unique_ptr<LoadedTrack> t, double from, double to) {
    auto e = playing(std::move(t));
    const int id = e->fx_insert(0, k, -1, true);
    setup(*e, id);
    const auto o = run(*e, to + 0.1);
    CHECK(finite(o));
    return rms(o, from, to);
  };
  const double base = level(EffectKind::Isolator, [](IEngine&, int) {}, sine_track(110), 0.5, 1.5);
  const double low_kill = level(EffectKind::Isolator, [](IEngine& e, int id) { e.fx_set(0, id, FxField::Param, 0); }, sine_track(110), 0.5, 1.5);
  const double hi_kill = level(EffectKind::Isolator, [](IEngine& e, int id) { e.fx_set(0, id, FxField::Param3, 0); }, sine_track(6000), 0.5, 1.5);
  const double hi_keep = level(EffectKind::Isolator, [](IEngine& e, int id) { e.fx_set(0, id, FxField::Param, 0); }, sine_track(6000), 0.5, 1.5);
  std::printf("isolator: 110 Hz %.4f -> %.4f with low killed; 6 kHz %.4f with high killed vs %.4f\n", base, low_kill, hi_kill, hi_keep);
  CHECK(low_kill < base * 0.1 && hi_kill < hi_keep * 0.1 && hi_keep > 0.2);
  const double f_lp = level(EffectKind::Filter, [](IEngine& e, int id) { e.fx_set(0, id, FxField::Param, 0.1); }, sine_track(6000), 0.5, 1.5);
  const double f_hp = level(EffectKind::Filter, [](IEngine& e, int id) { e.fx_set(0, id, FxField::Param, 0.9); }, sine_track(110), 0.5, 1.5);
  const double f_off = level(EffectKind::Filter, [](IEngine&, int) {}, sine_track(1000), 0.5, 1.5);
  std::printf("filter: LP kills 6k %.4f, HP kills 110 %.4f, centre keeps 1k %.4f\n", f_lp, f_hp, f_off);
  CHECK(f_lp < 0.05 && f_hp < 0.05 && f_off > 0.3);
  // slicer: a gated pattern leaves silent steps
  {
    auto e = playing(sine_track(440));
    const int id = e->fx_insert(0, EffectKind::Slicer, -1, true);
    e->fx_set(0, id, FxField::Beats, 0.25);
    e->fx_set(0, id, FxField::Param, 0.3);  // 3 hits of 8
    const auto o = run(*e, 3.0);
    int quiet = 0, loud = 0;
    for (std::size_t w = 44100; w + 800 < o.size() / kCh; w += 800) (rms(o, static_cast<double>(w) / kSr, static_cast<double>(w + 800) / kSr) < 0.02 ? quiet : loud)++;
    std::printf("slicer: %d quiet windows, %d loud\n", quiet, loud);
    CHECK(quiet > 10 && loud > 5);
  }
  // build-up: engaged until the next beat after switching off, then clean
  {
    auto ref = playing(sine_track(110));
    auto e = playing(sine_track(110));
    const int id = e->fx_insert(0, EffectKind::BuildUp, -1, true);
    e->fx_set(0, id, FxField::Param, 0.5);
    auto o = run(*e, 0.75);
    e->fx_set(0, id, FxField::On, 0);
    append(o, run(*e, 6.0));
    const auto r = run(*ref, 6.75);
    const double engaged = rms(o, 0.6, 0.74), ref_level = rms(r, 0.6, 0.74);
    std::printf("build-up: engaged level %.3f vs dry %.3f\n", engaged, ref_level);
    CHECK(engaged < ref_level * 0.5);
    double back = -1;
    for (double t = 0.76; t < 2.0; t += 0.01)
      if (rms(o, t, t + 0.01) > 0.5 * ref_level) {
        back = t;
        break;
      }
    std::printf("build-up: output back to normal at %.3f s (beat line at 1.000)\n", back);
    CHECK(back > 0.98 && back < 1.05);
    CHECK(finite(o) && max_diff(o, r, 6.0) < 2e-3);  // echo gone, signal clean
  }
}

void test_clickfree_and_tails() {
  // reorder, remove, insert, replace on a running tone: no step in the output
  auto e = playing(sine_track(440));
  const int p = e->fx_insert(0, EffectKind::Phaser, -1, true);
  const int f = e->fx_insert(0, EffectKind::Filter, -1, true);
  e->fx_set(0, f, FxField::Param, 0.3);
  e->fx_set(0, p, FxField::Wet, 1);
  std::vector<float> all = run(*e, 0.6);
  const std::size_t from = all.size() / kCh;
  e->fx_move(0, p, 1);
  append(all, run(*e, 0.4));
  e->fx_move(0, p, 0);
  append(all, run(*e, 0.4));
  e->fx_remove(0, f);
  append(all, run(*e, 0.4));
  const int c = e->fx_insert(0, EffectKind::Flanger, 0, true);
  append(all, run(*e, 0.4));
  e->fx_replace(0, c, EffectKind::Phaser);
  append(all, run(*e, 0.4));
  e->fx_chain_load(0, "{\"v\":1,\"fx\":[{\"type\":\"flanger\",\"on\":true}]}");
  append(all, run(*e, 0.4));
  CHECK(finite(all));
  const float md = max_delta(all, from);
  std::printf("edits on a 440 Hz tone: max sample delta %.4f (clean tone peaks near 0.031)\n", md);
  CHECK(md < 0.08f);

  // tails ring out: burst, effect switched off (or removed) while the burst plays
  auto burst = [](std::uint64_t i) { return i < 22050 ? 0.4f * static_cast<float>(std::sin(2 * kPi * 440.0 * static_cast<double>(i) / kSr)) : 0.0f; };
  for (int mode = 0; mode < 2; ++mode) {
    auto t = playing(make_track(30, burst));
    const int id = t->fx_insert(0, EffectKind::Echo, -1, true);
    t->fx_set(0, id, FxField::Param, 0.8);
    t->fx_set(0, id, FxField::Beats, 0.25);
    t->fx_set(0, id, FxField::Wet, 1);
    auto o = run(*t, 0.4);
    if (mode == 0) t->fx_set(0, id, FxField::On, 0);
    else t->fx_remove(0, id);
    append(o, run(*t, 24.0));
    const float ring = peak(o, 0.6, 1.4);
    const float end = peak(o, 20.0, 24.4);
    std::printf("tail after %s: peak %.3f at 0.6-1.4 s, %.1e at 20 s+\n", mode == 0 ? "switch-off" : "removal", ring, end);
    CHECK(ring > 0.02f && end < 1e-4f);
  }
}

void test_beat_sync_and_legacy() {
  auto click = [](std::uint64_t i) { return i == 44100 ? 0.9f : 0.0f; };
  double lens[2] = {};
  for (int k = 0; k < 2; ++k) {
    auto e = engine();
    e->load(0, make_track(8, click));
    e->send(cmd(Op::Tempo, 0, k ? 1.0f : 0.0f));
    e->send(cmd(Op::PlayToggle, 0));
    const int id = e->fx_insert(0, EffectKind::Echo, -1, true);
    e->fx_set(0, id, FxField::Beats, 1.0);
    e->fx_set(0, id, FxField::Wet, 1);
    e->fx_set(0, id, FxField::Param, 0.2);
    const auto o = run(*e, 4.0);
    std::vector<std::size_t> pk;
    float top = 0;
    for (std::size_t i = 0; i < o.size() / kCh; ++i) top = std::max(top, std::fabs(o[i * kCh]));
    for (std::size_t i = 1; i + 1 < o.size() / kCh; ++i) {
      const float v = std::fabs(o[i * kCh]);
      if (v > 0.25f * top && v >= std::fabs(o[(i - 1) * kCh]) && v >= std::fabs(o[(i + 1) * kCh]) && (pk.empty() || i - pk.back() > 2000)) pk.push_back(i);
    }
    CHECK(pk.size() >= 2);
    lens[k] = static_cast<double>(pk[1] - pk[0]);
    const double bpm = 120.0 * (k ? 1.06 : 1.0);
    std::printf("echo of one beat at %.1f bpm: %.1f frames (expected %.1f)\n", bpm, lens[k], 60.0 / bpm * kSr);
    CHECK(std::fabs(lens[k] - 60.0 / bpm * kSr) < 12);
  }
  CHECK(lens[0] > lens[1] * 1.04);

  // legacy view
  auto e = playing(sine_track(300));
  e->send(cmd(Op::FxSelect, 0, 0, 1, static_cast<int>(EffectKind::PingPong)));
  e->send(cmd(Op::FxEnable, 0, 1, 1));
  e->send(cmd(Op::FxWet, 0, 0.7f, 1));
  e->send(cmd(Op::FxParam, 0, 0.3f, 1));
  e->send(cmd(Op::FxTime, 0, 0.75f, 1));
  run(*e, 0.1);
  FxSlotView v = e->deck(0).fx[1];
  CHECK(v.kind == EffectKind::PingPong && v.on && std::fabs(v.wet - 0.7f) < 1e-6 && std::fabs(v.param - 0.3f) < 1e-6 && std::fabs(v.beats - 0.75f) < 1e-6);
  CHECK(e->deck(0).fx[0].kind == EffectKind::None && e->deck(0).fx[2].kind == EffectKind::None);
  const FxChainInfo ci = e->fx_chain(0);
  CHECK(ci.count == 1 && ci.unit[0].legacy_slot == 1 && ci.unit[0].on && ci.unit[0].kind == EffectKind::PingPong);
  e->send(cmd(Op::FxSelect, kFxMaster, 0, 2, static_cast<int>(EffectKind::Reverb)));
  run(*e, 0.1);
  CHECK(e->mixer().fx[2].kind == EffectKind::Reverb && e->fx_chain(kFxMaster).count == 1);
  // a scheduled fixed-slot command still works; a scheduled FxSelect is refused (it builds an effect)
  CHECK(e->schedule(cmd(Op::FxSelect, 0, 0, 0, 1), Schedule::at_clock_frame(100)) == 0);
  CHECK(e->schedule(cmd(Op::FxEnable, 0, 0, 1), Schedule::at_clock_frame(e->clock_frames() + 100)) != 0);
  run(*e, 0.1);
  CHECK(!e->deck(0).fx[1].on && !e->fx_chain(0).unit[0].on);
}

void test_buses() {
  // master chain
  {
    auto e = playing(sine_track(110));
    auto r = playing(sine_track(110));
    const int id = e->fx_insert(kFxMaster, EffectKind::Isolator, -1, true);
    e->fx_set(kFxMaster, id, FxField::Param, 0);
    const double a = rms(run(*e, 1.0), 0.5, 1.0), b = rms(run(*r, 1.0), 0.5, 1.0);
    CHECK(a < b * 0.1);
  }
  // microphone chain: offline input fed straight in
  {
    std::vector<float> in(static_cast<std::size_t>(kSr) * 2), out(static_cast<std::size_t>(kSr) * kCh);
    for (int i = 0; i < kSr; ++i) in[static_cast<std::size_t>(i) * 2] = in[static_cast<std::size_t>(i) * 2 + 1] = 0.3f * static_cast<float>(std::sin(2 * kPi * 110.0 * i / kSr));
    double lv[2];
    for (int k = 0; k < 2; ++k) {
      auto e = engine();
      e->send(cmd(Op::MicOn, 0, 0, 1));
      e->send(cmd(Op::MicLevel, 0, 0.8f));
      if (k) {
        const int id = e->fx_insert(kFxTargetMic, EffectKind::Isolator, -1, true);
        e->fx_set(kFxTargetMic, id, FxField::Param, 0);
      }
      for (int done = 0; done < kSr; done += 512) {
        const int n = std::min(512, kSr - done);
        e->render_with_input(out.data() + static_cast<std::size_t>(done) * kCh, in.data() + static_cast<std::size_t>(done) * 2, static_cast<std::uint32_t>(n));
      }
      lv[k] = rms(out, 0.5, 1.0);
    }
    std::printf("microphone chain: %.4f dry, %.4f with the lows killed\n", lv[0], lv[1]);
    CHECK(lv[0] > 0.02 && lv[1] < lv[0] * 0.15);
  }
}

// ---------------------------------------------------------------------------------------------------------
void test_cpu_and_allocs() {
  const EffectKind heavy[kFxChainMax] = {EffectKind::Reverb,  EffectKind::Echo,   EffectKind::PingPong, EffectKind::Flanger,
                                         EffectKind::Phaser,  EffectKind::Pitch,  EffectKind::Spiral,   EffectKind::BuildUp};
  std::printf("per-effect cost, 512-frame block at 44.1 kHz (budget %.0f us):\n", 512.0 / kSr * 1e6);
  for (int k = 1; k < static_cast<int>(EffectKind::Count); ++k) {
    const double ns = fx_bench_ns_per_block(static_cast<EffectKind>(k), 512, kSr, 200);
    std::printf("  %-14s %8.1f us\n", effect_info(static_cast<EffectKind>(k))->name, ns / 1000.0);
  }
  auto make = [&](bool chain) {
    auto e = playing(mix_track());
    if (chain)
      for (EffectKind k : heavy) e->fx_insert(0, k, -1, true);
    run(*e, 0.3, 512);
    return e;
  };
  auto timed = [&](Engine& e) {
    double best = 1e9;
    for (int rep = 0; rep < 5; ++rep) {
      const auto t0 = std::chrono::steady_clock::now();
      run(e, 1.0, 512);
      const auto t1 = std::chrono::steady_clock::now();
      best = std::min(best, std::chrono::duration<double, std::micro>(t1 - t0).count() / (kSr / 512.0));
    }
    return best;
  };
  auto base = make(false), full = make(true);
  const double b = timed(*base), f = timed(*full);
  std::printf("full 8-effect chain (reverb, echo, ping pong, flanger, phaser, pitch, spiral, build-up) on a deck: %.1f us per 512-frame block (engine without chain %.1f us, chain adds %.1f us = %.1f%% of the %.0f us block)\n",
              f, b, f - b, 100.0 * (f - b) / (512.0 / kSr * 1e6), 512.0 / kSr * 1e6);
  CHECK(f - b < 512.0 / kSr * 1e6 * 0.5);

  // allocation-free processing, including blocks that swap chains
  auto e = playing(mix_track());
  for (EffectKind k : heavy) e->fx_insert(0, k, -1, true);
  run(*e, 0.2, 512);
  long allocs_steady, allocs_edit;
  std::vector<float> scratch(512 * kCh);
  auto render_blocks = [&](int blocks) {
    for (int i = 0; i < blocks; ++i) e->render(scratch.data(), 512);
  };
  g_allocs = 0;
  g_armed = true;
  render_blocks(40);
  g_armed = false;
  allocs_steady = g_allocs;
  const int id = e->fx_chain(0).unit[3].id;
  e->fx_move(0, id, 0);  // built here (allocates, off the audio thread), then swapped inside the audio blocks below
  e->fx_remove(0, e->fx_chain(0).unit[5].id);
  e->fx_set(0, id, FxField::Param, 0.2);
  g_allocs = 0;
  g_armed = true;
  render_blocks(40);
  g_armed = false;
  allocs_edit = g_allocs;
  std::printf("allocations while rendering: %ld steady, %ld across a reorder and a removal\n", allocs_steady, allocs_edit);
  CHECK(allocs_steady == 0 && allocs_edit == 0);
}

// ---------------------------------------------------------------------------------------------------------
bool wait_for(const std::function<bool()>& p, double s) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(s);
  while (std::chrono::steady_clock::now() < end) {
    if (p()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return p();
}

void test_remote(const std::string& helper) {
  const std::string dir = (fs::temp_directory_path() / ("dh-fxchain-test-" + std::to_string(ipc::current_pid()))).string();
  fs::remove_all(dir);
  fs::create_directories(dir);
  RemoteEngine::Options o;
  o.data_dir = dir;
  o.helper_path = helper;
  o.null_audio = true;
  o.log_level = 2;
  RemoteEngine remote(o);
  std::string err;
  CHECK(remote.connect(err));
  OutputConfig oc;
  CHECK(remote.start(oc, err));
  CHECK(remote.sample_rate() > 0);

  // the same edit script against both engines: identical results, identical state
  Engine local;
  local.configure_offline(remote.sample_rate(), 2);
  auto script = [&](IEngine& e, std::vector<std::string>& log) {
    log.push_back(std::to_string(e.fx_insert(0, EffectKind::Echo, -1, true)));
    log.push_back(std::to_string(e.fx_insert(0, EffectKind::Isolator)));
    log.push_back(std::to_string(e.fx_insert(0, EffectKind::Filter, 0)));
    log.push_back(std::to_string(e.fx_move(0, 1, 2)));
    log.push_back(std::to_string(e.fx_set(0, 2, FxField::Param2, 0.2)));
    log.push_back(std::to_string(e.fx_set(0, 77, FxField::Param2, 0.2)));
    log.push_back(std::to_string(e.fx_replace(0, 2, EffectKind::Slicer)));
    log.push_back(std::to_string(e.fx_remove(0, 3)));
    log.push_back(std::to_string(e.fx_chain_bypass(kFxMaster, true)));
    log.push_back(std::to_string(e.fx_chain_wet(kFxMaster, 0.4)));
    log.push_back(std::to_string(e.fx_insert(kFxTargetMic, EffectKind::Phaser)));
    log.push_back(std::to_string(e.fx_insert(fx_target_stem(2, 5), EffectKind::BuildUp, -1, true)));
    log.push_back(std::to_string(e.fx_insert(50, EffectKind::Echo)));
    log.push_back(std::to_string(e.fx_chain_load(kFxTargetSampler, "{\"v\":1,\"fx\":[{\"type\":\"roll\"},{\"type\":\"isolator\",\"p3\":0.1}]}")));
    log.push_back(std::to_string(e.fx_chain_load(kFxTargetSampler, "garbage")));
    e.send(cmd(Op::FxSelect, 1, 0, 0, static_cast<int>(EffectKind::Flanger)));
    e.send(cmd(Op::FxSelect, 1, 0, 2, static_cast<int>(EffectKind::Reverb)));
    e.send(cmd(Op::FxSelect, 1, 0, 0, 0));
    log.push_back(e.fx_chain_save(0));
  };
  std::vector<std::string> la, lb;
  script(local, la);
  script(remote, lb);
  CHECK(la == lb);
  const int targets[] = {0, 1, kFxMaster, kFxTargetMic, kFxTargetSampler, fx_target_stem(2, 5), 3};
  for (int t : targets) {
    const bool same = wait_for([&] { return sig(local.fx_chain(t)) == sig(remote.fx_chain(t)); }, 3);
    if (!same) std::printf("mismatch on target %d:\n  local  %s\n  remote %s\n", t, sig(local.fx_chain(t)).c_str(), sig(remote.fx_chain(t)).c_str());
    CHECK(same);
  }
  const FxChainInfo rc = remote.fx_chain(0);
  CHECK(rc.count == 2 && rc.unit[0].kind == EffectKind::Slicer && rc.unit[1].kind == EffectKind::Echo);
  std::printf("remote parity: %zu script results and %zu chains identical\n", la.size(), sizeof(targets) / sizeof(targets[0]));
  // a hostile request is refused, not trusted
  FxChainEdit bad;
  bad.verb = static_cast<FxChainVerb>(200);
  FxChainResult br;
  CHECK(!remote.fx_chain_edit(bad, br));

  // and the audio really changes: stream tap of a playing sine, low band killed after a moment
  remote.load(0, sine_track(110, 20));
  CHECK(wait_for([&] { return remote.deck(0).loaded; }, 10));
  remote.send(cmd(Op::MasterLevel, 0, 0.8f));
  remote.stream_enable(true, false, false);
  remote.send(cmd(Op::PlayToggle, 0));
  remote.fx_chain_bypass(kFxMaster, false);  // the script above left it bypassed at 40 % wet
  remote.fx_chain_wet(kFxMaster, 1.0);
  const int iso = remote.fx_insert(kFxMaster, EffectKind::Isolator, -1, true);
  std::vector<float> buf(4096 * 2);
  auto drain_rms = [&](double seconds) {
    double sum = 0;
    std::size_t n = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
      const std::size_t got = remote.stream_read(buf.data(), 4096);
      for (std::size_t i = 0; i < got; ++i) sum += static_cast<double>(buf[i * 2]) * buf[i * 2], ++n;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
  };
  // Real time through a helper process: on a slow or busy machine the stream may take a while to
  // carry audio and the edit a while to land, so wait for each state rather than for a fixed time.
  double unity = 0;
  for (int i = 0; i < 40 && unity <= 0.02; ++i) unity = drain_rms(0.5);
  remote.fx_set(kFxMaster, iso, FxField::Param, 0);
  double killed = unity;
  for (int i = 0; i < 40 && !(killed < unity * 0.2); ++i) killed = drain_rms(0.5);
  std::printf("remote audio through the master chain: %.4f at unity, %.4f with the lows killed\n", unity, killed);
  CHECK(unity > 0.02 && killed < unity * 0.2);
  remote.stream_enable(false);
  remote.shutdown_remote(50);
  fs::remove_all(dir);
}

}  // namespace

int main(int, char** argv) {
  {
    auto e = engine();
    test_edits_and_presets(*e, false);
  }
  test_ordering_bypass();
  test_new_effects();
  test_clickfree_and_tails();
  test_beat_sync_and_legacy();
  test_buses();
  test_cpu_and_allocs();
#if !defined(_WIN32)
  std::string helper = (fs::absolute(argv[0]).parent_path() / "deckhand-engine").string();
  if (const char* h = std::getenv("DECKHAND_ENGINE_BIN")) helper = h;
  if (fs::exists(helper)) test_remote(helper);
  else std::printf("deckhand-engine not built: skipping the remote parity test\n");
#else
  (void)argv;
#endif
  std::printf("fx chain ok\n");
  return 0;
}
