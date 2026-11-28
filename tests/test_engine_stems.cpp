// Stem playback: transparency against normal playback, mute / solo / level math, gain ramps, attach / detach while
// playing, per-stem effect chains, allocation-free rendering, CPU and memory, and engine-process parity.
// Synthetic stems (four distinct tones) written through the real stem cache; no model needed.
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
#include <random>
#include <thread>
#include <vector>

#include "../src/audio/stemset.hpp"
#include "check.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/remote_engine.hpp"
#include "deckhand/stems/cache.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

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
const double kHz[4] = {1300.0, 6000.0, 110.0, 3100.0};  // vocals, drums, bass, other
const float kAmp[4] = {0.10f, 0.10f, 0.14f, 0.08f};

struct World {
  fs::path dir;
  std::string key;
  std::string path;
  stems::Stems loaded;  // the dequantised stems exactly as stored
  std::vector<float> mix;  // sum of them (interleaved)
  std::uint64_t frames = 0;
};

World make_world(double seconds) {
  World w;
  w.dir = fs::temp_directory_path() / ("dh_stems_play_" + std::to_string(std::random_device{}()));
  fs::create_directories(w.dir);
  stems::CacheConfig cfg;
  cfg.root = w.dir;
  stems::StemCache cache(cfg);
  stems::Stems st;
  const std::size_t n = static_cast<std::size_t>(seconds * kSr);
  for (int s = 0; s < 4; ++s) {
    st[static_cast<std::size_t>(s)].sample_rate = kSr;
    st[static_cast<std::size_t>(s)].l.resize(n);
    st[static_cast<std::size_t>(s)].r.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / kSr;
      // a slow amplitude wobble keeps the similarity search of the stretcher from facing exact ties
      const double env = 1.0 + 0.3 * std::sin(2 * kPi * (0.7 + 0.31 * s) * t);
      st[static_cast<std::size_t>(s)].l[i] = kAmp[s] * static_cast<float>(env * std::sin(2 * kPi * kHz[s] * t));
      st[static_cast<std::size_t>(s)].r[i] = kAmp[s] * static_cast<float>(env * std::sin(2 * kPi * kHz[s] * t + 0.4 * (s + 1)));
    }
  }
  w.key = std::string(64, 'a');
  std::string err;
  CHECK(cache.store(w.key, st, &err));
  w.path = cache.path_for(w.key).string();
  auto r = cache.open(w.key);
  CHECK(r && r->load_all(w.loaded));
  w.frames = n;
  w.mix.assign(n * 2, 0.f);
  for (std::size_t i = 0; i < n; ++i)
    for (int s = 0; s < 4; ++s) {
      w.mix[i * 2] += w.loaded[static_cast<std::size_t>(s)].l[i];
      w.mix[i * 2 + 1] += w.loaded[static_cast<std::size_t>(s)].r[i];
    }
  return w;
}

// Mix of the stems with per-stem gains (the reference a stem state must reproduce).
std::unique_ptr<LoadedTrack> track_of(const World& w, const float g[4] = nullptr, std::int64_t id = 5) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = kSr;
  audio->frames = w.frames;
  audio->pcm.assign(w.frames * 2, 0.f);
  for (std::size_t i = 0; i < w.frames; ++i)
    for (int s = 0; s < 4; ++s) {
      const float gg = g ? g[s] : 1.0f;
      audio->pcm[i * 2] += gg * w.loaded[static_cast<std::size_t>(s)].l[i];
      audio->pcm[i * 2 + 1] += gg * w.loaded[static_cast<std::size_t>(s)].r[i];
    }
  build_waveform(*audio);
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->track_id = id;
  t->bpm = 120;
  t->grid.push_back({0, 120, 1});
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

std::unique_ptr<Engine> engine() {
  auto e = std::make_unique<Engine>();
  e->configure_offline(kSr, kCh);
  return e;
}

