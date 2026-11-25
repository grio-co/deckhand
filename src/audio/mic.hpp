#pragma once
// Microphone channel internals (not installed): the clock-drift buffer between the capture device and the output
// device, and the channel's signal path (high-pass, 2-band EQ, gate, level, talkover). Everything that runs on an
// audio thread here takes no locks, allocates nothing and makes no system calls.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "deckhand/audio/engine.hpp"
#include "dsp.hpp"

namespace deckhand {

// ---- capture clock to output clock ---------------------------------------------------------------------------
// Two devices never run at exactly the same rate (different crystals, different nominal rates), so a fixed-ratio
// resampler would slowly fill or empty the buffer between them. The capture thread pushes frames into a wait-free
// ring; the output thread pulls them through a Hermite resampler whose ratio is steered by a PI loop on the ring's
// smoothed fill level, so the fill settles at `target` frames and latency stays bounded.
class MicClock {
 public:
  // Non-real-time; neither side may be running. Allocates the ring.
  void init(std::uint32_t in_rate, std::uint32_t out_rate, std::size_t target_frames) {
    in_rate_ = std::max<std::uint32_t>(in_rate, 8000);
    out_rate_ = std::max<std::uint32_t>(out_rate, 8000);
    target_ = std::max<std::size_t>(target_frames, 64);
    std::size_t cap = 1;
    while (cap < std::max<std::size_t>(target_ * 8, in_rate_ / 2)) cap <<= 1;
    ring_.init(cap);
    underruns_.store(0);
    overruns_.store(0);
    reset_consumer();
  }
  // The output device was reopened at another rate (the output thread is not running).
  void set_out_rate(std::uint32_t out_rate) {
    out_rate_ = std::max<std::uint32_t>(out_rate, 8000);
    reset_consumer();
  }
  // Output side only (output thread stopped, or the first call): forget the buffered audio and re-prime.
  void reset_consumer() {
    float tmp[256 * 2];
    for (std::size_t k = 0, max = ring_.capacity() / 256 + 2; k < max && ring_.pop(tmp, 256) > 0; ++k) {
    }
    primed_ = false;
    stage_pos_ = stage_len_ = 0;
    fill_s_ = static_cast<double>(target_);
    integ_ = 0;
    ratio_ = base_ratio();
    last_[0] = last_[1] = 0;
  }
  bool ready() const { return ring_.capacity() != 0; }

  // Capture thread. Frames that do not fit are dropped (counted).
  void push(const float* stereo, std::size_t frames) {
    const std::size_t n = ring_.push(stereo, frames);
    if (n < frames) overruns_.fetch_add(1, std::memory_order_relaxed);
  }

  // Output thread: exactly n stereo frames in the output clock; silence (with a short fade) while the buffer fills.
  void pull(float* out, int n) {
    if (!ring_.capacity()) {
      std::fill(out, out + 2 * n, 0.0f);
      return;
    }
    std::size_t avail = ring_.size() + (stage_len_ - stage_pos_);
    if (avail > target_ * 4) {  // the capture side ran ahead (a stalled output, a burst): fall back to the target
      float junk[256 * 2];
      while (avail > target_) {
        const std::size_t drop = std::min<std::size_t>(avail - target_, 256);
        const std::size_t got = ring_.pop(junk, drop);
        if (got == 0) break;
        avail -= got;
      }
      overruns_.fetch_add(1, std::memory_order_relaxed);
      primed_ = false;  // the interpolator history is stale
      stage_pos_ = stage_len_ = 0;
    }
    if (!primed_) {
      if (avail < target_) {
        fade_out(out, 0, n);
        return;
      }
      float first[4 * 2];
      if (ring_.pop(first, 4) < 4) {
        fade_out(out, 0, n);
        return;
      }
      for (int k = 0; k < 4; ++k) {
        hist_[k][0] = first[2 * k];
        hist_[k][1] = first[2 * k + 1];
      }
      frac_ = 0;
      fill_s_ = static_cast<double>(target_);
      primed_ = true;
      fade_in_ = 0;  // ramp the signal in: the first sample after a gap is rarely zero
      avail -= 4;
    }

    // Control: smoothed fill against the target steers the consumption ratio (kp: 0.67 / s, integral time 4 s).
    const double dt = static_cast<double>(n) / out_rate_;
    fill_s_ += std::min(1.0, dt / 0.4) * (static_cast<double>(avail) - fill_s_);
    const double err = fill_s_ - static_cast<double>(target_);
    const double kp = 0.67 / in_rate_;
    integ_ = std::clamp(integ_ + kp * err * dt / 4.0, -0.02, 0.02);
    ratio_ = base_ratio() * (1.0 + std::clamp(kp * err + integ_, -0.025, 0.025));

    for (int i = 0; i < n; ++i) {
      while (frac_ >= 1.0) {
        float f[2];
        if (!next_frame(f)) {  // ran dry: count it, fade out and re-prime
          underruns_.fetch_add(1, std::memory_order_relaxed);
          primed_ = false;
          fade_out(out, i, n);
          return;
        }
        for (int c = 0; c < 2; ++c) {
          hist_[0][c] = hist_[1][c];
          hist_[1][c] = hist_[2][c];
          hist_[2][c] = hist_[3][c];
          hist_[3][c] = f[c];
        }
        frac_ -= 1.0;
      }
      const float t = static_cast<float>(frac_);
      for (int c = 0; c < 2; ++c) {
        const float xm1 = hist_[0][c], x0 = hist_[1][c], x1 = hist_[2][c], x2 = hist_[3][c];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        float y = ((c3 * t + c2) * t + c1) * t + x0;
        if (fade_in_ < kFadeIn) y *= static_cast<float>(fade_in_) / static_cast<float>(kFadeIn);
        last_[c] = out[2 * i + c] = y;
      }
      if (fade_in_ < kFadeIn) ++fade_in_;
      frac_ += ratio_;
    }
  }

