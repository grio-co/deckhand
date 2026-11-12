// Offline tests for key lock, quantise, slip, effects, sampler, recording and the stream tap.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"

using namespace deckhand;

namespace {

const double kPi = 3.14159265358979323846;
constexpr int kCh = 4;

// Track whose sample i (mono) is f(i); grid at `bpm` starting at `first`.
std::unique_ptr<LoadedTrack> make_track(double seconds, const std::function<float(std::uint64_t)>& f, double bpm = 120,
                                        double first = 0, std::uint32_t sr = 44100) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = sr;
  audio->frames = static_cast<std::uint64_t>(seconds * sr);
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i) audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = f(i);
  build_waveform(*audio);
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->bpm = bpm;
  t->grid.push_back({first, bpm, 1});
  return t;
}
std::unique_ptr<LoadedTrack> sine(double hz, double seconds, double bpm = 120, double first = 0) {
  return make_track(seconds, [=](std::uint64_t i) { return 0.5f * static_cast<float>(std::sin(2 * kPi * hz * static_cast<double>(i) / 44100)); }, bpm, first);
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

std::vector<float> run(Engine& e, double seconds) {
  const std::uint32_t frames = static_cast<std::uint32_t>(seconds * 44100);
  std::vector<float> out(static_cast<std::size_t>(frames) * kCh);
  for (std::uint32_t done = 0; done < frames;) {
    const std::uint32_t n = std::min<std::uint32_t>(256, frames - done);
    e.render(out.data() + static_cast<std::size_t>(done) * kCh, n);
    done += n;
  }
  return out;
}
std::unique_ptr<Engine> engine() {
  auto e = std::make_unique<Engine>();
  e->configure_offline(44100, kCh);
  return e;
}

// Frequency of channel 0 from positive-going zero crossings (sub-sample interpolation).
double freq_of(const std::vector<float>& b, std::size_t from = 0) {
  double first = -1, last = -1;
  int count = 0;
  for (std::size_t i = from + 1; i < b.size() / kCh; ++i) {
    const float a = b[(i - 1) * kCh], c = b[i * kCh];
    if (a < 0 && c >= 0) {
      const double t = static_cast<double>(i - 1) + static_cast<double>(-a) / static_cast<double>(c - a);
      if (first < 0) first = t;
      last = t;
      ++count;
    }
  }
  return count > 1 ? (count - 1) * 44100.0 / (last - first) : 0;
}
float max_delta(const std::vector<float>& b, std::size_t from = 0) {
  float m = 0;
  for (std::size_t i = from + 1; i < b.size() / kCh; ++i) m = std::max(m, std::fabs(b[i * kCh] - b[(i - 1) * kCh]));
  return m;
}
float peak_of(const std::vector<float>& b, std::size_t from, std::size_t to) {
  float m = 0;
  for (std::size_t i = from; i < std::min(to, b.size() / kCh); ++i) m = std::max(m, std::fabs(b[i * kCh]));
  return m;
}
bool finite(const std::vector<float>& b) {
  for (float v : b)
    if (!std::isfinite(v)) return false;
  return true;
}

}  // namespace

