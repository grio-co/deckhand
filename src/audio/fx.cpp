// Effect table and processors. Original implementations: delay lines, a Schroeder-style plate reverb,
// modulated delays, all-pass phaser, state-variable filter, two-tap delay-line pitch shifter, and so on.
#include "fx.hpp"
#include "fx_plugin.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace deckhand {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kTwoPi = 6.2831853071795864769f;

struct FxDef {
  EffectInfo info;
  void (*process)(FxUnit&, const FxCtx&, float*, int);
  void (*reset)(FxUnit&);
  bool always_run;  // keeps listening while switched off (backspin needs history)
};

struct K {
  float on, wet, par;
};
K coefs(const FxCtx& cx) {
  return {static_cast<float>(1.0 - std::exp(-1.0 / (0.008 * cx.sr))), static_cast<float>(1.0 - std::exp(-1.0 / (0.02 * cx.sr))),
          static_cast<float>(1.0 - std::exp(-1.0 / (0.03 * cx.sr)))};
}
inline void smooth(FxUnit& u, const K& k) {
  u.on_s += ((u.on ? 1.0f : 0.0f) - u.on_s) * k.on;
  u.wet_s += (u.wet - u.wet_s) * k.wet;
  u.param_s += (u.param - u.param_s) * k.par;
  u.param2_s += (u.param2 - u.param2_s) * k.par;
  u.param3_s += (u.param3 - u.param3_s) * k.par;
}
inline float fz(float x) { return std::fabs(x) < 1e-20f ? 0.0f : x; }
float lp_coef(double fc, double sr) { return static_cast<float>(1.0 - std::exp(-2.0 * kPi * fc / sr)); }
inline float noise(FxUnit& u) {
  u.rng ^= u.rng << 13;
  u.rng ^= u.rng >> 17;
  u.rng ^= u.rng << 5;
  return static_cast<float>(u.rng) * (2.0f / 4294967296.0f) - 1.0f;
}
inline void track_quiet(FxUnit& u, float peak, int n) {
  if (peak > 1e-5f) u.quiet = 0;
  else u.quiet += n;
}
inline void follow_env(FxUnit& u, float x, float rel) {  // fast attack, slow release
  const float a = std::fabs(x);
  u.env_in = a > u.env_in ? a : u.env_in * rel;
}

// ---- delay memory -------------------------------------------------------------------------------
inline void dl_read(const FxUnit& u, int ch, double delay, float& out) {
  const std::int64_t m = u.cap - 1;
  const double rp = static_cast<double>(u.w) - delay;
  const std::int64_t i = static_cast<std::int64_t>(std::floor(rp));
  const float t = static_cast<float>(rp - static_cast<double>(i));
  const float a = u.mem[static_cast<std::size_t>(i & m) * 2 + static_cast<std::size_t>(ch)];
  const float b = u.mem[static_cast<std::size_t>((i + 1) & m) * 2 + static_cast<std::size_t>(ch)];
  out = a + (b - a) * t;
}
inline void dl_write(FxUnit& u, float l, float r) {
  const std::size_t k = static_cast<std::size_t>(u.w & (u.cap - 1)) * 2;
  u.mem[k] = l;
  u.mem[k + 1] = r;
  ++u.w;
}
double beat_time(const FxUnit& u, const FxCtx& cx) { return u.seconds > 0 ? u.seconds * cx.sr : u.beats * cx.bf; }

// Two-tap crossfading delay-line pitch shifter (stereo). ratio > 1 raises the pitch.
inline void ps_step(FxUnit& u, float il, float ir, double ratio, float& ol, float& orr) {
  const std::int64_t m = u.acap - 1;
  float* a = u.aux.data();
  const std::size_t wi = static_cast<std::size_t>(u.aw & m) * 2;
  a[wi] = il;
  a[wi + 1] = ir;
  const double W = u.sr * 0.04;
  u.ph += (1.0 - ratio) / W;
  u.ph -= std::floor(u.ph);
  const double ph2 = u.ph >= 0.5 ? u.ph - 0.5 : u.ph + 0.5;
  const float g1 = 0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(u.ph));
  float res[2] = {0, 0};
  for (int t = 0; t < 2; ++t) {
    const double d = (t == 0 ? u.ph : ph2) * W + 2.0;
    const double rp = static_cast<double>(u.aw) - d;
    const std::int64_t i = static_cast<std::int64_t>(std::floor(rp));
    const float f = static_cast<float>(rp - static_cast<double>(i));
    const float valid = static_cast<float>(std::clamp((static_cast<double>(u.aw) - d) / 64.0, 0.0, 1.0));
    const float g = (t == 0 ? g1 : 1.0f - g1) * valid;
    for (int c = 0; c < 2; ++c) {
      const float x0 = a[static_cast<std::size_t>(i & m) * 2 + static_cast<std::size_t>(c)];
      const float x1 = a[static_cast<std::size_t>((i + 1) & m) * 2 + static_cast<std::size_t>(c)];
      res[c] += (x0 + (x1 - x0) * f) * g;
    }
  }
  ++u.aw;
  ol = res[0];
  orr = res[1];
}

// Beat-synced LFO phase at the start of a block and its per-frame increment.
void lfo_start(const FxUnit& u, const FxCtx& cx, double& p, double& dp) {
  const double period = std::max(1.0 / 64, u.seconds > 0 ? u.seconds * cx.sr / cx.bf : static_cast<double>(u.beats));
  p = cx.beat_pos / period;
  p -= std::floor(p);
  dp = 1.0 / (cx.bf * period);
}

// ---- echoes -------------------------------------------------------------------------------------
enum EchoMode { kEcho, kPing, kSpiral, kLowCut, kDub, kTape };

void echo_run(FxUnit& u, const FxCtx& cx, float* buf, int n, int mode) {
  const K k = coefs(cx);
  const double maxd = static_cast<double>(u.cap - 8);
  double tgt = beat_time(u, cx);
  while (tgt > maxd) tgt *= 0.5;
  tgt = std::max(tgt, 8.0);
  if (u.time_s < 0) u.time_s = tgt;
  float lp_a = lp_coef(mode == kDub ? 1800.0 : 6000.0, cx.sr);
  float lp2_a = 1.0f;
  double wob_dp = 0;
  if (mode == kTape) {  // repeats get darker the lower the tone knob; a second pole keeps the roll-off tape-like
    lp_a = lp_coef(std::min(450.0 * std::pow(20.0, static_cast<double>(u.param2_s)), cx.sr * 0.45), cx.sr);
    lp2_a = lp_a;
    wob_dp = 0.55 / cx.sr;
  }
  float peak = 0;
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    u.time_s += std::clamp(tgt - u.time_s, -0.25, 0.25);  // slewed: a time change glides like a tape echo
    float yl, yr;
    double rd = u.time_s;
    if (mode == kTape) {  // slow wow plus a faster flutter, up to ~0.35 % of the delay time
      u.d0 += wob_dp;
      u.d0 -= std::floor(u.d0);
      const double w = std::sin(2.0 * kPi * u.d0) + 0.35 * std::sin(2.0 * kPi * 9.0 * u.d0);
      rd = u.time_s * (1.0 + 0.0026 * static_cast<double>(u.param3_s) * w);
    }
    dl_read(u, 0, rd, yl);
    dl_read(u, 1, rd, yr);
    float fb = 0.15f + 0.7f * u.param_s;
    float hp_a = 0;
    double ratio = 1.0;
    if (mode == kSpiral) {
      fb = 0.6f;
      ratio = std::exp2((0.3 + 1.2 * u.param_s) / 12.0);
    } else if (mode == kLowCut) {
      fb = 0.5f;
      hp_a = lp_coef(150.0 + 1350.0 * u.param_s, cx.sr);
    } else if (mode == kDub) {
      fb = 0.5f + 0.3f * u.param_s;
      hp_a = lp_coef(150.0, cx.sr);
    }
    if (mode == kTape) {
      hp_a = lp_coef(90.0, cx.sr);
      fb = 0.1f + 0.78f * u.param_s;
      u.s[6] += lp_a * (yl - u.s[6]);
      u.s[7] += lp_a * (yr - u.s[7]);
      u.s[0] += lp2_a * (u.s[6] - u.s[0]);
      u.s[1] += lp2_a * (u.s[7] - u.s[1]);
    } else {
      u.s[0] += lp_a * (yl - u.s[0]);
      u.s[1] += lp_a * (yr - u.s[1]);
    }
    float pl = u.s[0], pr = u.s[1];
    if (hp_a > 0) {
      u.s[2] += hp_a * (pl - u.s[2]);
      u.s[3] += hp_a * (pr - u.s[3]);
      pl -= u.s[2];
      pr -= u.s[3];
    }
    if (mode == kTape) {  // tape saturation on the repeats: gentle, level-dependent
      pl = pl / (1.0f + 0.35f * std::fabs(pl));
      pr = pr / (1.0f + 0.35f * std::fabs(pr));
    }
    const bool raw_out = mode == kEcho || mode == kPing || mode == kSpiral;
    const float ol = raw_out ? yl : pl, orr = raw_out ? yr : pr;
    if (mode == kSpiral) ps_step(u, pl, pr, ratio, pl, pr);
    const float inl = buf[i * 2] * u.on_s, inr = buf[i * 2 + 1] * u.on_s;
    float wl, wr;
    if (mode == kPing) {
      wl = 0.5f * (inl + inr) + fb * pr;
      wr = fb * pl;
    } else {
      wl = inl + fb * pl;
      wr = inr + fb * pr;
    }
    wl = fz(wl);
    wr = fz(wr);
    dl_write(u, wl, wr);
    peak = std::max(peak, std::fabs(wl) + std::fabs(wr));
    buf[i * 2] += u.wet_s * ol;
    buf[i * 2 + 1] += u.wet_s * orr;
  }
  track_quiet(u, peak, n);
}
void echo_proc(FxUnit& u, const FxCtx& c, float* b, int n) { echo_run(u, c, b, n, kEcho); }
void ping_proc(FxUnit& u, const FxCtx& c, float* b, int n) { echo_run(u, c, b, n, kPing); }
void spiral_proc(FxUnit& u, const FxCtx& c, float* b, int n) { echo_run(u, c, b, n, kSpiral); }
void lowcut_proc(FxUnit& u, const FxCtx& c, float* b, int n) { echo_run(u, c, b, n, kLowCut); }
void dub_proc(FxUnit& u, const FxCtx& c, float* b, int n) { echo_run(u, c, b, n, kDub); }
void tape_proc(FxUnit& u, const FxCtx& c, float* b, int n) { echo_run(u, c, b, n, kTape); }