  std::size_t fill_frames() const { return ring_.size(); }
  std::size_t target_frames() const { return target_; }
  std::uint32_t in_rate() const { return in_rate_; }
  std::uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
  std::uint64_t overruns() const { return overruns_.load(std::memory_order_relaxed); }
  double ratio() const { return ratio_; }  // output thread / tests

 private:
  double base_ratio() const { return static_cast<double>(in_rate_) / static_cast<double>(out_rate_); }
  bool next_frame(float* f) {
    if (stage_pos_ == stage_len_) {
      stage_len_ = ring_.pop(stage_, kStage);
      stage_pos_ = 0;
      if (stage_len_ == 0) return false;
    }
    f[0] = stage_[2 * stage_pos_];
    f[1] = stage_[2 * stage_pos_ + 1];
    ++stage_pos_;
    return true;
  }
  void fade_out(float* out, int from, int n) {
    for (int i = from; i < n; ++i) {
      out[2 * i] = last_[0];
      out[2 * i + 1] = last_[1];
      last_[0] *= 0.99f;
      last_[1] *= 0.99f;
    }
    if (std::fabs(last_[0]) < 1e-7f) last_[0] = 0;
    if (std::fabs(last_[1]) < 1e-7f) last_[1] = 0;
  }

  static constexpr std::size_t kStage = 64;
  static constexpr int kFadeIn = 256;
  FrameRing ring_;
  std::uint32_t in_rate_ = 48000, out_rate_ = 48000;
  std::size_t target_ = 1024;
  std::atomic<std::uint64_t> underruns_{0}, overruns_{0};
  // output-thread state
  bool primed_ = false;
  float hist_[4][2] = {};
  float stage_[kStage * 2] = {};
  std::size_t stage_pos_ = 0, stage_len_ = 0;
  double frac_ = 0, fill_s_ = 0, integ_ = 0, ratio_ = 1;
  int fade_in_ = kFadeIn;
  float last_[2] = {0, 0};
};

// ---- the channel -----------------------------------------------------------------------------------------------
class MicChannel {
 public:
  // Parameters (audio thread, from commands).
  bool on = false, monitor = false;
  int talkover = 0;  // 0 off, 1 voice-activated, 2 always
  float level = 0.8f, eq_low = 0.5f, eq_high = 0.5f, depth_db = -12.0f, gate_db = -80.0f;

  void prepare(std::uint32_t rate) {
    sr_ = static_cast<double>(rate);
    hp_ = Bq();
    low_ = Bq();
    high_ = Bq();
    high_pass(hp_, kMicHighPassHz, 0.7071);
    eq_update();
    gate_env_ = gate_g_ = act_env_ = 0;
    duck_g_ = 1;
    on_g_ = on ? 1.0f : 0.0f;
    lvl_prev_ = level_gain(level);
    peak_ = 0;
    active_ = false;
  }
  void eq_update() {
    const double lo = (static_cast<double>(eq_low) - 0.5) * 24.0, hi = (static_cast<double>(eq_high) - 0.5) * 24.0;
    shelf(low_, 200.0, lo, false);
    shelf(high_, 4000.0, hi, true);
  }