std::vector<float> run(Engine& e, double seconds, std::uint32_t block = 256) {
  const std::uint32_t frames = static_cast<std::uint32_t>(seconds * kSr);
  std::vector<float> out(static_cast<std::size_t>(frames) * kCh);
  for (std::uint32_t done = 0; done < frames;) {
    const std::uint32_t n = std::min<std::uint32_t>(block, frames - done);
    e.render(out.data() + static_cast<std::size_t>(done) * kCh, n);
    done += n;
  }
  e.collect_garbage();
  return out;
}

// Loads `t` on deck 0 and, when `w` is given, attaches its stems and renders until they are in.
void setup(Engine& e, std::unique_ptr<LoadedTrack> t, const World* w) {
  e.load(0, std::move(t));
  float tmp[256 * kCh];
  e.render(tmp, 256);
  if (!w) return;
  std::string err;
  CHECK(e.stems_attach(0, 5, w->path, err));
  // no deadline to speak of: the background loader has taken many seconds on a loaded machine (only a hung test stops here)
  for (int i = 0; i < 30000 && e.deck(0).stems_state != 2; ++i) {
    e.render(tmp, 64);
    if (e.deck(0).stems_state == 3) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  e.render(tmp, 256);
  CHECK(e.deck(0).stems_state == 2);
  CHECK(e.deck(0).stems_mix > 0.999f);
}

double max_diff(const std::vector<float>& a, const std::vector<float>& b, std::size_t from = 0) {
  CHECK(a.size() == b.size());
  double m = 0;
  for (std::size_t i = from; i < a.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}
double max_step(const std::vector<float>& a, std::size_t from = 0) {
  double m = 0;
  for (std::size_t i = from + kCh; i < a.size(); i += kCh) m = std::max(m, static_cast<double>(std::fabs(a[i] - a[i - kCh])));
  return m;
}
// Amplitude of a sine of `hz` in channel 0 (Goertzel).
double tone(const std::vector<float>& b, double hz, std::size_t from = 0) {
  const std::size_t n = b.size() / kCh - from;
  double re = 0, im = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double ph = 2 * kPi * hz * static_cast<double>(i) / kSr;
    const double x = b[(from + i) * kCh];
    re += x * std::cos(ph);
    im += x * std::sin(ph);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

using Setup = std::function<void(Engine&)>;
// Plays the same commands on a plain engine and on one with stems at unity; returns the largest difference.
double transparency(const World& w, const Setup& s, double secs = 1.5) {
  auto a = engine(), b = engine();
  setup(*a, track_of(w), nullptr);
  setup(*b, track_of(w), &w);
  s(*a);
  s(*b);
  a->send(cmd(Op::PlayToggle, 0));
  b->send(cmd(Op::PlayToggle, 0));
  const auto xa = run(*a, secs), xb = run(*b, secs);
  double energy = 0;
  for (float v : xa) energy += std::fabs(v);
  CHECK(energy / static_cast<double>(xa.size()) > 0.01);  // it really played something
  return max_diff(xa, xb);
}

double ns_per_block(Engine& e, int blocks) {
  float out[256 * kCh];
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < blocks; ++i) e.render(out, 256);
  return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / blocks;
}

}  // namespace

int main(int, char** argv) {
  const World w = make_world(8.0);
  std::printf("stem world: %llu frames, %s\n", static_cast<unsigned long long>(w.frames), w.path.c_str());

  // ---- the file loader agrees with the cache reader, and the memory cost is what the docs say ----
  {
    StemSet set;
    std::string err;
    CHECK(stemset_load_file(w.path, set, err));
    CHECK(set.frames == static_cast<std::int64_t>(w.frames) && set.rate == kSr && set.bits == 16);
    for (std::size_t i : {std::size_t(0), std::size_t(12345), static_cast<std::size_t>(w.frames - 1)})
      for (int s = 0; s < 4; ++s) {
        CHECK(std::fabs(set.s16[i * 8 + static_cast<std::size_t>(s) * 2] * set.lsb - w.loaded[static_cast<std::size_t>(s)].l[i]) < 1e-7f);
        CHECK(std::fabs(set.s16[i * 8 + static_cast<std::size_t>(s) * 2 + 1] * set.lsb - w.loaded[static_cast<std::size_t>(s)].r[i]) < 1e-7f);
      }
    const double mb_min = static_cast<double>(set.bytes()) / (static_cast<double>(w.frames) / kSr / 60.0) / 1e6;
    std::printf("memory: %.1f MB per minute per deck (16-bit stems)\n", mb_min);
    CHECK(set.bytes() == w.frames * 16);
    std::string bad;
    CHECK(!stemset_load_file(w.path + ".missing", set, bad));
  }

  // ---- unity sum is transparent: plain playback, tempo, key lock, loop, reverse ----
  {
    const double plain = transparency(w, [](Engine&) {});
    const double tempo = transparency(w, [](Engine& e) { e.send(cmd(Op::Tempo, 0, 0.5f)); });
    const double klock = transparency(w, [](Engine& e) {
      e.send(cmd(Op::Tempo, 0, 0.6f));
      e.send(cmd(Op::KeyLock, 0, 0, 1));
    });
    const double shift = transparency(w, [](Engine& e) {
      e.send(cmd(Op::KeyLock, 0, 0, 1));
      e.send(cmd(Op::KeyShift, 0, 3.0f));
    });
    const double loop = transparency(w, [](Engine& e) { e.send(cmd(Op::BeatLoop, 0, 0.25f)); });
    const double rev = transparency(w, [](Engine& e) {
      e.send(cmd(Op::Seek, 0, 0, 0, 5.0 * kSr));
      e.send(cmd(Op::Reverse, 0, 0, 1));
    });
    std::printf("transparency max |diff|: plain %.2e tempo %.2e keylock %.2e keyshift %.2e loop %.2e reverse %.2e\n", plain,
                tempo, klock, shift, loop, rev);
    CHECK(plain < 1e-6 && tempo < 1e-6 && loop < 1e-6 && rev < 1e-6);
    CHECK(klock < 1e-5 && shift < 1e-5);
  }

  // ---- mute / solo / level math: each state equals normal playback of the matching mix ----
  {
    struct Case {
      const char* name;
      std::function<void(Engine&)> set;
      float g[4];
    };
    const std::vector<Case> cases = {
        {"mute vocals", [](Engine& e) { e.stem_set(0, 0, StemField::Mute, 1); }, {0, 1, 1, 1}},
        {"solo bass", [](Engine& e) { e.stem_set(0, 2, StemField::Solo, 1); }, {0, 0, 1, 0}},
        {"drums at 0.5", [](Engine& e) { e.stem_set(0, 1, StemField::Level, 0.5); }, {1, 0.5f, 1, 1}},
        {"other at 1.5", [](Engine& e) { e.stem_set(0, 3, StemField::Level, 1.5); }, {1, 1, 1, 1.5f}},
        {"isolate vocals", [](Engine& e) { e.stem_set(0, 0, StemField::Isolate, 1); }, {1, 0, 0, 0}},
        {"solo wins over mute", [](Engine& e) { e.stem_set(0, 1, StemField::Mute, 1); e.stem_set(0, 1, StemField::Solo, 1); }, {0, 1, 0, 0}},
        {"acapella", [](Engine& e) { e.stem_preset(0, StemPresetId::Acapella); }, {1, 0, 0, 0}},
        {"instrumental", [](Engine& e) { e.stem_preset(0, StemPresetId::Instrumental); }, {0, 1, 1, 1}},
        {"drums only", [](Engine& e) { e.stem_preset(0, StemPresetId::DrumsOnly); }, {0, 1, 0, 0}},
        {"no drums", [](Engine& e) { e.stem_preset(0, StemPresetId::NoDrums); }, {1, 0, 1, 1}},
    };
    for (const Case& c : cases) {
      auto a = engine(), b = engine();
      setup(*a, track_of(w, c.g), nullptr);
      setup(*b, track_of(w), &w);
      c.set(*b);
      a->send(cmd(Op::PlayToggle, 0));
      b->send(cmd(Op::PlayToggle, 0));
      const auto xa = run(*a, 1.0), xb = run(*b, 1.0);
      const std::size_t skip = 2000 * kCh;  // the gain ramp (10 ms) has settled
      const double d = max_diff(xa, xb, skip);
      std::printf("  %-20s max |diff| after the ramp %.2e\n", c.name, d);
      CHECK(d < 2e-5);  // float rounding of the sum, amplified by the channel EQ's low shelf; one 16-bit step is 3e-5
    }
    // reset brings everything back
    auto b = engine();
    setup(*b, track_of(w), &w);
    b->stem_set(0, 0, StemField::Mute, 1);
    b->stem_set(0, 1, StemField::Level, 0.2);
    b->stem_preset(0, StemPresetId::Reset);
    float tmp[256 * kCh];
    b->render(tmp, 256);
    const DeckView v = b->deck(0);
    CHECK(v.stem_mute == 0 && v.stem_solo == 0 && v.stem_level[1] == 1.0f);
    // toggle with f < 0, state in the snapshot
    b->stem_set(0, 2, StemField::Mute, -1);
    b->stem_set(0, 3, StemField::Isolate, -1);
    b->render(tmp, 256);
    CHECK(b->deck(0).stem_mute == 4u && b->deck(0).stem_isolate == 8u);
    b->stem_set(0, 2, StemField::Mute, -1);
    b->render(tmp, 256);
    CHECK(b->deck(0).stem_mute == 0u);
  }

  // ---- gain ramps: a mute never steps ----
  {
    auto a = engine(), b = engine();
    setup(*a, track_of(w), &w);
    setup(*b, track_of(w), &w);
    a->send(cmd(Op::PlayToggle, 0));
    b->send(cmd(Op::PlayToggle, 0));
    run(*a, 0.3);
    run(*b, 0.3);
    b->stem_set(0, 2, StemField::Mute, 1);  // the loudest stem (bass)
    const auto xa = run(*a, 0.3), xb = run(*b, 0.3);
    std::vector<float> diff(xa.size());
    for (std::size_t i = 0; i < xa.size(); ++i) diff[i] = xa[i] - xb[i];  // the removed bass, ramped
    const double step = max_step(diff);
    const double full = kAmp[2] * 2 * kPi * kHz[2] / kSr;  // steepest the bass tone itself is
    std::printf("mute ramp: largest step of the removed stem %.4f (tone slope %.4f, amplitude %.2f)\n", step, full, kAmp[2]);
    CHECK(step < 0.02);
    // and the mute ends at silence of that stem
    CHECK(tone(xb, kHz[2], 6000 * kCh / kCh) < 0.002 && tone(xa, kHz[2], 6000) > 0.1);
  }

  // ---- attach / detach while playing: click free, the playhead does not move ----
  {
    auto a = engine(), b = engine();
    setup(*a, track_of(w), nullptr);
    setup(*b, track_of(w), nullptr);
    a->send(cmd(Op::PlayToggle, 0));
    b->send(cmd(Op::PlayToggle, 0));
    run(*a, 0.2);
    run(*b, 0.2);
    // stems arrive with vocals already muted: the blend goes mix -> mix without vocals
    b->stem_set(0, 0, StemField::Mute, 1);
    std::string err;
    CHECK(b->stems_attach(0, 5, w.path, err));
    std::vector<float> xa, xb;
    // Wait on the clock, not on a number of blocks: how long the loader thread takes depends on the machine's load. Both
    // engines render the same blocks in lockstep, so what they play stays comparable whenever the stems arrive.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (b->deck(0).stems_state != 2 && std::chrono::steady_clock::now() < deadline) {
      auto p = run(*a, 0.01), q = run(*b, 0.01);
      xa.insert(xa.end(), p.begin(), p.end());
      xb.insert(xb.end(), q.begin(), q.end());
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(b->deck(0).stems_state == 2);
    while (b->deck(0).stems_mix < 0.999f && std::chrono::steady_clock::now() < deadline) {  // the blend in is frame-driven: it completes
      auto p = run(*a, 0.01), q = run(*b, 0.01);
      xa.insert(xa.end(), p.begin(), p.end());
      xb.insert(xb.end(), q.begin(), q.end());
    }
    CHECK(b->deck(0).stems_mix > 0.999f);
    std::vector<float> diff(xa.size());
    for (std::size_t i = 0; i < xa.size(); ++i) diff[i] = xa[i] - xb[i];
    const double step = max_step(diff);
    std::printf("attach while playing: largest step of the change %.4f\n", step);
    CHECK(step < 0.03);  // the removed vocal tone itself slopes 0.024 per sample at its peak: no step beyond that
    CHECK(std::fabs(a->deck(0).position - b->deck(0).position) < 1e-6);
    // detach: back to the original mix, position untouched
    b->stems_detach(0);
    run(*a, 0.01);
    run(*b, 0.01);
    while ((b->deck(0).stems_state != 0 || b->deck(0).stems_mix > 0.0f) && std::chrono::steady_clock::now() < deadline) {
      run(*a, 0.01);
      run(*b, 0.01);
    }
    // After the blend the engine plays the original mix again, but a slow tail of the stem path (about -100 dB, decaying over a
    // few seconds) is still dying out, and how much of it is left depends on exactly when the stems were adopted relative to
    // the render blocks. So compare once it has decayed, judged on audio frames (deterministic), not on wall time.
    double tail = 1;
    int rounds = 0;
    for (; rounds < 400; ++rounds) {  // at most 40 s of audio
      xa = run(*a, 0.1);
      xb = run(*b, 0.1);
      tail = max_diff(std::vector<float>(xa.end() - 4000, xa.end()), std::vector<float>(xb.end() - 4000, xb.end()));
      if (tail < 1e-9) break;
    }
    std::printf("detach: the original mix is exact again after %.1f s (last difference %.2e)\n", rounds * 0.1, tail);
    CHECK(std::fabs(a->deck(0).position - b->deck(0).position) < 1e-6);
    CHECK(b->deck(0).stems_state == 0);
    CHECK(tail < 1e-9);
    for (std::size_t i = 0; i < xa.size(); ++i) diff[i % diff.size()] = 0;
    // and toggling the stems off and on while playing
    auto c = engine(), pl = engine();
    setup(*c, track_of(w), &w);
    setup(*pl, track_of(w), nullptr);
    c->send(cmd(Op::PlayToggle, 0));
    pl->send(cmd(Op::PlayToggle, 0));
    run(*c, 0.1);
    run(*pl, 0.1);
    c->stem_set(0, 0, StemField::Mute, 1);  // muted vocals, then the stems are switched out and in again
    c->stems_enable(0, 0);
    auto t0 = run(*c, 0.1), p0 = run(*pl, 0.1);
    CHECK(c->deck(0).stems_mix < 1e-3f);
    std::vector<float> df(t0.size());
    for (std::size_t i = 0; i < t0.size(); ++i) df[i] = t0[i] - p0[i];
    CHECK(max_step(df) < 0.03);
    CHECK(max_diff(std::vector<float>(t0.end() - 2000, t0.end()), std::vector<float>(p0.end() - 2000, p0.end())) < 2e-4);  // channel EQ state still settling from the muted vocals
    c->stems_enable(0, 1);
    auto t1 = run(*c, 0.1), p1 = run(*pl, 0.1);
    CHECK(c->deck(0).stems_mix > 0.999f);
    for (std::size_t i = 0; i < t1.size(); ++i) df[i] = t1[i] - p1[i];
    CHECK(max_step(df) < 0.03);
  }

  // ---- a stem effect chain affects only its own stem ----
  {
    auto a = engine(), b = engine();
    setup(*a, track_of(w), &w);
    setup(*b, track_of(w), &w);
    const int tgt = fx_target_stem(0, 1);  // drums (6 kHz)
    const int id = b->fx_insert(tgt, EffectKind::Filter, -1, true);
    CHECK(id > 0);
    CHECK(b->fx_set(tgt, id, FxField::Param, 0.0));  // heavy low-pass
    CHECK(b->fx_set(tgt, id, FxField::Wet, 1.0));
    a->send(cmd(Op::PlayToggle, 0));
    b->send(cmd(Op::PlayToggle, 0));
    run(*a, 0.3);
    run(*b, 0.3);
    const auto xa = run(*a, 1.0), xb = run(*b, 1.0);
    const std::size_t skip = 4000;
    const double d_a = tone(xa, kHz[1], skip), d_b = tone(xb, kHz[1], skip);
    const double b_a = tone(xa, kHz[2], skip), b_b = tone(xb, kHz[2], skip);
    const double v_a = tone(xa, kHz[0], skip), v_b = tone(xb, kHz[0], skip);
    std::printf("stem fx: drums tone %.4f -> %.4f, bass %.4f -> %.4f, vocals %.4f -> %.4f; stem_fx mask %u\n", d_a, d_b, b_a, b_b,
                v_a, v_b, b->deck(0).stem_fx);
    CHECK(d_b < d_a * 0.5);
    CHECK(std::fabs(b_b - b_a) < b_a * 0.02 && std::fabs(v_b - v_a) < v_a * 0.02);
    CHECK((b->deck(0).stem_fx & 2u) != 0 && (a->deck(0).stem_fx == 0));
    // with key lock and a tempo change the split-out stem has its own stretcher and everything stays finite
    b->send(cmd(Op::Tempo, 0, 0.5f));
    b->send(cmd(Op::KeyLock, 0, 0, 1));
    const auto xk = run(*b, 1.0);
    bool finite = true;
    for (float v : xk) finite = finite && std::isfinite(v) && std::fabs(v) < 2.0f;
    CHECK(finite);
    // bypass takes the stem back into the shared path with no step
    b->fx_chain_bypass(tgt, true);
    run(*b, 0.5);
    CHECK((b->deck(0).stem_fx & 2u) == 0);
  }

  // ---- allocation free while playing, with and without a stem chain, and across attach / mute ----
  {
    auto b = engine();
    setup(*b, track_of(w), &w);
    const int tgt = fx_target_stem(0, 2);
    const int id = b->fx_insert(tgt, EffectKind::Echo, -1, true);
    CHECK(id > 0);
    b->send(cmd(Op::PlayToggle, 0));
    b->send(cmd(Op::Tempo, 0, 0.3f));
    b->send(cmd(Op::KeyLock, 0, 0, 1));
    run(*b, 0.3);
    float out[256 * kCh];
    g_allocs = 0;
    g_armed = true;
    for (int i = 0; i < 200; ++i) {
      if (i == 50) b->stem_set(0, 0, StemField::Mute, 1);
      if (i == 80) b->stem_preset(0, StemPresetId::DrumsOnly);
      if (i == 120) b->stems_enable(0, 0);
      if (i == 150) b->stems_enable(0, 1);
      b->render(out, 256);
    }
    g_armed = false;
    std::printf("allocations on the audio thread over 200 blocks (stems on, stem chain, key lock): %ld\n", g_allocs.load());
    CHECK(g_allocs.load() == 0);
  }

  // ---- CPU per 256-frame block (informational: no timing thresholds) ----
  {
    auto off = engine(), on = engine(), fx = engine(), kl_off = engine(), kl_on = engine();
    setup(*off, track_of(w), nullptr);
    setup(*on, track_of(w), &w);
    setup(*fx, track_of(w), &w);
    setup(*kl_off, track_of(w), nullptr);
    setup(*kl_on, track_of(w), &w);
    const int id = fx->fx_insert(fx_target_stem(0, 0), EffectKind::Filter, -1, true);
    fx->fx_set(fx_target_stem(0, 0), id, FxField::Param, 0.2);
    for (Engine* e : {off.get(), on.get(), fx.get(), kl_off.get(), kl_on.get()}) {
      e->send(cmd(Op::PlayToggle, 0));
      e->send(cmd(Op::Tempo, 0, 0.3f));
    }
    kl_off->send(cmd(Op::KeyLock, 0, 0, 1));
    kl_on->send(cmd(Op::KeyLock, 0, 0, 1));
    for (Engine* e : {off.get(), on.get(), fx.get(), kl_off.get(), kl_on.get()}) ns_per_block(*e, 100);
    const double c_off = ns_per_block(*off, 600), c_on = ns_per_block(*on, 600), c_fx = ns_per_block(*fx, 600);
    const double k_off = ns_per_block(*kl_off, 300), k_on = ns_per_block(*kl_on, 300);
    std::printf("cpu per 256-frame block, one deck (tempo +3%%): plain %.1f us, stems %.1f us, stems + 1 stem chain %.1f us; key lock: plain %.1f us, stems %.1f us\n",
                c_off / 1e3, c_on / 1e3, c_fx / 1e3, k_off / 1e3, k_on / 1e3);
    CHECK(c_off > 0 && c_on > 0);
  }

  // ---- the engine process: same commands, same snapshot ----
  std::string helper = (fs::absolute(argv[0]).parent_path() / "deckhand-engine").string();
  if (const char* h = std::getenv("DECKHAND_ENGINE_BIN")) helper = h;
  if (fs::exists(helper)) {
    const fs::path data = w.dir / "remote";
    fs::create_directories(data);
    RemoteEngine::Options o;
    o.data_dir = data.string();
    o.helper_path = helper;
    o.null_audio = true;
    o.auto_restart = false;
    RemoteEngine re(o);
    std::string err;
    CHECK(re.connect(err));
    OutputConfig oc;
    oc.null_backend = true;
    CHECK(re.start(oc, err));
    auto ref = engine();
    setup(*ref, track_of(w), &w);
    re.load(0, track_of(w));
    auto wait = [&](auto pred) {
      for (int i = 0; i < 12000; ++i) {  // up to a minute: waits for the helper process, no timing is asserted
        if (pred(re.deck(0))) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return false;
    };
    CHECK(wait([](const DeckView& v) { return v.loaded; }));
    CHECK(re.stems_attach(0, 5, w.path, err));
    CHECK(wait([](const DeckView& v) { return v.stems_state == 2; }));
    for (IEngine* e : {static_cast<IEngine*>(&re), static_cast<IEngine*>(ref.get())}) {
      e->stem_set(0, 0, StemField::Mute, 1);
      e->stem_set(0, 2, StemField::Solo, 1);
      e->stem_set(0, 3, StemField::Level, 0.75);
      e->stem_set(0, 1, StemField::Isolate, 1);
    }
    float tmp[256 * kCh];
    ref->render(tmp, 256);
    CHECK(wait([](const DeckView& v) { return v.stem_mute == 1u && v.stem_solo == 4u && v.stem_isolate == 2u; }));
    const DeckView r = re.deck(0), l = ref->deck(0);
    CHECK(r.stem_mute == l.stem_mute && r.stem_solo == l.stem_solo && r.stem_isolate == l.stem_isolate);
    for (int s = 0; s < 4; ++s) CHECK(r.stem_level[s] == l.stem_level[s]);
    CHECK(r.stems_on == l.stems_on && r.stems_state == l.stems_state);
    re.stems_detach(0);
    CHECK(wait([](const DeckView& v) { return v.stems_state == 0; }));
    // a request for something that is not a stem file is refused by the engine process
    CHECK(!re.stems_attach(0, 5, (w.dir / "x.wav").string(), err));
    re.shutdown_remote(50);
    std::printf("engine process parity ok\n");
  }

  std::error_code ec;
  fs::remove_all(w.dir, ec);
  std::printf("test_engine_stems ok\n");
  return 0;
}