// ---- plate reverb ----------------------------------------------------------------------------------
// Input diffusion (4 all-passes) feeding two cross-coupled tanks (modulated all-pass, delay, damping,
// all-pass, delay). Lines live in `mem` as mono blocks; n[0..11] lengths, n[12..23] offsets, n[24..35] indices.
void reverb_reset(FxUnit& u) {
  static const int base[12] = {142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163};
  const double sc = u.sr / 29761.0;
  std::int32_t off = 0;
  for (int k = 0; k < 12; ++k) {
    std::int32_t len = std::max(16, static_cast<std::int32_t>(std::lround(base[k] * sc)));
    if (k == 4 || k == 8) len += 24;  // room for the modulation
    u.n[k] = len;
    u.n[12 + k] = off;
    u.n[24 + k] = 0;
    off += len;
  }
}
void reverb_run(FxUnit& u, const FxCtx& cx, float* buf, int n, bool beat_decay) {
  const K k = coefs(cx);
  // Beat length decay: the signal passes one tank in about 0.36 s and meets `decay` twice on the way (at the cross-feed
  // and after the damping), so decay = 10^(-1.5 * 0.36 / T) falls 60 dB in T.
  const float beat_decay_target = beat_decay
      ? static_cast<float>(std::clamp(std::pow(10.0, -1.5 * 0.36 / std::clamp(beat_time(u, cx) / cx.sr, 0.15, 12.0)), 0.2, 0.965))
      : 0.0f;
  if (beat_decay && u.s[12] <= 0.0f) u.s[12] = beat_decay_target;
  float* base = u.mem.data();
  const double sc = u.sr / 29761.0;
  const auto ap = [&](int line, float x, float g) {
    float* b = base + u.n[12 + line];
    std::int32_t& idx = u.n[24 + line];
    const float z = b[idx], w = x + g * z, y = z - g * w;
    b[idx] = fz(w);
    if (++idx >= u.n[line]) idx = 0;
    return y;
  };
  const auto apm = [&](int line, float x, float g, float lfo) {
    float* b = base + u.n[12 + line];
    std::int32_t& idx = u.n[24 + line];
    const int len = u.n[line];
    const float d = static_cast<float>(len - 24) + 12.0f + 10.0f * lfo;  // delay in [len-22, len-2]
    float rp = static_cast<float>(idx) - d;
    if (rp < 0) rp += static_cast<float>(len);
    const int i0 = static_cast<int>(rp);
    const float f = rp - static_cast<float>(i0);
    const int i1 = i0 + 1 >= len ? 0 : i0 + 1;
    const float z = b[i0] + (b[i1] - b[i0]) * f;
    const float w = x + g * z, y = z - g * w;
    b[idx] = fz(w);
    if (++idx >= len) idx = 0;
    return y;
  };
  const auto del = [&](int line, float x) {
    float* b = base + u.n[12 + line];
    std::int32_t& idx = u.n[24 + line];
    const float y = b[idx];
    b[idx] = fz(x);
    if (++idx >= u.n[line]) idx = 0;
    return y;
  };
  const auto tap = [&](int line, int t) {
    const int len = u.n[line];
    int tt = std::clamp(static_cast<int>(std::lround(t * sc)), 1, len);
    int p = u.n[24 + line] - tt;
    if (p < 0) p += len;
    return base[u.n[12 + line] + p];
  };
  float peak = 0;
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    if (beat_decay) u.s[12] += (beat_decay_target - u.s[12]) * k.par;
    const float decay = beat_decay ? u.s[12] : 0.45f + 0.5f * u.param_s;
    u.ph += 0.8 / cx.sr;
    u.ph -= std::floor(u.ph);
    const float lfo = std::sin(kTwoPi * static_cast<float>(u.ph));
    float x = 0.5f * (buf[i * 2] + buf[i * 2 + 1]) * u.on_s;
    u.s[0] += 0.8f * (x - u.s[0]);
    x = u.s[0];
    x = ap(0, x, 0.75f);
    x = ap(1, x, 0.75f);
    x = ap(2, x, 0.625f);
    x = ap(3, x, 0.625f);
    const float p2 = u.s[2], p3 = u.s[3];
    float a = apm(4, x + decay * p3, -0.7f, lfo);
    float d = del(5, a);
    u.s[4] = d * 0.6f + u.s[4] * 0.4f;
    u.s[2] = del(7, ap(6, u.s[4] * decay, 0.5f));
    a = apm(8, x + decay * p2, -0.7f, -lfo);
    d = del(9, a);
    u.s[5] = d * 0.6f + u.s[5] * 0.4f;
    u.s[3] = del(11, ap(10, u.s[5] * decay, 0.5f));
    const float ol = tap(9, 266) + tap(9, 2974) - tap(10, 1913) + tap(11, 1996) - tap(5, 1990) - tap(6, 187) - tap(7, 1066);
    const float orr = tap(5, 353) + tap(5, 3627) - tap(6, 1228) + tap(7, 2673) - tap(9, 2111) - tap(10, 335) - tap(11, 121);
    peak = std::max(peak, std::fabs(x) + std::fabs(u.s[2]) + std::fabs(u.s[3]));
    buf[i * 2] += u.wet_s * 0.5f * ol;
    buf[i * 2 + 1] += u.wet_s * 0.5f * orr;
  }
  track_quiet(u, peak, n);
}

void reverb_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) { reverb_run(u, cx, buf, n, false); }
void beat_reverb_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) { reverb_run(u, cx, buf, n, true); }

// ---- modulation ------------------------------------------------------------------------------------
void flanger_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  double p, dp;
  lfo_start(u, cx, p, dp);
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const float lfoL = 0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(p));
    const float lfoR = 0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(p + 0.25));
    p += dp;
    if (p >= 1.0) p -= 1.0;
    const float fb = (u.param_s * 2.0f - 1.0f) * 0.7f;
    float yl, yr;
    dl_read(u, 0, cx.sr * (0.0004 + 0.0036 * lfoL), yl);
    dl_read(u, 1, cx.sr * (0.0004 + 0.0036 * lfoR), yr);
    const float l = buf[i * 2], r = buf[i * 2 + 1];
    dl_write(u, fz(l + fb * yl), fz(r + fb * yr));
    const float m = u.on_s * u.wet_s;
    buf[i * 2] = l * (1 - m) + 0.5f * (l + yl) * m;
    buf[i * 2 + 1] = r * (1 - m) + 0.5f * (r + yr) * m;
  }
}