  // in: n stereo frames (nullptr = no input). out: processed microphone, n stereo frames. duck: per-frame gain for the
  // music, 1 = untouched.
  void process(const float* in, float* out, float* duck, int n) {
    const float lvl1 = level_gain(level), lvl0 = lvl_prev_;
    lvl_prev_ = lvl1;
    const float inv_n = 1.0f / static_cast<float>(n);
    const float on_t = on ? 1.0f : 0.0f;
    const float on_c = static_cast<float>(1.0 - std::exp(-1.0 / (0.004 * sr_)));
    const float thr_act = std::pow(10.0f, kMicTalkoverThresholdDb / 20.0f);
    const float thr_gate = gate_db > -79.5f ? std::pow(10.0f, gate_db / 20.0f) : 0.0f;
    const float duck_t_on = std::pow(10.0f, std::clamp(depth_db, -24.0f, 0.0f) / 20.0f);
    const float c_att = static_cast<float>(1.0 - std::exp(-1.0 / (kMicTalkoverAttack * sr_)));
    const float c_rel = static_cast<float>(1.0 - std::exp(-1.0 / (kMicTalkoverRelease * sr_)));
    const float gate_env_dec = static_cast<float>(std::exp(-1.0 / (0.03 * sr_)));
    const float gate_open_c = static_cast<float>(1.0 - std::exp(-1.0 / (0.002 * sr_)));
    const float gate_close_c = static_cast<float>(1.0 - std::exp(-1.0 / (0.08 * sr_)));
    const int hold_n = static_cast<int>(0.15 * sr_);
    const float act_dec = static_cast<float>(std::exp(-1.0 / (0.1 * sr_)));
    float pk = 0;
    for (int i = 0; i < n; ++i) {
      float l = in ? in[2 * i] : 0.0f, r = in ? in[2 * i + 1] : 0.0f;
      l = hp_.run(0, l);
      r = hp_.run(1, r);
      l = high_.run(0, low_.run(0, l));
      r = high_.run(1, low_.run(1, r));
      float g = 1.0f;
      if (thr_gate > 0) {
        const float a = std::max(std::fabs(l), std::fabs(r));
        gate_env_ = std::max(a, gate_env_ * gate_env_dec);
        if (gate_env_ > thr_gate) {
          gate_open_ = true;
          gate_hold_ = hold_n;
        } else if (gate_hold_ > 0) {
          --gate_hold_;
        } else if (gate_env_ < thr_gate * 0.7f) {
          gate_open_ = false;
        }
        const float tgt = gate_open_ ? 1.0f : 0.0f;
        gate_g_ += (tgt - gate_g_) * (tgt > gate_g_ ? gate_open_c : gate_close_c);
        g = gate_g_;
      } else {
        gate_g_ = 1.0f;
        gate_open_ = true;
      }
      on_g_ += (on_t - on_g_) * on_c;
      g *= on_g_ * (lvl0 + (lvl1 - lvl0) * static_cast<float>(i) * inv_n);
      l *= g;
      r *= g;
      out[2 * i] = l;
      out[2 * i + 1] = r;
      const float m = std::max(std::fabs(l), std::fabs(r));
      pk = std::max(pk, m);
      act_env_ = std::max(m, act_env_ * act_dec);
      if (act_env_ > thr_act) active_ = true;
      else if (act_env_ < thr_act * 0.7f) active_ = false;
      const bool duck_now = talkover == 2 || (talkover == 1 && active_);
      const float tgt = duck_now ? duck_t_on : 1.0f;
      duck_g_ += (tgt - duck_g_) * (tgt < duck_g_ ? c_att : c_rel);
      duck[i] = duck_g_;
    }
    const float decay = static_cast<float>(std::exp(-static_cast<double>(n) / (0.3 * sr_)));
    peak_ = std::max(pk, peak_ * decay);
    if (!std::isfinite(duck_g_)) duck_g_ = 1;
  }

  float peak() const { return peak_; }
  float duck_db() const { return 20.0f * std::log10(std::max(duck_g_, 1e-4f)); }
  bool ducking() const { return duck_g_ < 0.98f; }
  bool voice_active() const { return active_; }

 private:
  struct Bq {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] = {0, 0}, z2[2] = {0, 0};
    inline float run(int ch, float x) {
      const float y = b0 * x + z1[ch];
      z1[ch] = b1 * x - a1 * y + z2[ch];
      z2[ch] = b2 * x - a2 * y;
      return y;
    }
    void set(double B0, double B1, double B2, double A0, double A1, double A2) {
      b0 = static_cast<float>(B0 / A0);
      b1 = static_cast<float>(B1 / A0);
      b2 = static_cast<float>(B2 / A0);
      a1 = static_cast<float>(A1 / A0);
      a2 = static_cast<float>(A2 / A0);
    }
  };
  static float level_gain(float k) { return k * k * 1.5625f; }  // unity at 0.8, like the other level knobs
  void high_pass(Bq& q, double f0, double Q) const {
    const double w = 2 * 3.14159265358979323846 * f0 / sr_, c = std::cos(w), al = std::sin(w) / (2 * Q);
    q.set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
  }
  void shelf(Bq& q, double f0, double db, bool high) const {
    const double A = std::pow(10.0, db / 40.0), w = 2 * 3.14159265358979323846 * f0 / sr_, c = std::cos(w);
    const double al = std::sin(w) / 2 * std::sqrt(2.0), s = 2 * std::sqrt(A) * al;
    if (high)
      q.set(A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
            (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
    else
      q.set(A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
            (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
  }

  double sr_ = 48000;
  Bq hp_, low_, high_;
  float gate_env_ = 0, gate_g_ = 0, act_env_ = 0, duck_g_ = 1, on_g_ = 0, lvl_prev_ = 1, peak_ = 0;
  int gate_hold_ = 0;
  bool gate_open_ = false, active_ = false;
};

}  // namespace deckhand
