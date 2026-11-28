// Mixer laws (channel fader, crossfader, EQ kill depth and crossovers, trim range) and sample-accurate knob ramps,
// against the real Engine rendered offline. The defaults must reproduce the original behaviour exactly.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/ipc.hpp"
#include "deckhand/audio/mixer_laws.hpp"

using namespace deckhand;
namespace mx = deckhand::mixer;

namespace {

const double kPi = 3.14159265358979323846;
constexpr std::uint32_t kSr = 44100;

std::unique_ptr<LoadedTrack> sine_track(double hz, double seconds, float amp) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = kSr;
  audio->frames = static_cast<std::uint64_t>(seconds * kSr);
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i) {
    const float v = amp * static_cast<float>(std::sin(2 * kPi * hz * static_cast<double>(i) / kSr));
    audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = v;
  }
  build_waveform(*audio);
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
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
Command setting(mx::MixerField f, float v) { return cmd(Op::MixerSetting, 0, v, static_cast<int>(f)); }

// Renders `frames` in 256-frame callbacks; returns the master left channel.
std::vector<float> run(Engine& e, std::uint32_t frames) {
  std::vector<float> out, blk(256 * 2);
  for (std::uint32_t done = 0; done < frames;) {
    const std::uint32_t n = std::min<std::uint32_t>(256, frames - done);
    e.render(blk.data(), n);
    for (std::uint32_t k = 0; k < n; ++k) out.push_back(blk[k * 2]);
    done += n;
  }
  return out;
}
double rms(const std::vector<float>& v, std::size_t from, std::size_t to) {
  double s = 0;
  for (std::size_t i = from; i < to; ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, to - from)));
}
double db(double x) { return 20 * std::log10(std::max(x, 1e-12)); }
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

struct Rig {
  Engine e;
  explicit Rig(double hz, float amp = 0.5f, int decks = 1) {
    e.configure_offline(kSr, 2);
    for (int d = 0; d < decks; ++d) {
      e.load(d, sine_track(hz, 60, amp));
      e.send(cmd(Op::PlayToggle, d));
    }
    run(e, kSr / 4);
  }
  // steady-state level after the smoothing has settled
  double level() {
    run(e, kSr / 4);
    const auto v = run(e, kSr / 4);
    return rms(v, 0, v.size());
  }
};

}  // namespace