void phaser_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  double p, dp;
  lfo_start(u, cx, p, dp);
  float a = 0;
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    if ((i & 15) == 0) {
      const double lfo = 0.5 - 0.5 * std::cos(2 * kPi * p);
      const double f = 200.0 * std::pow(4000.0 / 200.0, lfo);
      const double tn = std::tan(kPi * std::min(f, cx.sr * 0.45) / cx.sr);
      a = static_cast<float>((1.0 - tn) / (1.0 + tn));
    }
    p += dp;
    if (p >= 1.0) p -= 1.0;
    const float fb = 0.6f * u.param_s;
    float dry[2] = {buf[i * 2], buf[i * 2 + 1]};
    float wetv[2];
    for (int c = 0; c < 2; ++c) {
      float x = dry[c] + fb * u.s[12 + c];
      for (int st = 0; st < 6; ++st) {
        float& z = u.s[c * 6 + st];
        const float y = a * x + z;
        z = fz(x - a * y);
        x = y;
      }
      u.s[12 + c] = fz(x);
      wetv[c] = 0.5f * (dry[c] + x);
    }
    const float m = u.on_s * u.wet_s;
    buf[i * 2] = dry[0] * (1 - m) + wetv[0] * m;
    buf[i * 2 + 1] = dry[1] * (1 - m) + wetv[1] * m;
  }
}

void filter_sweep_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  double p, dp;
  lfo_start(u, cx, p, dp);
  float a1 = 0, a2 = 0, a3 = 0, kk = 1;
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    if ((i & 15) == 0) {
      const double lfo = 0.5 - 0.5 * std::cos(2 * kPi * p);
      const double f = std::min(150.0 * std::pow(8000.0 / 150.0, lfo), cx.sr * 0.45);
      const double g = std::tan(kPi * f / cx.sr);
      kk = static_cast<float>(1.0 / (0.7 + 6.0 * u.param_s));
      a1 = static_cast<float>(1.0 / (1.0 + g * (g + kk)));
      a2 = static_cast<float>(g) * a1;
      a3 = static_cast<float>(g) * a2;
    }
    p += dp;
    if (p >= 1.0) p -= 1.0;
    const float m = u.on_s * u.wet_s;
    for (int c = 0; c < 2; ++c) {
      const float x = buf[i * 2 + c];
      float& ic1 = u.s[c * 2];
      float& ic2 = u.s[c * 2 + 1];
      const float v3 = x - ic2;
      const float v1 = a1 * ic1 + a2 * v3;
      const float v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = fz(2 * v1 - ic1);
      ic2 = fz(2 * v2 - ic2);
      buf[i * 2 + c] = x * (1 - m) + v2 * m;
    }
  }
}

// ---- rhythmic --------------------------------------------------------------------------------------
void transform_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  double p, dp;
  lfo_start(u, cx, p, dp);
  const float sm = static_cast<float>(1.0 - std::exp(-1.0 / (0.002 * cx.sr)));
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const double duty = 0.15 + 0.7 * u.param_s;
    u.s[0] += ((p < duty ? 1.0f : 0.0f) - u.s[0]) * sm;
    p += dp;
    if (p >= 1.0) p -= 1.0;
    const float m = u.on_s * u.wet_s, g = 1.0f - m + m * u.s[0];
    buf[i * 2] *= g;
    buf[i * 2 + 1] *= g;
  }
}

// Beat repeat: the first slice after switching on plays live while it is captured, then it loops.
void roll_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  constexpr int kFade = 96;
  const std::int32_t len = static_cast<std::int32_t>(std::clamp<double>(beat_time(u, cx), 2 * kFade + 16, static_cast<double>(u.cap - 8)));
  if (u.rise) {
    u.n[0] = 1;  // capturing
    u.n[3] = 0;
    u.n[1] = 0;
    u.n[2] = len;
  }
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const float l = buf[i * 2], r = buf[i * 2 + 1];
    float fl = l, fr = r;
    if (u.n[0] == 1) {
      u.mem[static_cast<std::size_t>(u.n[1]) * 2] = l;
      u.mem[static_cast<std::size_t>(u.n[1]) * 2 + 1] = r;
      if (++u.n[1] >= u.n[2]) {
        u.n[0] = 2;
        u.n[1] = 0;
      }
    } else if (u.n[0] == 2) {
      const int pos = u.n[1];
      const float g = std::min({1.0f, static_cast<float>(pos) / kFade, static_cast<float>(u.n[2] - 1 - pos) / kFade});
      fl = u.mem[static_cast<std::size_t>(pos) * 2] * g;
      fr = u.mem[static_cast<std::size_t>(pos) * 2 + 1] * g;
      if (u.n[3] == 0) {  // first pass: cross-fade from the live signal instead of dipping to silence
        const float live = std::min(1.0f, static_cast<float>(pos) / kFade);
        fl = fl + l * (1.0f - live);
        fr = fr + r * (1.0f - live);
        if (pos >= kFade) u.n[3] = 1;
      }
      if (++u.n[1] >= u.n[2]) u.n[1] = 0;
    }
    const float m = u.on_s * u.wet_s;
    buf[i * 2] = l * (1 - m) + fl * m;
    buf[i * 2 + 1] = r * (1 - m) + fr * m;
  }
}

// ---- pitch, crush, noise ----------------------------------------------------------------------
void pitch_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const double ratio = std::exp2((u.param_s - 0.5) * 2.0);  // +-12 semitones
    float ol, orr;
    ps_step(u, buf[i * 2], buf[i * 2 + 1], ratio, ol, orr);
    const float m = u.on_s * u.wet_s;
    buf[i * 2] = buf[i * 2] * (1 - m) + ol * m;
    buf[i * 2 + 1] = buf[i * 2 + 1] * (1 - m) + orr * m;
  }
}

void crush_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const float p = u.param_s;
    const float levels = std::exp2(15.0f - 11.0f * p);
    const int hold = 1 + static_cast<int>(p * p * 14.0f);
    const float m = u.on_s * u.wet_s;
    follow_env(u, 0.5f * (std::fabs(buf[i * 2]) + std::fabs(buf[i * 2 + 1])), 0.99995f);
    if (--u.n[0] <= 0) {
      u.n[0] = hold;
      u.s[0] = buf[i * 2];
      u.s[1] = buf[i * 2 + 1];
    }
    const float hiss = p * 0.05f * u.env_in;
    for (int c = 0; c < 2; ++c) {
      const float x = u.s[c] + hiss * noise(u);
      const float q = std::round(std::clamp(x, -1.0f, 1.0f) * levels) / levels;
      buf[i * 2 + c] = buf[i * 2 + c] * (1 - m) + q * m;
    }
  }
}

// Colour noise: band-passed noise, level and centre frequency from the knob, following the signal level.
void noise_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  float a1 = 0, a2 = 0, a3 = 0;
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    if ((i & 15) == 0) {
      const double f = std::min(250.0 * std::exp2(6.0 * u.param_s), cx.sr * 0.45);
      const double g = std::tan(kPi * f / cx.sr), kk = 1.0 / 1.5;
      a1 = static_cast<float>(1.0 / (1.0 + g * (g + kk)));
      a2 = static_cast<float>(g) * a1;
      a3 = static_cast<float>(g) * a2;
    }
    follow_env(u, 0.5f * (std::fabs(buf[i * 2]) + std::fabs(buf[i * 2 + 1])), 0.99997f);
    const float amp = u.param_s * 0.5f * std::min(1.0f, 3.0f * u.env_in) * u.on_s;
    float nz[2];
    for (int c = 0; c < 2; ++c) {
      const float x = noise(u);
      float& ic1 = u.s[c * 2];
      float& ic2 = u.s[c * 2 + 1];
      const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = fz(2 * v1 - ic1);
      ic2 = fz(2 * v2 - ic2);
      nz[c] = v1 * 1.5f;  // band-pass output
    }
    buf[i * 2] += amp * nz[0];
    buf[i * 2 + 1] += amp * nz[1];
  }
}

