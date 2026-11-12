// Effect table and processors. Original implementations: delay lines, a Schroeder-style plate reverb,
// modulated delays, all-pass phaser, state-variable filter, two-tap delay-line pitch shifter, and so on.
#include "fx.hpp"

#include <algorithm>
#include <cmath>
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
enum EchoMode { kEcho, kPing, kSpiral, kLowCut, kDub };

void echo_run(FxUnit& u, const FxCtx& cx, float* buf, int n, int mode) {
  const K k = coefs(cx);
  const double maxd = static_cast<double>(u.cap - 8);
  double tgt = beat_time(u, cx);
  while (tgt > maxd) tgt *= 0.5;
  tgt = std::max(tgt, 8.0);
  if (u.time_s < 0) u.time_s = tgt;
  const float lp_a = lp_coef(mode == kDub ? 1800.0 : 6000.0, cx.sr);
  float peak = 0;
  for (int i = 0; i < n; ++i) {
    smooth(u, k);
    u.time_s += std::clamp(tgt - u.time_s, -0.25, 0.25);  // slewed: a time change glides like a tape echo
    float yl, yr;
    dl_read(u, 0, u.time_s, yl);
    dl_read(u, 1, u.time_s, yr);
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
    u.s[0] += lp_a * (yl - u.s[0]);
    u.s[1] += lp_a * (yr - u.s[1]);
    float pl = u.s[0], pr = u.s[1];
    if (hp_a > 0) {
      u.s[2] += hp_a * (pl - u.s[2]);
      u.s[3] += hp_a * (pr - u.s[3]);
      pl -= u.s[2];
      pr -= u.s[3];
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
void reverb_proc(FxUnit& u, const FxCtx& cx, float* buf, int n) {
  const K k = coefs(cx);
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
    const float decay = 0.45f + 0.5f * u.param_s;
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
void brake_run(FxUnit& u, const FxCtx& cx, float* buf, int n, bool spin) {
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
    const double v = spin ? -3.5 * std::exp(-u.d1 / (T / 3.0)) : std::max(0.0, 1.0 - u.d1 / T);
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

void reset_none(FxUnit&) {}
void reset_delay(FxUnit& u) { u.time_s = -1; }

// ---- registry ----------------------------------------------------------------------------------------
//        kind, name, param, synced, tail, release, colour, wet, param, beats
const FxDef kDefs[] = {
    {{EffectKind::None, "None", "", false, false, false, false, 0, 0, 1}, nullptr, reset_none, false},
    {{EffectKind::Echo, "Echo", "feedback", true, true, false, false, 0.5f, 0.5f, 0.5f}, echo_proc, reset_delay, false},
    {{EffectKind::PingPong, "Ping Pong", "feedback", true, true, false, false, 0.5f, 0.5f, 0.5f}, ping_proc, reset_delay, false},
    {{EffectKind::Spiral, "Spiral", "pitch drift", true, true, false, false, 0.5f, 0.5f, 0.75f}, spiral_proc, reset_delay, false},
    {{EffectKind::Reverb, "Reverb", "decay", false, true, false, false, 0.4f, 0.5f, 1.0f}, reverb_proc, reverb_reset, false},
    {{EffectKind::Flanger, "Flanger", "feedback", true, false, false, false, 1.0f, 0.7f, 4.0f}, flanger_proc, reset_none, false},
    {{EffectKind::Phaser, "Phaser", "feedback", true, false, false, false, 1.0f, 0.6f, 4.0f}, phaser_proc, reset_none, false},
    {{EffectKind::LowCutEcho, "Low Cut Echo", "cut frequency", true, true, false, false, 0.5f, 0.3f, 0.5f}, lowcut_proc, reset_delay, false},
    {{EffectKind::FilterSweep, "Filter Sweep", "resonance", true, false, false, false, 1.0f, 0.4f, 2.0f}, filter_sweep_proc, reset_none, false},
    {{EffectKind::Roll, "Roll", "", true, false, false, false, 1.0f, 0.5f, 0.25f}, roll_proc, reset_none, false},
    {{EffectKind::Transform, "Transform", "gate length", true, false, false, false, 1.0f, 0.5f, 0.25f}, transform_proc, reset_none, false},
    {{EffectKind::Pitch, "Pitch", "pitch (0.5 = none, +-12 st)", false, false, false, false, 1.0f, 0.75f, 1.0f}, pitch_proc, reset_none, false},
    {{EffectKind::Crusher, "Crusher", "crush amount", false, false, false, false, 1.0f, 0.5f, 1.0f}, crush_proc, reset_none, false},
    {{EffectKind::VinylBrake, "Vinyl Brake", "", true, false, true, false, 1.0f, 0.5f, 1.0f}, brake_proc, reset_none, false},
    {{EffectKind::Backspin, "Backspin", "", true, false, true, false, 1.0f, 0.5f, 1.0f}, spin_proc, reset_none, true},
    {{EffectKind::ColorNoise, "Noise", "", false, false, false, true, 1.0f, 0.5f, 1.0f}, noise_proc, reset_none, false},
    {{EffectKind::ColorDubEcho, "Dub Echo", "", true, true, false, true, 0.5f, 0.5f, 0.75f}, dub_proc, reset_delay, false},
    {{EffectKind::ColorCrush, "Crush", "", false, false, false, true, 1.0f, 0.5f, 1.0f}, crush_proc, reset_none, false},
    {{EffectKind::ColorPitch, "Pitch", "", false, false, false, true, 1.0f, 0.5f, 1.0f}, pitch_proc, reset_none, false},
    {{EffectKind::ColorSpace, "Space", "", false, true, false, true, 0.4f, 0.7f, 1.0f}, reverb_proc, reverb_reset, false},
};
static_assert(sizeof(kDefs) / sizeof(kDefs[0]) == static_cast<std::size_t>(EffectKind::Count), "one table row per EffectKind");

const FxDef& def_of(EffectKind k) {
  const std::size_t i = static_cast<std::size_t>(k);
  return kDefs[i < static_cast<std::size_t>(EffectKind::Count) ? i : 0];
}

bool unit_active(const FxUnit& u, const FxDef& d) {
  if (u.on || u.on_s > 1e-4f || d.always_run) return true;
  return d.info.tail && u.quiet < u.cap + 8192;
}

void run_unit(FxUnit& u, const FxCtx& cx, float* buf, int n) {
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

}  // namespace

int effect_count() { return static_cast<int>(EffectKind::Count) - 1; }
const EffectInfo& effect_info_at(int i) { return kDefs[1 + std::clamp(i, 0, effect_count() - 1)].info; }
const EffectInfo* effect_info(EffectKind k) {
  return static_cast<std::size_t>(k) < static_cast<std::size_t>(EffectKind::Count) ? &kDefs[static_cast<std::size_t>(k)].info : nullptr;
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
  prev_on = rise = false;
  def_of(kind).reset(*this);
}

void FxBus::prepare(double sr) {
  std::int64_t cap = 1;
  while (static_cast<double>(cap) < sr * 2.5) cap <<= 1;
  FxUnit* all[kFxSlots + 1];
  for (int i = 0; i < kFxSlots; ++i) all[i] = &slot[i];
  all[kFxSlots] = &colour;
  std::uint32_t seed = 0x9E3779B9u;
  for (FxUnit* u : all) {
    if (u->sr == sr && u->cap == cap && !u->mem.empty()) continue;
    u->sr = sr;
    u->cap = cap;
    u->acap = 16384;
    u->mem.assign(static_cast<std::size_t>(cap) * 2, 0.0f);
    u->aux.assign(static_cast<std::size_t>(u->acap) * 2, 0.0f);
    u->rng = seed += 0x6D2B79F5u;
    u->clear();
  }
}

void FxBus::select(int slot_index, EffectKind kind) {
  FxUnit& u = slot_index >= kFxSlots ? colour : slot[std::max(0, slot_index)];
  const FxDef& d = def_of(kind);
  if (kind != EffectKind::None && d.info.colour != (slot_index >= kFxSlots)) kind = EffectKind::None;
  const FxDef& dd = def_of(kind);
  u.kind = kind;
  u.on = false;
  u.wet = u.wet_s = dd.info.default_wet;
  u.param = u.param_s = dd.info.default_param;
  u.beats = dd.info.default_beats;
  u.seconds = 0;
  u.clear();
}

void FxBus::run_slots(float* buf, int n, const FxCtx& cx) {
  for (FxUnit& u : slot) run_unit(u, cx, buf, n);
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
  for (const FxUnit& u : slot) {
    if (u.kind == EffectKind::None) continue;
    const FxDef& d = def_of(u.kind);
    if (u.on || u.on_s > 1e-4f || (d.info.tail && u.quiet < u.cap + 8192)) return true;
  }
  if (colour.kind != EffectKind::None && (colour.on_s > 1e-4f || (def_of(colour.kind).info.tail && colour.quiet < colour.cap + 8192)))
    return true;
  return false;
}

void FxBus::view(FxSlotView* out) const {
  for (int i = 0; i < kFxSlots; ++i) {
    out[i].kind = slot[i].kind;
    out[i].on = slot[i].on;
    out[i].wet = slot[i].wet;
    out[i].param = slot[i].param;
    out[i].beats = slot[i].beats;
  }
}

}  // namespace deckhand