int main() {
  // ---- defaults reproduce the original laws exactly -------------------------------------------------
  {
    const mx::Settings s;
    bool same = true;
    for (int i = 0; i <= 1000; ++i) {
      const float k = static_cast<float>(i) / 1000.0f;
      same = same && mx::fader_gain(s, k) == k * k;
      same = same && mx::crossfader_gain(s, k) == std::min(1.0f, 2.0f * k);
      const double eq_old = k < 0.5f ? -26.0 * (1.0 - 2.0 * k) : 6.0 * (2.0 * k - 1.0);
      same = same && mx::eq_db(s, k) == eq_old;
      const float trim_old = k < 0.5f ? (2 * k) * (2 * k) : static_cast<float>(std::pow(10.0, (2.0 * k - 1.0) * 9.0 / 20.0));
      same = same && mx::trim_gain(s, k) == trim_old;
    }
    CHECK(same);
  }

  // ---- curves ---------------------------------------------------------------------------------------
  {
    mx::Settings s;
    mx::set_field(s, mx::MixerField::FaderLaw, 1);
    CHECK(mx::fader_gain(s, 0.3f) == 0.3f);
    mx::set_field(s, mx::MixerField::FaderLaw, 2);
    CHECK(mx::fader_gain(s, 0) == 0 && mx::fader_gain(s, 1) == 1);
    CHECK(near(db(mx::fader_gain(s, 0.5f)), -30.0, 0.5));  // half the travel of a 60 dB range
    mx::set_field(s, mx::MixerField::FaderRangeDb, 40);
    CHECK(near(db(mx::fader_gain(s, 0.5f)), -20.0, 1.0) && near(db(mx::fader_gain(s, 0.75f)), -10.0, 0.3));
    float prev = -1;
    bool mono = true, inv = true;
    for (int law = 0; law < 3; ++law) {
      s.fader_law = law;
      prev = -1;
      for (int i = 0; i <= 200; ++i) {
        const float k = static_cast<float>(i) / 200.0f, g = mx::fader_gain(s, k);
        mono = mono && g >= prev;
        prev = g;
        inv = inv && std::fabs(mx::fader_for_gain(s, g) - k) < 2e-3f;
      }
    }
    CHECK(mono && inv);
    // out-of-range values clamp, junk becomes a safe value
    CHECK(mx::clamp_setting(mx::MixerField::EqKillDb, 1000) == 120 && mx::clamp_setting(mx::MixerField::EqKillDb, std::nanf("")) == 6);
    CHECK(mx::clamp_setting(mx::MixerField::FaderLaw, 9) == 2 && mx::clamp_setting(mx::MixerField::TrimMaxDb, -3) == 0);

    mx::Settings c;
    mx::set_field(c, mx::MixerField::CrossfaderLaw, 1);
    bool power = true;
    for (int i = 0; i <= 100; ++i) {
      const float x = static_cast<float>(i) / 100.0f, a = mx::crossfader_gain(c, 1.0f - x), b = mx::crossfader_gain(c, x);
      power = power && near(a * a + b * b, 1.0, 1e-5);
    }
    CHECK(power);
    mx::set_field(c, mx::MixerField::CrossfaderLaw, 2);
    CHECK(near(mx::crossfader_gain(c, 0.25f), 0.25, 1e-6));
    mx::set_field(c, mx::MixerField::CrossfaderLaw, 3);
    mx::set_field(c, mx::MixerField::CrossfaderCutIn, 0.1f);
    CHECK(mx::crossfader_gain(c, 0) == 0 && near(mx::crossfader_gain(c, 0.05f), 0.5, 1e-6) && mx::crossfader_gain(c, 0.1f) == 1 &&
          mx::crossfader_gain(c, 0.4f) == 1);
    mx::set_field(c, mx::MixerField::CrossfaderCutIn, 0.5f);  // a cut-in of half the travel is the engine's own curve
    CHECK(mx::crossfader_gain(c, 0.3f) == mx::crossfader_gain(mx::Settings{}, 0.3f));

    mx::Settings t;
    mx::set_field(t, mx::MixerField::TrimMaxDb, 12);
    mx::set_field(t, mx::MixerField::TrimCutDb, 12);
    CHECK(near(db(mx::trim_gain(t, 1.0f)), 12, 1e-3) && near(db(mx::trim_gain(t, 0.0f)), -12, 1e-3) && near(db(mx::trim_gain(t, 0.5f)), 0, 1e-6));
    CHECK(mx::eq_db(mx::Settings{}, 0) == -26.0);
    mx::Settings q;
    mx::set_field(q, mx::MixerField::EqKillDb, 120);
    mx::set_field(q, mx::MixerField::EqBoostDb, 12);
    CHECK(mx::eq_db(q, 0) == -120.0 && mx::eq_db(q, 1) == 12.0 && mx::eq_db(q, 0.5f) == 0.0);
  }

  // ---- engine: channel fader laws ---------------------------------------------------------------------
  {
    mx::Settings law2, law2r40;
    mx::set_field(law2, mx::MixerField::FaderLaw, 2);
    law2r40 = law2;
    mx::set_field(law2r40, mx::MixerField::FaderRangeDb, 40);
    Rig r(440);
    const double full = r.level();
    CHECK(full > 0.3);
    r.e.send(cmd(Op::Fader, 0, 0.5f));
    CHECK(near(r.level() / full, 0.25, 0.01));  // default: audio taper
    r.e.send(setting(mx::MixerField::FaderLaw, 1));
    CHECK(near(r.level() / full, 0.5, 0.01));
    r.e.send(setting(mx::MixerField::FaderLaw, 2));
    CHECK(near(r.level() / full, mx::fader_gain(law2, 0.5f), 0.004));
    r.e.send(setting(mx::MixerField::FaderRangeDb, 40));
    CHECK(near(r.level() / full, mx::fader_gain(law2r40, 0.5f), 0.006));
  }

  // ---- engine: crossfader laws (deck 0 is the left side, deck 1 the right) ---------------------------------------
  {
    Rig r(440, 0.25f, 2);
    r.e.send(cmd(Op::Crossfader, 0, 0.9f));  // far right: deck 1 open, deck 0 at u = 0.1
    const double both_default = r.level();
    r.e.send(cmd(Op::Crossfader, 0, 0.0f));
    const double left_only = r.level();
    CHECK(left_only > 0.1);
    r.e.send(cmd(Op::Crossfader, 0, 0.9f));
    CHECK(near(both_default / (2 * left_only), 0.6, 0.02));  // two in-phase decks: 1 + 0.2
    r.e.send(setting(mx::MixerField::CrossfaderLaw, 3));
    r.e.send(setting(mx::MixerField::CrossfaderCutIn, 0.05f));
    CHECK(near(r.level() / (2 * left_only), 1.0, 0.02));  // both fully open already at 10 % of the travel
    r.e.send(setting(mx::MixerField::CrossfaderLaw, 2));
    CHECK(near(r.level() / (2 * left_only), 0.5, 0.02));  // 0.1 + 0.9
    r.e.send(setting(mx::MixerField::CrossfaderLaw, 1));
    CHECK(near(r.level() / (2 * left_only), (std::sin(0.1 * kPi / 2) + std::sin(0.9 * kPi / 2)) / 2, 0.02));
  }

  // ---- engine: a jump of the crossfader or fader never steps (bounded change per sample) ----------------------------
  {
    for (int what = 0; what < 3; ++what) {
      Rig r(100, 0.5f);
      r.e.send(setting(mx::MixerField::CrossfaderLaw, 3));
      r.e.send(setting(mx::MixerField::CrossfaderCutIn, 0.01f));
      if (what == 2) r.e.send(setting(mx::MixerField::FaderLaw, 1));
      run(r.e, 4000);
      r.e.send(what == 2 ? cmd(Op::Fader, 0, 0.0f) : cmd(Op::Crossfader, 0, 1.0f));
      const auto v = run(r.e, 4000);
      double worst = 0;
      for (std::size_t i = 1; i < v.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(v[i] - v[i - 1])));
      CHECK(worst < 0.5 * (2 * kPi * 100 / kSr) + 0.5 / 256.0 * 1.2);  // the sine's own slope plus amp / chunk
      CHECK(rms(v, v.size() - 500, v.size()) < 1e-3);                 // and it did go silent
    }
  }

  // ---- engine: EQ kill depth, boost and crossovers --------------------------------------------------------------
  {
    Rig r(60, 0.2f);
    const double flat = r.level();
    r.e.send(cmd(Op::EqLow, 0, 0.0f));
    const double k26 = db(r.level() / flat);
    CHECK(near(k26, -26, 3));
    r.e.send(setting(mx::MixerField::EqKillDb, 12));
    CHECK(near(db(r.level() / flat), -12, 2));
    r.e.send(setting(mx::MixerField::EqKillDb, 120));
    CHECK(db(r.level() / flat) < -50);
    r.e.send(cmd(Op::EqLow, 0, 1.0f));
    r.e.send(setting(mx::MixerField::EqBoostDb, 12));
    CHECK(near(db(r.level() / flat), 12, 2));
    r.e.send(setting(mx::MixerField::EqBoostDb, 6));
    CHECK(near(db(r.level() / flat), 6, 1.5));
  }
  {
    Rig r(300, 0.2f);  // between the two low-shelf corners
    const double flat = r.level();
    r.e.send(cmd(Op::EqLow, 0, 0.0f));
    const double a = db(r.level() / flat);
    r.e.send(setting(mx::MixerField::EqLowHz, 500));
    const double b = db(r.level() / flat);
    r.e.send(setting(mx::MixerField::EqLowHz, 80));
    const double c = db(r.level() / flat);
    CHECK(b < a - 3 && c > a + 3);  // a higher corner cuts a 300 Hz tone harder
  }

  // ---- engine: trim range -------------------------------------------------------------------------------------
  {
    Rig r(440, 0.02f);
    const double unity = r.level();
    r.e.send(cmd(Op::Trim, 0, 1.0f));
    CHECK(near(db(r.level() / unity), 9, 0.3));
    r.e.send(setting(mx::MixerField::TrimMaxDb, 24));
    CHECK(near(db(r.level() / unity), 24, 0.3));
    r.e.send(setting(mx::MixerField::TrimMaxDb, 0));
    CHECK(near(db(r.level() / unity), 0, 0.2));
    r.e.send(cmd(Op::Trim, 0, 0.0f));
    CHECK(r.level() / unity < 1e-3);  // default: silence at the bottom
    r.e.send(setting(mx::MixerField::TrimCutDb, 12));
    CHECK(near(db(r.level() / unity), -12, 0.3));
  }

  // ---- ramps: sample-accurate start, ideal curve, no steps ---------------------------------------------------------
  {
    Rig r(440, 0.5f);
    const double total = 44100;
    r.e.send(cmd(Op::MixRamp, 0, 0.0f, static_cast<int>(mx::RampField::Fader) | (1 << 8), total));
    const mx::Settings s;
    double worst = 0, worst_step = 0, prev_gain = 1;
    std::vector<float> all;
    for (int b = 1; b <= 175; ++b) {
      const auto v = run(r.e, 256);
      all.insert(all.end(), v.begin(), v.end());
      const double t = std::min(1.0, 256.0 * b / total), ideal = std::sqrt(std::cos(t * kPi / 2));
      const double got = r.e.deck(0).fader;
      worst = std::max(worst, std::fabs(got - ideal));
      const double g = mx::fader_gain(s, static_cast<float>(got));
      worst_step = std::max(worst_step, std::fabs(g - prev_gain));
      prev_gain = g;
    }
    CHECK(worst < 1e-5);
    CHECK(worst_step < kPi / 2 * 256 / total * 1.01 + 1e-4);
    CHECK(r.e.deck(0).fader == 0.0f);
    // the audible envelope follows the ideal equal-power gain
    double env_err = 0;
    for (int b = 8; b < 160; ++b) {
      const double mid = (256.0 * b + 128) / total, ideal = std::cos(mid * kPi / 2);
      env_err = std::max(env_err, std::fabs(rms(all, 256 * static_cast<std::size_t>(b), 256 * static_cast<std::size_t>(b + 1)) / (0.5 / std::sqrt(2.0)) - ideal));
    }
    CHECK(env_err < 0.02);
  }
  {  // a scheduled ramp begins on exactly the requested output frame
    Rig r(440, 0.5f);
    const std::uint64_t at = r.e.clock_frames() + 10000;
    CHECK(r.e.schedule(cmd(Op::MixRamp, 0, 0.0f, static_cast<int>(mx::RampField::Fader), 20000), Schedule::at_clock_frame(at)) != 0);
    run(r.e, 10000);
    CHECK(r.e.deck(0).fader == 1.0f);
    run(r.e, 250);  // 10 frames short of the end of the first 256-frame callback after the trigger
    CHECK(near(r.e.deck(0).fader, 1.0 - 250.0 / 20000.0, 1e-5));
  }
  {  // any direct command for the knob cancels its ramp; the ramp can also move EQ and trim
    Rig r(440, 0.5f);
    r.e.send(cmd(Op::MixRamp, 0, 0.0f, static_cast<int>(mx::RampField::Fader), 44100));
    run(r.e, 4096);
    CHECK(r.e.deck(0).fader < 0.95f && r.e.deck(0).fader > 0.8f);
    r.e.send(cmd(Op::Fader, 0, 0.7f));
    run(r.e, 4096);
    CHECK(r.e.deck(0).fader == 0.7f);
    r.e.send(cmd(Op::MixRamp, 0, 0.0f, static_cast<int>(mx::RampField::EqLow), 2560));
    run(r.e, 2560);
    CHECK(r.e.deck(0).eq_low == 0.0f);
    r.e.send(cmd(Op::MixRamp, 0, 1.0f, static_cast<int>(mx::RampField::EqLow), 0));  // no duration: set at once
    run(r.e, 256);
    CHECK(r.e.deck(0).eq_low == 1.0f);
  }

  // ---- the wire accepts the new ops and rejects nonsense --------------------------------------------------------------
  {
    const auto ok = [](Op op, int i, float f, double d = 0) {
      ipc::WireCommand w;
      w.op = static_cast<std::uint8_t>(op);
      w.i = i;
      w.f = f;
      w.d = d;
      Command out;
      return ipc::validate_command(w, out);
    };
    CHECK(ipc::kProtocolVersion == 12);
    CHECK(ok(Op::MixerSetting, 0, 2) && ok(Op::MixerSetting, static_cast<int>(mx::MixerField::Count) - 1, 12));
    CHECK(!ok(Op::MixerSetting, static_cast<int>(mx::MixerField::Count), 1) && !ok(Op::MixerSetting, -1, 1) && !ok(Op::MixerSetting, 0, std::nanf("")));
    CHECK(ok(Op::MixRamp, 0, 0.5f, 1000) && ok(Op::MixRamp, 1 | (1 << 8), 0.0f, 0));
    CHECK(!ok(Op::MixRamp, 99, 0.5f, 10) && !ok(Op::MixRamp, 0, 2.0f, 10) && !ok(Op::MixRamp, 2 << 8, 0.5f, 10) && !ok(Op::MixRamp, 0, 0.5f, std::nan("")));
  }
  return 0;
}