// ---- release effects ---------------------------------------------------------------------------
void brake_run(FxUnit& u, const FxCtx& cx, float* buf, int n, bool spin, bool tape = false) {
  const K k = coefs(cx);
  const std::int64_t m = u.cap - 1;
  const double T = std::clamp(beat_time(u, cx), 0.1 * cx.sr, static_cast<double>(u.cap) * (spin ? 0.2 : 0.45));
  if (u.rise) {
    u.d0 = static_cast<double>(u.w);
    u.d1 = 0;
  }
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const float l = buf[i * 2], r = buf[i * 2 + 1];
    const std::size_t wi = static_cast<std::size_t>(u.w & m) * 2;
    u.mem[wi] = l;
    u.mem[wi + 1] = r;
    ++u.w;
    if (!(u.on || u.on_s > 1e-4f)) continue;
    double v = spin ? -3.5 * std::exp(-u.d1 / (T / 3.0)) : std::max(0.0, 1.0 - u.d1 / T);
    if (tape) v = std::pow(std::max(0.0, 1.0 - u.d1 / T), 1.0 + 3.0 * static_cast<double>(u.param_s));
    u.d1 += 1;
    u.d0 = std::clamp(u.d0 + v, static_cast<double>(u.w - u.cap + 16), static_cast<double>(u.w - 2));
    const std::int64_t i0 = static_cast<std::int64_t>(std::floor(u.d0));
    const float f = static_cast<float>(u.d0 - static_cast<double>(i0));
    float o[2];
    for (int c = 0; c < 2; ++c) {
      const float x0 = u.mem[static_cast<std::size_t>(i0 & m) * 2 + static_cast<std::size_t>(c)];
      const float x1 = u.mem[static_cast<std::size_t>((i0 + 1) & m) * 2 + static_cast<std::size_t>(c)];
      o[c] = x0 + (x1 - x0) * f;
    }
    const float amp = static_cast<float>(std::min(1.0, std::fabs(v) * 6.0));
    const float mx = u.on_s * u.wet_s;
    buf[i * 2] = l * (1 - mx) + o[0] * amp * mx;
    buf[i * 2 + 1] = r * (1 - mx) + o[1] * amp * mx;
  }
}
void brake_proc(FxUnit& u, const FxCtx& c, float* b, int n) { brake_run(u, c, b, n, false); }
void spin_proc(FxUnit& u, const FxCtx& c, float* b, int n) { brake_run(u, c, b, n, true); }
void tapestop_proc(FxUnit& u, const FxCtx& c, float* b, int n) { brake_run(u, c, b, n, false, true); }

// ---- appended effects (protocol 5) ---------------------------------------------------------------------------
// 3-band isolator: two Linkwitz-Riley (4th order) crossovers, 250 Hz and 2.5 kHz, built from cascaded second-order
// state-variable filters. The bands add back to a flat magnitude (phase-shifted like any crossover); each band has
// its own gain: 0 = kill, 0.5 = unity, 1 = +6 dB.
inline float iso_gain(float k) { return k < 0.5f ? (2.0f * k) * (2.0f * k) : 1.0f + (k - 0.5f) * 2.0f; }
struct IsoCoef {
  float a1, a2, a3;
};
inline IsoCoef iso_coef(double fc, double sr) {
  const double g = std::tan(kPi * std::min(fc, sr * 0.45) / sr), k = 1.41421356237;
  const float a1 = static_cast<float>(1.0 / (1.0 + g * (g + k)));
  return {a1, static_cast<float>(g) * a1, static_cast<float>(g) * static_cast<float>(g) * a1};
}
inline void iso_svf(float x, float* st, const IsoCoef& c, float& lp, float& hp) {
  const float k = 1.41421356f;
  const float v3 = x - st[1];
  const float v1 = c.a1 * st[0] + c.a2 * v3;
  const float v2 = st[1] + c.a2 * st[0] + c.a3 * v3;
  st[0] = fz(2 * v1 - st[0]);
  st[1] = fz(2 * v2 - st[1]);
  lp = v2;
  hp = x - k * v1 - v2;
}
// Band kill fader law: unity at 0.5, +6 dB at 1, then dB-linear down to -60 dB at 0.04 and an exact zero below it.
inline float kill_gain(float k) {
  if (k >= 0.5f) return 1.0f + (k - 0.5f) * 2.0f;
  if (k <= 0.04f) return 0.0f;
  return std::pow(10.0f, -3.0f * (0.5f - k) / 0.46f);
}
void iso_run(FxUnit& u, const FxCtx& cx, float* buf, int n, double f1, double f2, bool kill_law) {
  const K k = coefs(cx);
  const IsoCoef c1 = iso_coef(f1, cx.sr), c2 = iso_coef(f2, cx.sr);
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const float gl = kill_law ? kill_gain(u.param_s) : iso_gain(u.param_s), gm = kill_law ? kill_gain(u.param2_s) : iso_gain(u.param2_s),
                gh = kill_law ? kill_gain(u.param3_s) : iso_gain(u.param3_s);
    const float m = u.on_s * u.wet_s;
    for (int c = 0; c < 2; ++c) {
      float* st = u.s + c * 16;  // 8 two-state filters: low path (2), high path (2) at 250 Hz; the same at 2.5 kHz
      const float x = buf[i * 2 + c];
      float t, lo, h1, d;
      iso_svf(x, st, c1, t, d);
      iso_svf(t, st + 2, c1, lo, d);
      iso_svf(x, st + 4, c1, d, t);
      iso_svf(t, st + 6, c1, d, h1);
      float mid, hi;
      iso_svf(h1, st + 8, c2, t, d);
      iso_svf(t, st + 10, c2, mid, d);
      iso_svf(h1, st + 12, c2, d, t);
      iso_svf(t, st + 14, c2, d, hi);
      const float y = lo * gl + mid * gm + hi * gh;
      buf[i * 2 + c] = x + (y - x) * m;
    }
  }
}
void isolator_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) { iso_run(u, cx, buf, n, 250.0, 2500.0, false); }
void bandkill_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) { iso_run(u, cx, buf, n, 160.0, 3500.0, true); }

// Slicer: eight steps of one beat division; param picks how many of them play (a Euclidean spread), param2 rotates
// the pattern, param3 sets how much of each step the gate stays open.
void slicer_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  const double period = std::max(1.0 / 64, u.seconds > 0 ? u.seconds * cx.sr / cx.bf : static_cast<double>(u.beats));
  const int hits = 1 + static_cast<int>(std::lround(std::clamp(u.param, 0.0f, 1.0f) * 7.0f));
  const int rot = static_cast<int>(std::lround(std::clamp(u.param2, 0.0f, 1.0f) * 7.0f));
  const float sm = static_cast<float>(1.0 - std::exp(-1.0 / (0.002 * cx.sr)));
  double sp = cx.beat_pos / period;
  const double dsp = 1.0 / (cx.bf * period);
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const double fl = std::floor(sp);
    const double frac = sp - fl;
    const int step = static_cast<int>(std::fmod(fl, 8.0) + 8.0) & 7;
    const bool hit = (((step + rot) & 7) * hits) % 8 < hits;
    const float open = 0.15f + 0.8f * u.param3_s;
    u.s[0] += ((hit && frac < open ? 1.0f : 0.0f) - u.s[0]) * sm;
    sp += dsp;
    const float m = u.on_s * u.wet_s, g = 1.0f - m + m * u.s[0];
    buf[i * 2] *= g;
    buf[i * 2 + 1] *= g;
  }
}

// Build-up: one knob (param) raises a high-pass, feeds a beat-synced echo with growing feedback and fades in a noise
// riser. Switching off does not cut: the effect is released on the next beat line, the filter and the riser leave in a
// few milliseconds and the echo rings out like any other tail.
void buildup_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  const double maxd = static_cast<double>(u.cap - 8);
  double tgt = beat_time(u, cx);
  while (tgt > maxd) tgt *= 0.5;
  tgt = std::max(tgt, 8.0);
  if (u.time_s < 0) u.time_s = tgt;
  const float ke = static_cast<float>(1.0 - std::exp(-1.0 / (0.004 * cx.sr)));
  if (u.rise) {
    u.n[0] = 1;
    u.n[2] = 0;
    u.hold = true;
  }
  if (u.on) u.n[2] = 0;
  else if (u.n[0] == 1) u.n[2] = 1;  // release when the next beat starts
  double bp = cx.beat_pos;
  const double dbp = 1.0 / cx.bf;
  float peak = 0;
  float hp_a = 0, nz_a = 0;
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const std::int32_t bi = static_cast<std::int32_t>(std::floor(bp));
    bp += dbp;
    if (u.n[2] == 1 && bi != u.n[1]) {
      u.n[0] = 0;
      u.n[2] = 0;
    }
    u.n[1] = bi;
    u.s[10] += (static_cast<float>(u.n[0]) - u.s[10]) * ke;
    if (u.n[0] == 0 && u.s[10] < 1e-4f) {
      u.s[10] = 0;
      u.hold = false;
    }
    const float amt = u.param_s * u.s[10];
    if ((i & 7) == 0) {
      hp_a = lp_coef(30.0 * std::exp2(7.0 * static_cast<double>(amt)), cx.sr);
      nz_a = lp_coef(700.0 + 9000.0 * static_cast<double>(amt * amt), cx.sr);
    }
    u.time_s += std::clamp(tgt - u.time_s, -0.25, 0.25);
    float yl, yr;
    dl_read(u, 0, u.time_s, yl);
    dl_read(u, 1, u.time_s, yr);
    const float fb = 0.3f + 0.5f * u.param_s;  // below 0.8 so the tail always dies
    const float nzv = noise(u);
    u.s[8] += nz_a * (nzv - u.s[8]);
    u.s[9] += lp_coef(250.0, cx.sr) * (u.s[8] - u.s[9]);
    const float riser = (u.s[8] - u.s[9]) * 0.1f * amt * amt;
    const float send = amt * 0.9f;
    const float mf = std::min(1.0f, amt * 3.0f), m = u.wet_s * u.s[10];
    float outv[2], hpv[2];
    for (int c = 0; c < 2; ++c) {
      const float x = buf[i * 2 + c];
      u.s[c * 2] = fz(u.s[c * 2] + hp_a * (x - u.s[c * 2]));
      u.s[c * 2 + 1] = fz(u.s[c * 2 + 1] + hp_a * (u.s[c * 2] - u.s[c * 2 + 1]));
      hpv[c] = x - u.s[c * 2 + 1];
      const float body = x + (hpv[c] - x) * mf + riser;
      outv[c] = x + (body - x) * m;
    }
    const float wl = fz(hpv[0] * send + fb * yl), wr = fz(hpv[1] * send + fb * yr);
    dl_write(u, wl, wr);
    peak = std::max(peak, std::fabs(wl) + std::fabs(wr));
    buf[i * 2] = outv[0] + u.wet_s * 0.8f * yl;
    buf[i * 2 + 1] = outv[1] + u.wet_s * 0.8f * yr;
  }
  track_quiet(u, peak, n);
}