int main() {
  // ---- key lock ------------------------------------------------------------------------------
  for (float sign : {1.0f, -1.0f}) {
    auto e = engine();
    e->load(0, sine(440, 40));
    run(*e, 0.05);
    e->send(cmd(Op::TempoRange, 0, 0.08f));
    e->send(cmd(Op::Tempo, 0, sign));
    e->send(cmd(Op::KeyLock, 0, 0, 1));
    e->send(cmd(Op::HeadMix, 0, 0.0f));
    e->send(cmd(Op::PlayToggle, 0));
    run(*e, 1.0);
    const double p0 = e->deck(0).position;
    const auto out = run(*e, 3.0);
    const double f = freq_of(out);
    const double adv = e->deck(0).position - p0;
    std::printf("key lock %+d%%: %.2f Hz, advanced %.4f s of source in 3 s\n", sign > 0 ? 8 : -8, f, adv / 44100);
    CHECK(e->deck(0).key_lock_active);
    CHECK(std::fabs(f - 440.0) < 440.0 * 0.005);
    CHECK(std::fabs(adv - 3.0 * 44100 * (1.0 + 0.08 * sign)) < 3.0 * 44100 * 0.004);
    e->send(cmd(Op::KeyLock, 0, 0, 0));
    const double f_off = freq_of(run(*e, 2.0), 22050);
    CHECK(std::fabs(f_off - 440.0 * (1.0 + 0.08 * sign)) < 440 * 0.005);  // varispeed moves the pitch
  }
  {  // key shift: +12 semitones = one octave, tempo untouched
    auto e = engine();
    e->load(0, sine(440, 30));
    e->send(cmd(Op::KeyShift, 0, 12.0f));
    e->send(cmd(Op::KeyLock, 0, 0, 1));
    e->send(cmd(Op::PlayToggle, 0));
    run(*e, 1.0);
    const double p0 = e->deck(0).position;
    const double f = freq_of(run(*e, 2.0));
    CHECK(std::fabs(f - 880.0) < 880 * 0.005);
    CHECK(std::fabs((e->deck(0).position - p0) - 2.0 * 44100) < 400);
  }
  {  // bit-exact bypass at ~1.0 and after switching key lock off again
    auto a = engine(), b = engine();
    for (Engine* e : {a.get(), b.get()}) {
      e->load(0, sine(440, 30));
      e->send(cmd(Op::TempoRange, 0, 0.0005f));
      e->send(cmd(Op::Tempo, 0, 1.0f));
      e->send(cmd(Op::PlayToggle, 0));
    }
    b->send(cmd(Op::KeyLock, 0, 0, 1));
    const auto oa = run(*a, 2.0), ob = run(*b, 2.0);
    CHECK(std::memcmp(oa.data(), ob.data(), oa.size() * sizeof(float)) == 0);
    for (Engine* e : {a.get(), b.get()}) e->send(cmd(Op::TempoRange, 0, 0.06f));
    b->send(cmd(Op::KeyLock, 0, 0, 1));
    run(*a, 1.0);
    run(*b, 1.0);
    CHECK(b->deck(0).key_lock_active);
    b->send(cmd(Op::KeyLock, 0, 0, 0));
    run(*a, 1.0);
    run(*b, 1.0);
    const auto pa = run(*a, 1.0), pb = run(*b, 1.0);
    CHECK(std::memcmp(pa.data(), pb.data(), pa.size() * sizeof(float)) == 0);
  }

  // ---- continuity: toggling key lock, effects and loops makes no clicks -------------------------------
  {
    auto e = engine();
    e->load(0, sine(440, 60));
    e->send(cmd(Op::Tempo, 0, 1.0f));
    e->send(cmd(Op::PlayToggle, 0));
    std::vector<float> all = run(*e, 0.5);
    auto add = [&](double s) {
      auto o = run(*e, s);
      if (std::getenv("DH_DEBUG")) { std::size_t at = 0; float m = 0; for (std::size_t q = 1; q < o.size() / kCh; ++q) { const float dd = std::fabs(o[q * kCh] - o[(q - 1) * kCh]); if (dd > m) { m = dd; at = q; } } std::printf("  segment %.1fs max delta %.4f at frame %zu\n", s, m, at); }
      all.insert(all.end(), o.begin(), o.end());
    };
    e->send(cmd(Op::KeyLock, 0, 0, 1));
    add(1.0);
    e->send(cmd(Op::KeyLock, 0, 0, 0));
    add(1.0);
    e->send(cmd(Op::KeyLock, 0, 0, 1));
    add(0.5);
    e->send(cmd(Op::BeatLoop, 0, 1.0f));  // loop wrap inside key lock
    add(2.5);
    e->send(cmd(Op::LoopExit, 0));
    e->send(cmd(Op::BeatJump, 0, 4.0f));
    add(1.0);
    e->send(cmd(Op::KeyLock, 0, 0, 0));
    e->send(cmd(Op::BeatLoop, 0, 0.5f));  // loop wrap in varispeed
    add(2.0);
    e->send(cmd(Op::LoopExit, 0));
    for (int k = 1; k < static_cast<int>(EffectKind::Count); ++k) {
      const EffectInfo* info = effect_info(static_cast<EffectKind>(k));
      if (info->colour || info->release) continue;
      e->send(cmd(Op::FxSelect, 0, 0, 0, k));
      e->send(cmd(Op::FxEnable, 0, 1, 0));
      if (std::getenv("DH_DEBUG")) std::printf("  %s\n", info->name);
      add(0.4);
      e->send(cmd(Op::FxEnable, 0, 0, 0));
      add(0.3);
    }
    CHECK(finite(all));
    const float md = max_delta(all);
    std::printf("max sample delta over toggles/loops/effects: %.4f (a clean 466 Hz tone peaks near 0.033)\n", md);
    CHECK(md < 0.2f);  // the crusher legitimately steps up to ~0.16; a click would approach 0.5
  }

  // ---- quantise, slip, reverse, loop move ------------------------------------------------------------------
  {
    auto e = engine();
    e->load(0, sine(440, 60, 120, 4410));  // first beat at frame 4410
    e->send(cmd(Op::Quantize, 0, 0, 1));
    e->send(cmd(Op::PlayToggle, 0));
    run(*e, 1.1);
    e->send(cmd(Op::HotCueDown, 0, 0, 0));
    e->send(cmd(Op::HotCueUp, 0, 0, 0));
    run(*e, 0.1);
    const double cue = e->deck(0).hot_cue[0];
    CHECK(cue > 0);
    CHECK(std::fabs(std::remainder(cue - 4410, 22050.0)) < 1.0);
    run(*e, 0.8);
    e->send(cmd(Op::HotCueDown, 0, 0, 0));  // trigger while playing: lands on the beat
    run(*e, 0.002);
    CHECK(std::fabs(e->deck(0).position - cue) < 400);
    e->send(cmd(Op::HotCueUp, 0, 0, 0));
    // quantised loop in/out
    run(*e, 0.33);
    e->send(cmd(Op::LoopIn, 0));
    run(*e, 0.9);
    e->send(cmd(Op::LoopOut, 0));
    run(*e, 0.01);
    const DeckView v = e->deck(0);
    CHECK(v.loop_active);
    CHECK(std::fabs(std::remainder(v.loop_in - 4410, 22050.0)) < 1.0 && std::fabs(std::remainder(v.loop_out - 4410, 22050.0)) < 1.0);
    e->send(cmd(Op::LoopMove, 0, 1.0f));
    run(*e, 0.01);
    CHECK(std::fabs(e->deck(0).loop_in - (v.loop_in + 22050)) < 2.0);
    e->send(cmd(Op::LoopExit, 0));
    run(*e, 0.01);
    CHECK(!e->deck(0).loop_active);
  }
  {  // slip: the shadow playhead keeps running while a loop holds the audio
    auto e = engine();
    e->load(0, sine(440, 60));
    e->send(cmd(Op::Slip, 0, 0, 1));
    e->send(cmd(Op::PlayToggle, 0));
    run(*e, 1.0);
    const double p0 = e->deck(0).position;
    e->send(cmd(Op::BeatLoop, 0, 1.0f));
    run(*e, 3.0);
    CHECK(e->deck(0).slip_active && e->deck(0).position < p0 + 22050 * 2);
    e->send(cmd(Op::LoopExit, 0));
    run(*e, 0.01);
    const double expect = p0 + 3.01 * 44100;
    std::printf("slip: position after exit %.0f, shadow expected %.0f\n", e->deck(0).position, expect);
    CHECK(std::fabs(e->deck(0).position - expect) < 1500);
    CHECK(!e->deck(0).slip_active);
  }
  {  // reverse
    auto e = engine();
    e->load(0, sine(440, 60));
    e->send(cmd(Op::Seek, 0, 0, 0, 20.0 * 44100));
    e->send(cmd(Op::PlayToggle, 0));
    run(*e, 0.5);
    const double p0 = e->deck(0).position;
    e->send(cmd(Op::Reverse, 0, 0, 1));
    run(*e, 1.0);
    CHECK(e->deck(0).reverse && e->deck(0).position < p0 - 20000);
    e->send(cmd(Op::Reverse, 0, 0, 0));
    const double p1 = e->deck(0).position;
    run(*e, 0.5);
    CHECK(e->deck(0).position > p1 + 10000);
  }
  {  // auto-gain
    auto e = engine();
    e->load(0, sine(440, 30));
    e->send(cmd(Op::PlayToggle, 0));
    auto rms = [](const std::vector<float>& b) {
      double s = 0;
      for (std::size_t i = 0; i < b.size() / kCh; ++i) s += b[i * kCh] * b[i * kCh];
      return std::sqrt(s / static_cast<double>(b.size() / kCh));
    };
    run(*e, 0.5);
    const double r0 = rms(run(*e, 0.5));
    e->send(cmd(Op::AutoGain, 0, -6.0f));
    run(*e, 0.3);
    const double r1 = rms(run(*e, 0.5));
    CHECK(std::fabs(r1 / r0 - 0.501) < 0.02);
  }

  // ---- effects --------------------------------------------------------------------------------------
  {  // echo time equals the beat fraction: a click repeats half a beat (0.25 s at 120 BPM) later
    auto e = engine();
    e->load(0, make_track(10, [](std::uint64_t i) { return i == 22050 ? 0.9f : 0.0f; }));
    e->send(cmd(Op::FxSelect, 0, 0, 0, static_cast<double>(EffectKind::Echo)));
    e->send(cmd(Op::FxTime, 0, 0.5f, 0));
    e->send(cmd(Op::FxWet, 0, 1.0f, 0));
    e->send(cmd(Op::FxEnable, 0, 1.0f, 0));
    e->send(cmd(Op::PlayToggle, 0));
    const auto out = run(*e, 2.0);
    auto argmax = [&](std::size_t a, std::size_t b) {
      std::size_t best = a;
      for (std::size_t i = a; i < b; ++i)
        if (std::fabs(out[i * kCh]) > std::fabs(out[best * kCh])) best = i;
      return best;
    };
    const std::size_t p1 = argmax(21000, 24000), p2 = argmax(p1 + 10000, p1 + 12500), p3 = argmax(p2 + 10000, p2 + 12500);
    std::printf("echo: repeats %zu and %zu frames after the click (expect 11025)\n", p2 - p1, p3 - p2);
    CHECK(std::fabs(out[p1 * kCh]) > 0.3f && std::fabs(out[p2 * kCh]) > 0.1f);
    CHECK(std::llabs(static_cast<long long>(p2 - p1) - 11025) <= 3 && std::llabs(static_cast<long long>(p3 - p2) - 11025) <= 3);
  }
  {  // every effect: finite, bounded, and silent once the input is silent
    auto burst = [](std::uint64_t i) {
      if (i > 44100) return 0.0f;
      return 0.4f * static_cast<float>(std::sin(2 * kPi * 440.0 * static_cast<double>(i) / 44100)) + 0.1f * static_cast<float>(((i * 2654435761u) >> 8 & 0xFFFF) / 32768.0 - 1.0);
    };
    for (int pass = 0; pass < 3; ++pass) {  // channel slot, master bus, colour slot
      for (int k = 1; k < static_cast<int>(EffectKind::Count); ++k) {
        const EffectInfo* info = effect_info(static_cast<EffectKind>(k));
        if ((pass == 2) != info->colour) continue;
        auto e = engine();
        e->load(0, make_track(24, burst));
        const int target = pass == 1 ? kFxMaster : 0;
        int colour_index = 0;
        if (pass == 2) {
          colour_index = k - static_cast<int>(EffectKind::ColorNoise) + 1;
          e->send(cmd(Op::ColorFx, 0, 0, colour_index));
          e->send(cmd(Op::Filter, 0, 0.9f));
        } else {
          e->send(cmd(Op::FxSelect, target, 0, 0, k));
          e->send(cmd(Op::FxEnable, target, 1.0f, 0));
        }
        e->send(cmd(Op::PlayToggle, 0));
        std::vector<float> all = run(*e, 1.2);
        if (pass == 2) e->send(cmd(Op::Filter, 0, 0.5f));
        else e->send(cmd(Op::FxEnable, target, 0.0f, 0));
        auto rest = run(*e, 22.0);
        all.insert(all.end(), rest.begin(), rest.end());
        CHECK(finite(all));
        CHECK(peak_of(all, 0, all.size() / kCh) <= 1.0f);
        const float tail = peak_of(all, 22 * 44100, all.size() / kCh);
        std::printf("fx %-14s pass %d: tail peak %.2e\n", info->name, pass, tail);
        CHECK(tail < 1e-4f);
      }
    }
  }

  // ---- sampler ---------------------------------------------------------------------------------------
  {
    auto e = engine();
    auto s = std::make_unique<LoadedSample>();
    auto audio = std::make_shared<TrackAudio>();
    audio->sample_rate = 44100;
    audio->frames = 22050;
    audio->pcm.resize(audio->frames * 2);
    for (std::uint64_t i = 0; i < audio->frames; ++i)
      audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = 0.5f * static_cast<float>(std::sin(2 * kPi * 1000.0 * static_cast<double>(i) / 44100));
    s->audio = audio;
    e->sampler_load(3, std::move(s));
    run(*e, 0.05);
    CHECK(e->sampler(3).loaded && !e->sampler(3).playing);
    e->send(cmd(Op::SamplerTrigger, 3));
    const auto out = run(*e, 1.0);
    double sum = 0;
    for (std::size_t i = 4410; i < 17640; ++i) sum += out[i * kCh] * out[i * kCh];
    const double r = std::sqrt(sum / 13230);
    CHECK(r > 0.30 && r < 0.40);
    CHECK(peak_of(out, 30000, 44100) < 1e-4f && !e->sampler(3).playing);
    e->send(cmd(Op::SamplerLoop, 3, 0, 1));
    e->send(cmd(Op::SamplerTrigger, 3));
    run(*e, 2.0);
    CHECK(e->sampler(3).playing);
    e->send(cmd(Op::SamplerStop, 3, 0, -1));
    const auto after = run(*e, 0.5);
    CHECK(peak_of(after, 10000, 22050) < 1e-4f);
  }

  // ---- recording and the stream tap -----------------------------------------------------------------
  {
    auto e = engine();
    e->load(0, sine(330, 30));
    e->send(cmd(Op::PlayToggle, 0));
    run(*e, 0.2);
    const std::string path = (std::filesystem::temp_directory_path() / "deckhand_test_rec.wav").string();
    std::string err;
    CHECK(e->record_open(path, false, err));
    e->stream_enable(true);
    const auto out = run(*e, 2.0);
    e->record_stop();
    run(*e, 0.05);
    e->record_close();
    const RecorderStatus st = e->recorder_status();
    CHECK(st.finished && !st.error && st.dropped_frames == 0);
    CHECK(std::fabs(st.seconds - 2.0) < 0.01);
    std::vector<float> st_buf(44100 * 2 * 3);
    std::size_t got = 0;
    while (std::size_t n = e->stream_read(st_buf.data() + got * 2, 4096)) got += n;
    CHECK(got >= 88200 && e->stream_dropped_frames() == 0);
    for (std::size_t i = 0; i < 88200; ++i) CHECK(st_buf[i * 2] == out[i * kCh] && st_buf[i * 2 + 1] == out[i * kCh + 1]);

    std::FILE* f = std::fopen(path.c_str(), "rb");
    CHECK(f);
    std::vector<unsigned char> b(static_cast<std::size_t>(std::filesystem::file_size(path)));
    CHECK(std::fread(b.data(), 1, b.size(), f) == b.size());
    std::fclose(f);
    CHECK(std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(b.data() + 8, "WAVE", 4) == 0);
    CHECK(std::memcmp(b.data() + 48, "fmt ", 4) == 0 && std::memcmp(b.data() + 72, "data", 4) == 0);
    CHECK(b[58] == 2 && b[70] == 24);
    const std::uint32_t dsz = b[76] | (b[77] << 8) | (b[78] << 16) | (static_cast<std::uint32_t>(b[79]) << 24);
    CHECK(dsz == 88200u * 6 && b.size() == 80 + dsz);
    double maxerr = 0;
    for (std::size_t i = 0; i < 88200; ++i)
      for (int c = 0; c < 2; ++c) {
        const unsigned char* p = b.data() + 80 + (i * 2 + static_cast<std::size_t>(c)) * 3;
        std::int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
        if (v & 0x800000) v -= 0x1000000;
        maxerr = std::max(maxerr, std::fabs(v / 8388607.0 - out[i * kCh + static_cast<std::size_t>(c)]));
      }
    std::printf("recorder: %u data bytes, max error vs rendered master %.2e\n", dsz, maxerr);
    CHECK(maxerr < 1e-6);
    std::filesystem::remove(path);
  }

  // ---- benchmark: 4 decks key-locked, 3 effects each ---------------------------------------------------
  {
    auto e = engine();
    for (int d = 0; d < 4; ++d) {
      e->load(d, sine(220.0 * (d + 1), 120));
      e->send(cmd(Op::Tempo, d, 1.0f));
      e->send(cmd(Op::KeyLock, d, 0, 1));
      const EffectKind kinds[3] = {EffectKind::Echo, EffectKind::Reverb, EffectKind::Phaser};
      for (int s = 0; s < 3; ++s) {
        e->send(cmd(Op::FxSelect, d, 0, s, static_cast<double>(kinds[s])));
        e->send(cmd(Op::FxEnable, d, 1.0f, s));
      }
      e->send(cmd(Op::PlayToggle, d));
    }
    run(*e, 0.5);
    std::vector<float> block(512 * kCh);
    double worst = 0, total = 0;
    std::vector<double> times;
    const int blocks = 2000;
    for (int i = 0; i < blocks; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      e->render(block.data(), 512);
      const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      worst = std::max(worst, us);
      total += us;
      times.push_back(us);
    }
    std::sort(times.begin(), times.end());
    std::printf("benchmark: 4 decks key-locked + 3 effects each, 512 frames: worst %.0f us, p99 %.0f us, mean %.0f us\n", worst,
                times[times.size() * 99 / 100], total / blocks);
  }
  std::puts("engine fx ok");
  return 0;
}