// ---- appended effects (protocol 9): riser, reverser, auto pan ---------------------------------------------------
// Riser: engaging starts a build that takes `beats` (or `seconds`) to reach full tension. Over that time a high-pass
// climbs, a band of noise swells and the signal falls into repeats of a snapshot that shorten every stage (1/2, 1/4,
// 1/8, 1/16 beat). Releasing waits for the next beat line, then everything leaves in 4 ms: the dry signal is back on
// the beat. param = intensity.
void riser_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  const std::int64_t m = u.cap - 1, am = u.acap - 1;
  const double total = std::max(0.05 * cx.sr, beat_time(u, cx));
  const float ke = static_cast<float>(1.0 - std::exp(-1.0 / (0.004 * cx.sr)));
  const float kr = static_cast<float>(1.0 - std::exp(-1.0 / (0.002 * cx.sr)));
  const double bf = std::max(1.0, cx.bf);
  if (u.rise) {
    u.n[0] = 1;
    u.n[2] = 0;
    u.n[3] = 0;
    u.n[6] = 0;
    u.d0 = 0;
    u.hold = true;
  }
  if (u.on) u.n[2] = 0;
  else if (u.n[0] == 1) u.n[2] = 1;
  double bp = cx.beat_pos;
  const double dbp = 1.0 / bf;
  float hp_a = 0, nz_a = 0;
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const std::int32_t bi = static_cast<std::int32_t>(std::floor(bp));
    bp += dbp;
    if (u.n[2] == 1 && bi != u.n[1]) {
      u.n[0] = 0;
      u.n[2] = 0;
    }
    u.n[1] = bi;
    u.s[10] += (static_cast<float>(u.n[0]) - u.s[10]) * ke;
    if (u.n[0] == 0 && u.s[10] < 1e-4f) {
      u.s[10] = 0;
      u.hold = false;
      u.n[6] = 0;
    }
    const double t = std::min(1.0, u.d0 / total);
    if (u.n[0] == 1) u.d0 += 1.0;
    const float tf = static_cast<float>(t), inten = 0.35f + 0.65f * u.param_s;
    // snapshot the audio just played into the aux buffer, then loop it; a new stage only starts at the loop's end
    const int want = t < 0.25 ? 0 : t < 0.5 ? 1 : t < 0.7 ? 2 : t < 0.85 ? 3 : 4;
    if (u.n[6] == 0 || u.n[4] >= u.n[5]) {
      if (want > 0 && u.n[0] == 1 && (u.n[6] != want || u.n[4] >= u.n[5])) {
        const std::int64_t L = std::clamp<std::int64_t>(static_cast<std::int64_t>(bf / static_cast<double>(1 << want)), 32, u.acap - 1);
        for (std::int64_t j = 0; j < L; ++j) {
          const std::size_t src = static_cast<std::size_t>((u.w - L + j) & m) * 2;
          u.aux[static_cast<std::size_t>(j) * 2] = u.mem[src];
          u.aux[static_cast<std::size_t>(j) * 2 + 1] = u.mem[src + 1];
        }
        u.n[5] = static_cast<std::int32_t>(L);
        u.n[6] = want;
      } else if (want == 0) {
        u.n[6] = 0;
      }
      u.n[4] = 0;
    }
    const float rep_t = (u.n[6] > 0 && u.n[0] == 1) ? 1.0f : 0.0f;
    u.s[11] += (rep_t * std::min(1.0f, (tf - 0.2f) * 6.0f) - u.s[11]) * kr;
    if (u.s[11] < 0) u.s[11] = 0;
    if ((i & 7) == 0) {
      hp_a = lp_coef(25.0 * std::exp2(7.0 * static_cast<double>(tf * inten)), cx.sr);
      nz_a = lp_coef(700.0 + 9000.0 * static_cast<double>(tf * tf), cx.sr);
    }
    float lv[2] = {0, 0};
    if (u.n[6] > 0) {
      const int j = u.n[4]++;
      const int edge = std::max(1, std::min(96, u.n[5] / 4));
      const float g = std::min({1.0f, static_cast<float>(j) / static_cast<float>(edge), static_cast<float>(u.n[5] - j) / static_cast<float>(edge)});
      const std::size_t a = static_cast<std::size_t>(std::min(j, u.n[5] - 1) & am) * 2;
      lv[0] = u.aux[a] * g;
      lv[1] = u.aux[a + 1] * g;
    }
    const float nzv = noise(u);
    u.s[8] += nz_a * (nzv - u.s[8]);
    u.s[9] += lp_coef(250.0, cx.sr) * (u.s[8] - u.s[9]);
    const float riser = (u.s[8] - u.s[9]) * 0.12f * tf * tf * inten;
    const float mf = std::min(1.0f, tf * 3.0f), mix = u.s[10] * u.wet_s, rp = u.s[11];
    for (int c = 0; c < 2; ++c) {
      const float x = buf[i * 2 + c];
      const std::size_t wi = static_cast<std::size_t>(u.w & m) * 2 + static_cast<std::size_t>(c);
      u.mem[wi] = x;
      const float y = x + (lv[c] - x) * rp;
      u.s[c * 2] = fz(u.s[c * 2] + hp_a * (y - u.s[c * 2]));
      u.s[c * 2 + 1] = fz(u.s[c * 2 + 1] + hp_a * (u.s[c * 2] - u.s[c * 2 + 1]));
      const float hp = y - u.s[c * 2 + 1];
      const float body = y + (hp - y) * mf + riser;
      buf[i * 2 + c] = x + (body - x) * mix;
    }
    ++u.w;
  }
}

// Reverser: each beat-length slice is played backwards from the slice before it (so it sounds one slice late), with
// short fades at the seams. Listens all the time so the first slice after engaging is already there.
void reverser_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  const double L = std::clamp(beat_time(u, cx), 64.0, static_cast<double>(u.cap / 2 - 16));
  double ph = cx.beat_pos / (L / cx.bf);
  ph -= std::floor(ph);
  // Free-running phase: the beat position is continuous from chunk to chunk, but a tempo change or a playhead nudge moves it
  // a little and a read position that followed it would click. Small errors are slewed out slowly; a real jump (seek,
  // loop, first use) snaps.
  if (u.rise || u.n[7] == 0) {
    u.d0 = ph;
    u.n[7] = 1;
  } else {
    double e = ph - u.d0;
    e -= std::round(e);
    if (std::fabs(e) * L > 0.04 * cx.bf) u.d0 = ph;
    else u.d0 += e * 0.02;
    if (u.d0 < 0) u.d0 += 1.0;
    if (u.d0 >= 1.0) u.d0 -= 1.0;
    ph = u.d0;
  }
  const double dph = 1.0 / L;
  const float edge = static_cast<float>(0.004 * cx.sr);
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    dl_write(u, buf[i * 2], buf[i * 2 + 1]);
    const float mx = u.on_s * u.wet_s;
    if (mx > 1e-5f) {
      const double p = ph * L;
      float rl, rr;
      dl_read(u, 0, 2.0 * p + 1.0, rl);
      dl_read(u, 1, 2.0 * p + 1.0, rr);
      const float g = std::min({1.0f, static_cast<float>(p) / edge, static_cast<float>(L - p) / edge});
      buf[i * 2] += (rl * g - buf[i * 2]) * mx;
      buf[i * 2 + 1] += (rr * g - buf[i * 2 + 1]) * mx;
    }
    ph += dph;
    if (ph >= 1.0) ph -= 1.0;
  }
  u.d0 = ph;
}

// Auto pan: an equal-power pan swings left and right once per `beats`; param = depth, param2 = shape (sine to a soft square).
void autopan_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  double p, dp;
  lfo_start(u, cx, p, dp);
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const float sh = 1.0f + 9.0f * u.param2_s;
    const float lfo = std::tanh(std::sin(kTwoPi * static_cast<float>(p)) * sh) / std::tanh(sh);
    p += dp;
    if (p >= 1.0) p -= 1.0;
    const float th = 0.78539816f * (1.0f + u.param_s * lfo);
    const float m = u.on_s * u.wet_s;
    const float gl = 1.0f + (1.41421356f * std::cos(th) - 1.0f) * m, gr = 1.0f + (1.41421356f * std::sin(th) - 1.0f) * m;
    buf[i * 2] *= gl;
    buf[i * 2 + 1] *= gr;
  }
}

// One-knob DJ filter: 0.5 = off, turning down closes a low-pass, turning up opens a high-pass (param2 = resonance).
void filter_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
  float a1 = 1, a2 = 0, a3 = 0, kk = 1;
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    const float knob = u.param_s;
    const bool low = knob < 0.5f;
    if ((i & 7) == 0) {
      const double a = std::min(1.0, std::fabs(static_cast<double>(knob) - 0.5) * 2.0);
      const double f = std::min(low ? 18000.0 * std::pow(60.0 / 18000.0, a) : 20.0 * std::pow(8000.0 / 20.0, a), cx.sr * 0.45);
      const double g = std::tan(kPi * f / cx.sr);
      kk = static_cast<float>(1.0 / (0.7 + 5.0 * static_cast<double>(u.param2_s)));
      a1 = static_cast<float>(1.0 / (1.0 + g * (g + kk)));
      a2 = static_cast<float>(g) * a1;
      a3 = static_cast<float>(g) * a2;
    }
    const float m = u.on_s * u.wet_s;
    for (int c = 0; c < 2; ++c) {
      const float x = buf[i * 2 + c];
      float& ic1 = u.s[c * 2];
      float& ic2 = u.s[c * 2 + 1];
      const float v3 = x - ic2;
      const float v1 = a1 * ic1 + a2 * v3;
      const float v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = fz(2 * v1 - ic1);
      ic2 = fz(2 * v2 - ic2);
      const float y = low ? v2 : x - kk * v1 - v2;
      buf[i * 2 + c] = x + (y - x) * m;
    }
  }
}

void plugin_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) { fx_plugin_run(u, cx, buf, n); }
void reset_none(FxUnit&) {}
void reset_delay(FxUnit& u) { u.time_s = -1; }

// ---- registry ----------------------------------------------------------------------------------------
//        kind, name, param, synced, tail, release, colour, wet, param, beats, param2, param3, slug
const FxDef kDefs[] = {
    {{EffectKind::None, "None", "", false, false, false, false, 0, 0, 1, "", "", "none"}, nullptr, reset_none, false},
    {{EffectKind::Echo, "Echo", "feedback", true, true, false, false, 0.5f, 0.5f, 0.5f, "", "", "echo"}, echo_proc, reset_delay, false},
    {{EffectKind::PingPong, "Ping Pong", "feedback", true, true, false, false, 0.5f, 0.5f, 0.5f, "", "", "ping_pong"}, ping_proc, reset_delay, false},
    {{EffectKind::Spiral, "Spiral", "pitch drift", true, true, false, false, 0.5f, 0.5f, 0.75f, "", "", "spiral"}, spiral_proc, reset_delay, false},
    {{EffectKind::Reverb, "Reverb", "decay", false, true, false, false, 0.4f, 0.5f, 1.0f, "", "", "reverb"}, reverb_proc, reverb_reset, false},
    {{EffectKind::Flanger, "Flanger", "feedback", true, false, false, false, 1.0f, 0.7f, 4.0f, "", "", "flanger"}, flanger_proc, reset_none, false},
    {{EffectKind::Phaser, "Phaser", "feedback", true, false, false, false, 1.0f, 0.6f, 4.0f, "", "", "phaser"}, phaser_proc, reset_none, false},
    {{EffectKind::LowCutEcho, "Low Cut Echo", "cut frequency", true, true, false, false, 0.5f, 0.3f, 0.5f, "", "", "low_cut_echo"}, lowcut_proc, reset_delay, false},
    {{EffectKind::FilterSweep, "Filter Sweep", "resonance", true, false, false, false, 1.0f, 0.4f, 2.0f, "", "", "filter_sweep"}, filter_sweep_proc, reset_none, false},
    {{EffectKind::Roll, "Roll", "", true, false, false, false, 1.0f, 0.5f, 0.25f, "", "", "roll"}, roll_proc, reset_none, false},
    {{EffectKind::Transform, "Transform", "gate length", true, false, false, false, 1.0f, 0.5f, 0.25f, "", "", "transform"}, transform_proc, reset_none, false},
    {{EffectKind::Pitch, "Pitch", "pitch (0.5 = none, +-12 st)", false, false, false, false, 1.0f, 0.75f, 1.0f, "", "", "pitch"}, pitch_proc, reset_none, false},
    {{EffectKind::Crusher, "Crusher", "crush amount", false, false, false, false, 1.0f, 0.5f, 1.0f, "", "", "crusher"}, crush_proc, reset_none, false},
    {{EffectKind::VinylBrake, "Vinyl Brake", "", true, false, true, false, 1.0f, 0.5f, 1.0f, "", "", "vinyl_brake"}, brake_proc, reset_none, false},
    {{EffectKind::Backspin, "Backspin", "", true, false, true, false, 1.0f, 0.5f, 1.0f, "", "", "backspin"}, spin_proc, reset_none, true},
    {{EffectKind::ColorNoise, "Noise", "", false, false, false, true, 1.0f, 0.5f, 1.0f, "", "", "color_noise"}, noise_proc, reset_none, false},
    {{EffectKind::ColorDubEcho, "Dub Echo", "", true, true, false, true, 0.5f, 0.5f, 0.75f, "", "", "color_dub_echo"}, dub_proc, reset_delay, false},
    {{EffectKind::ColorCrush, "Crush", "", false, false, false, true, 1.0f, 0.5f, 1.0f, "", "", "color_crush"}, crush_proc, reset_none, false},
    {{EffectKind::ColorPitch, "Pitch", "", false, false, false, true, 1.0f, 0.5f, 1.0f, "", "", "color_pitch"}, pitch_proc, reset_none, false},
    {{EffectKind::ColorSpace, "Space", "", false, true, false, true, 0.4f, 0.7f, 1.0f, "", "", "color_space"}, reverb_proc, reverb_reset, false},
    {{EffectKind::Isolator, "Isolator", "low", false, false, false, false, 1.0f, 0.5f, 1.0f, "mid", "high", "isolator"}, isolator_proc, reset_none, false},
    {{EffectKind::Slicer, "Slicer", "density", true, false, false, false, 1.0f, 0.5f, 0.25f, "rotation", "gate length", "slicer"}, slicer_proc, reset_none, false},
    {{EffectKind::BuildUp, "Build Up", "amount", true, true, false, false, 1.0f, 0.5f, 0.5f, "", "", "build_up"}, buildup_proc, reset_delay, false},
    {{EffectKind::Filter, "Filter", "knob (0.5 = off, low-pass below, high-pass above)", false, false, false, false, 1.0f, 0.5f, 1.0f, "resonance", "", "filter"}, filter_proc, reset_none, false},
    {{EffectKind::TapeEcho, "Tape Echo", "feedback", true, true, false, false, 0.5f, 0.5f, 0.5f, "tone", "wobble", "tape_echo"}, tape_proc, reset_delay, false},
    {{EffectKind::BeatReverb, "Beat Reverb", "", true, true, false, false, 0.4f, 0.5f, 2.0f, "", "", "beat_reverb"}, beat_reverb_proc, reverb_reset, false},
    {{EffectKind::TapeStop, "Tape Stop", "curve", true, false, true, false, 1.0f, 0.3f, 1.0f, "", "", "tape_stop"}, tapestop_proc, reset_none, false},
    {{EffectKind::Riser, "Riser", "intensity", true, false, true, false, 1.0f, 0.7f, 4.0f, "", "", "riser"}, riser_proc, reset_none, false},
    {{EffectKind::BandKill, "Band Kill", "low", false, false, false, false, 1.0f, 0.5f, 1.0f, "mid", "high", "band_kill"}, bandkill_proc, reset_none, false},
    {{EffectKind::Reverser, "Reverser", "", true, false, false, false, 1.0f, 0.5f, 1.0f, "", "", "reverser"}, reverser_proc, reset_none, true},
    {{EffectKind::AutoPan, "Auto Pan", "depth", true, false, false, false, 1.0f, 0.8f, 1.0f, "shape", "", "auto_pan"}, autopan_proc, reset_none, false},
    // Must stay last: effect_count() leaves it out. Parameters are the plug-in's own (FxChainEdit field >= kFxFieldPluginParam).
    {{EffectKind::Plugin, "Plug-in", "", false, true, false, false, 1.0f, 0.5f, 1.0f, "", "", "plugin"}, plugin_proc, reset_none, false},
};
static_assert(sizeof(kDefs) / sizeof(kDefs[0]) == static_cast<std::size_t>(EffectKind::Count), "one table row per EffectKind");

const FxDef& def_of(EffectKind k) {
  const std::size_t i = static_cast<std::size_t>(k);
  return kDefs[i < static_cast<std::size_t>(EffectKind::Count) ? i : 0];
}

bool unit_active(const FxUnit& u, const FxDef& d) {
  if (u.on || u.on_s > 1e-4f || u.hold || d.always_run) return true;
  return d.info.tail && u.quiet < u.cap + 8192;
}

void run_unit(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  if (u.kind == EffectKind::Plugin) {  // no delay memory: the plug-in decides whether it has anything to do
    plugin_proc(u, cx, buf, n);
    return;
  }
  if (u.kind == EffectKind::None || u.mem.empty()) return;
  const FxDef& d = def_of(u.kind);
  if (!unit_active(u, d)) {
    u.prev_on = false;
    if (u.kind == EffectKind::Pitch || u.kind == EffectKind::ColorPitch) {  // keep the shifter's history warm
      for (int i = 0; i < n; ++i) {
        const std::size_t wi = static_cast<std::size_t>(u.aw & (u.acap - 1)) * 2;
        u.aux[wi] = buf[i * 2];
        u.aux[wi + 1] = buf[i * 2 + 1];
        ++u.aw;
      }
    }
    return;
  }
  u.rise = u.on && !u.prev_on;
  if (u.rise) u.quiet = 0;
  d.process(u, cx, buf, n);
  u.prev_on = u.on;
}

// Kinds that keep no delay memory get a small allocation (a chain may hold eight units).
bool small_memory(EffectKind k) {
  switch (k) {
    case EffectKind::Isolator: case EffectKind::BandKill: case EffectKind::AutoPan: case EffectKind::Slicer: case EffectKind::Filter: case EffectKind::Transform:
    case EffectKind::FilterSweep: case EffectKind::Phaser:
      return true;
    default:
      return false;
  }
}

void apply_defaults(FxUnit& u, const FxDef& dd) {
  u.on = false;
  u.wet = u.wet_s = dd.info.default_wet;
  u.param = u.param_s = dd.info.default_param;
  u.param2 = u.param2_s = u.kind == EffectKind::Filter ? 0.3f : 0.5f;
  u.param3 = u.param3_s = 0.5f;
  u.beats = dd.info.default_beats;
  u.seconds = 0;
  u.hold = false;
}

}  // namespace

int effect_count() { return static_cast<int>(EffectKind::Count) - 2; }  // not None, not Plugin
const EffectInfo& effect_info_at(int i) { return kDefs[1 + std::clamp(i, 0, effect_count() - 1)].info; }
const EffectInfo* effect_info(EffectKind k) {
  return static_cast<std::size_t>(k) < static_cast<std::size_t>(EffectKind::Count) ? &kDefs[static_cast<std::size_t>(k)].info : nullptr;
}
const char* effect_slug(EffectKind k) { return def_of(k).info.slug; }
EffectKind effect_from_slug(const std::string& slug) {
  for (const FxDef& d : kDefs)
    if (d.info.kind != EffectKind::None && slug == d.info.slug) return d.info.kind;
  return EffectKind::None;
}

void FxUnit::clear() {
  std::fill(mem.begin(), mem.end(), 0.0f);
  std::fill(aux.begin(), aux.end(), 0.0f);
  std::memset(s, 0, sizeof(s));
  std::memset(n, 0, sizeof(n));
  w = aw = 0;
  ph = d0 = d1 = 0;
  time_s = -1;
  env_in = 0;
  quiet = cap * 8;
  on_s = 0;
  prev_on = rise = hold = false;
  def_of(kind).reset(*this);
}

FxUnit::~FxUnit() { delete plug; }

void fx_unit_prepare(FxUnit& u, double sr) {
  if (u.kind == EffectKind::Plugin && u.plug) fx_plugin_rate(u, sr);
  std::int64_t cap = 1;
  const double secs = small_memory(u.kind) ? 0.05 : 2.5;
  while (static_cast<double>(cap) < sr * secs) cap <<= 1;
  if (u.sr == sr && u.cap == cap && !u.mem.empty()) return;
  u.sr = sr;
  u.cap = cap;
  u.acap = small_memory(u.kind) ? 64 : u.kind == EffectKind::Riser ? 262144 : 16384;
  u.mem.assign(static_cast<std::size_t>(cap) * 2, 0.0f);
  u.aux.assign(static_cast<std::size_t>(u.acap) * 2, 0.0f);
  u.clear();
}

FxUnit* fx_make_unit(EffectKind kind, double sr, std::uint32_t seed) {
  if (static_cast<std::size_t>(kind) >= static_cast<std::size_t>(EffectKind::Count)) kind = EffectKind::None;
  FxUnit* u = new FxUnit;
  u->kind = kind;
  u->rng = seed | 1u;
  fx_unit_prepare(*u, sr);
  apply_defaults(*u, def_of(kind));
  return u;
}

bool fx_unit_busy(const FxUnit& u) {
  if (u.kind == EffectKind::None) return false;
  if (u.kind == EffectKind::Plugin) return fx_plugin_busy(u);
  const FxDef& d = def_of(u.kind);
  return u.on || u.on_s > 1e-4f || u.hold || (d.info.tail && u.quiet < u.cap + 8192);
}

void fx_unit_info(const FxUnit& u, FxUnitInfo& o) {
  o.id = u.id;
  o.kind = u.kind;
  o.on = u.on ? 1 : 0;
  o.legacy_slot = static_cast<std::int8_t>(u.legacy);
  o.wet = u.wet;
  o.param = u.param;
  o.param2 = u.param2;
  o.param3 = u.param3;
  o.beats = u.beats;
  o.seconds = u.seconds;
}

void fx_unit_set(FxUnit& u, FxField f, double v) {
  const float x = static_cast<float>(v);
  switch (f) {
    case FxField::On: u.on = v > 0.5; break;
    case FxField::Wet: u.wet = std::clamp(x, 0.0f, 1.0f); break;
    case FxField::Param: u.param = std::clamp(x, 0.0f, 1.0f); break;
    case FxField::Beats: u.beats = std::clamp(x, 1.0f / 16, 16.0f); break;
    case FxField::Seconds: u.seconds = v > 0 ? std::min(v, 10.0) : 0; break;
    case FxField::Param2: u.param2 = std::clamp(x, 0.0f, 1.0f); break;
    case FxField::Param3: u.param3 = std::clamp(x, 0.0f, 1.0f); break;
    default: break;
  }
}

// ---- FxBus (colour unit only) ------------------------------------------------------------------------------
void FxBus::prepare(double sr) {
  if (colour.sr != sr || colour.mem.empty()) {
    colour.rng = 0x9E3779B9u + 0x6D2B79F5u * 4u;
    fx_unit_prepare(colour, sr);
  }
}

void FxBus::select(int slot_index, EffectKind kind) {
  if (slot_index < kFxSlots) return;
  const FxDef& d = def_of(kind);
  if (kind != EffectKind::None && !d.info.colour) kind = EffectKind::None;
  colour.kind = kind;
  apply_defaults(colour, def_of(kind));
  fx_unit_prepare(colour, colour.sr);
  colour.clear();
}

void FxBus::run_colour(float* buf, int n, const FxCtx& cx, float knob) {
  FxUnit& u = colour;
  if (u.kind == EffectKind::None) return;
  const float a = std::min(1.0f, std::fabs(knob - 0.5f) * 2.0f);
  switch (u.kind) {
    case EffectKind::ColorNoise: u.wet = 1; u.param = a; break;
    case EffectKind::ColorDubEcho: u.wet = a; u.param = a; break;
    case EffectKind::ColorCrush: u.wet = std::min(1.0f, a * 4.0f); u.param = a; break;
    case EffectKind::ColorPitch: u.wet = 1; u.param = knob; break;
    case EffectKind::ColorSpace: u.wet = a * 0.8f; u.param = 0.7f; break;
    default: break;
  }
  u.on = a > 0.02f;
  run_unit(u, cx, buf, n);
}

bool FxBus::busy() const {
  return colour.kind != EffectKind::None &&
         (colour.on_s > 1e-4f || (def_of(colour.kind).info.tail && colour.quiet < colour.cap + 8192));
}

// ---- chains -----------------------------------------------------------------------------------------------------
namespace {
bool in_state(const FxChainState* s, const FxUnit* u) {
  if (!s) return false;
  for (int i = 0; i < s->count; ++i)
    if (s->unit[i] == u) return true;
  return false;
}
}  // namespace

void FxChainRT::retire(FxUnit* u, FxChainState* s, FxGarbageRing& gar) {
  if (!gar.push(FxGarbage{u, s})) ++leaked;  // never block the audio thread; a leak beats a stall
}

void FxChainRT::install(FxChainState* next, FxGarbageRing& gar) {
  if (pending) {  // an edit arrived before the previous one took effect: the older one never ran
    for (int i = 0; i < pending->count; ++i) {
      FxUnit* u = pending->unit[i];
      if (!in_state(next, u) && !in_state(cur, u)) retire(u, nullptr, gar);
    }
    retire(nullptr, pending, gar);
  }
  pending = next;
  if (immediate) do_swap(gar);
}

void FxChainRT::do_swap(FxGarbageRing& gar) {
  FxChainState* old = cur;
  cur = pending;
  pending = nullptr;
  phase = 0;
  if (old) {
    for (int i = 0; i < old->count; ++i) {
      FxUnit* u = old->unit[i];
      if (in_state(cur, u)) continue;
      bool placed = false;
      if (u->kind != EffectKind::None && def_of(u->kind).info.tail && !immediate) {  // a delay or reverb rings out
        u->on = false;
        u->hold = false;
        for (FxUnit*& g : ghost)
          if (!g) {
            g = u;
            placed = true;
            break;
          }
      }
      if (!placed) retire(u, nullptr, gar);
    }
    retire(nullptr, old, gar);
  }
}

bool FxChainRT::busy() const {
  if (pending) return true;
  for (const FxUnit* g : ghost)
    if (g) return true;
  if (cur)
    for (int i = 0; i < cur->count; ++i)
      if (fx_unit_busy(*cur->unit[i])) return true;
  return false;
}

FxUnit* FxChainRT::find_id(int id) const {
  for (const FxChainState* s : {pending, cur}) {
    if (!s) continue;
    for (int i = 0; i < s->count; ++i)
      if (s->unit[i]->id == id) return s->unit[i];
  }
  return nullptr;
}
FxUnit* FxChainRT::find_legacy(int slot) const {
  for (const FxChainState* s : {pending, cur}) {
    if (!s) continue;
    for (int i = 0; i < s->count; ++i)
      if (s->unit[i]->legacy == slot) return s->unit[i];
  }
  return nullptr;
}

void FxChainRT::view(FxChainInfo& o) const {
  const FxChainState* s = pending ? pending : cur;
  o = FxChainInfo{};
  o.exists = (cur || pending) ? 1 : 0;
  o.bypass = bypass ? 1 : 0;
  o.wet = wet;
  if (!s) return;
  o.rev = s->rev;
  o.count = s->count;
  for (int i = 0; i < s->count; ++i) fx_unit_info(*s->unit[i], o.unit[i]);
}

void FxChainRT::view_slots(FxSlotView* out) const {
  for (int i = 0; i < kFxSlots; ++i) {
    const FxUnit* u = find_legacy(i);
    out[i] = FxSlotView{};
    out[i].wet = 0;
    out[i].param = 0;
    if (u) {
      out[i].kind = u->kind;
      out[i].on = u->on;
      out[i].wet = u->wet;
      out[i].param = u->param;
      out[i].beats = u->beats;
    }
  }
}

void FxChainRT::collect(std::vector<FxUnit*>& units, std::vector<FxChainState*>& states) const {
  for (const FxChainState* s : {cur, pending}) {
    if (!s) continue;
    for (int i = 0; i < s->count; ++i) units.push_back(s->unit[i]);
  }
  if (cur) states.push_back(cur);
  if (pending) states.push_back(pending);
  for (FxUnit* g : ghost)
    if (g) units.push_back(g);
}

void FxChainRT::run(float* buf, int n, const FxCtx& cx, FxGarbageRing& gar) {
  for (int off = 0; off < n; off += kFxBlock) {
    const int m = std::min(kFxBlock, n - off);
    FxCtx c = cx;
    c.beat_pos += static_cast<double>(off) / cx.bf;
    run_block(buf + static_cast<std::size_t>(off) * 2, m, c, gar);
  }
}

void FxChainRT::run_block(float* buf, int n, const FxCtx& cx, FxGarbageRing& gar) {
  float g0 = 1, g1 = 1;
  if (pending) {
    // A structural edit dips the chain to the dry signal over one block, swaps, and comes back over the next, so a
    // reorder or a removal never steps the output. An idle chain swaps at once.
    bool idle = !cur || cur->count == 0;
    if (!idle) {
      idle = true;
      for (int i = 0; i < cur->count && idle; ++i) idle = !fx_unit_busy(*cur->unit[i]);
    }
    if (idle || immediate || phase == 1) {
      const bool ramp_up = phase == 1;
      do_swap(gar);
      if (ramp_up) g0 = 0;
    } else {
      phase = 1;
      g1 = 0;
    }
  }
  bool ghosts = false;
  for (const FxUnit* g : ghost) ghosts = ghosts || g;
  const bool units = cur && cur->count > 0;
  if (!units && !ghosts) return;
  if (bypass && act <= 0.0f) return;  // fully bypassed: the buffer is not touched at all

  float dry[kFxBlock * 2];
  std::memcpy(dry, buf, sizeof(float) * 2 * static_cast<std::size_t>(n));
  if (units)
    for (int i = 0; i < cur->count; ++i) run_unit(*cur->unit[i], cx, buf, n);
  if (ghosts) {
    float zero[kFxBlock * 2];
    for (FxUnit*& g : ghost) {
      if (!g) continue;
      std::memset(zero, 0, sizeof(float) * 2 * static_cast<std::size_t>(n));
      g->on = false;
      run_unit(*g, cx, zero, n);
      for (int i = 0; i < n * 2; ++i) buf[i] += zero[i];
      if (!fx_unit_busy(*g)) {
        retire(g, nullptr, gar);
        g = nullptr;
      }
    }
  }
  const float step = static_cast<float>(1.0 / (0.008 * cx.sr));
  const float kw = static_cast<float>(1.0 - std::exp(-1.0 / (0.02 * cx.sr)));
  const float target = bypass ? 0.0f : 1.0f;
  const float inv = 1.0f / static_cast<float>(n);
  for (int i = 0; i < n; ++i) {
    act += std::clamp(target - act, -step, step);
    wet_s += (wet - wet_s) * kw;
    if (std::fabs(wet - wet_s) < 1e-6f) wet_s = wet;
    const float gs = g0 + (g1 - g0) * (static_cast<float>(i) * inv);
    const float m = act * wet_s * gs;
    if (m >= 1.0f) continue;
    buf[i * 2] = dry[i * 2] + (buf[i * 2] - dry[i * 2]) * m;
    buf[i * 2 + 1] = dry[i * 2 + 1] + (buf[i * 2 + 1] - dry[i * 2 + 1]) * m;
  }
}

double fx_bench_ns_per_block(EffectKind kind, int block_frames, double sample_rate, int blocks) {
  FxUnit* u = fx_make_unit(kind, sample_rate, 1234);
  u->on = true;
  std::vector<float> buf(static_cast<std::size_t>(block_frames) * 2);
  FxCtx cx;
  cx.sr = sample_rate;
  cx.bf = 60.0 / 120.0 * sample_rate;
  std::uint32_t r = 1;
  double sum = 0;
  for (int b = 0; b < blocks + 8; ++b) {
    for (std::size_t i = 0; i < buf.size(); ++i) {
      r = r * 1664525u + 1013904223u;
      buf[i] = 0.3f * (static_cast<float>(r >> 9) / 4194304.0f - 1.0f);
    }
    const auto t0 = std::chrono::steady_clock::now();
    run_unit(*u, cx, buf.data(), block_frames);
    const auto t1 = std::chrono::steady_clock::now();
    cx.beat_pos += block_frames / cx.bf;
    if (b >= 8) sum += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  }
  delete u;
  return sum / std::max(1, blocks);
}

}  // namespace deckhand
