#include "stretch.hpp"

#include <algorithm>
#include <cmath>

#include "dsp.hpp"

namespace deckhand {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kAttack = 64;  // frames of fade-in for a grain that starts after a jump

inline float mono_at(const float* pcm, std::int64_t frames, std::int64_t i) {
  return (i < 0 || i >= frames) ? 0.0f : 0.5f * (pcm[i * 2] + pcm[i * 2 + 1]);
}

}  // namespace

void Stretcher::prepare(double sr) {
  hs_ = std::max(64, static_cast<int>(std::lround(sr * 0.0232)));  // grain = 2 * hs ~ 46 ms, 50 % overlap
  n_ = hs_ * 2;
  range_ = std::max(16, static_cast<int>(sr * 0.010));              // +-10 ms similarity search
  std::int64_t r = 1;
  while (r < hs_ * 4) r <<= 1;
  mask_ = r - 1;
  ring_.assign(static_cast<std::size_t>(r) * 2, 0.0f);
  acc_.assign(static_cast<std::size_t>(hs_) * 2, 0.0f);
  win_.resize(static_cast<std::size_t>(n_));
  for (int j = 0; j < n_; ++j) win_[static_cast<std::size_t>(j)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * j / n_));
  tgt_.assign(static_cast<std::size_t>(hs_ / 4 + 2), 0.0f);
  seg_.assign(static_cast<std::size_t>(2 * range_ + hs_ + 16), 0.0f);
  tgt2_.assign(static_cast<std::size_t>(hs_ / 2 + 2), 0.0f);
  reset();
}

void Stretcher::reset() {
  std::fill(ring_.begin(), ring_.end(), 0.0f);
  std::fill(acc_.begin(), acc_.end(), 0.0f);
  w_ = 0;
  rpos_ = 0;
  cont_ = 0;
  have_ = false;
  jump_ = true;
  keep_ = 0;
}

void Stretcher::flush() {
  if (ring_.empty()) return;
  const std::int64_t r0 = static_cast<std::int64_t>(std::ceil(rpos_));
  const int keep = static_cast<int>(std::clamp<std::int64_t>(w_ - r0, 0, 64));
  for (int k = 0; k < keep; ++k) {
    const float g = 1.0f - static_cast<float>(k + 1) / static_cast<float>(keep + 1);
    const std::size_t idx = static_cast<std::size_t>((r0 + k) & mask_) * 2;
    ring_[idx] *= g;
    ring_[idx + 1] *= g;
  }
  w_ = r0;
  keep_ = keep;
  std::fill(acc_.begin(), acc_.end(), 0.0f);
  have_ = false;
  jump_ = true;
}

// Similarity search: the grain start (in output frames, relative to `a`) whose first half best continues
// the previous grain. Coarse pass on a 4x-decimated mono signal, then a fine pass.
int Stretcher::search(const float* pcm, std::int64_t frames, double ratio, double a) {
  const int stride = 4;
  const int m = hs_ / stride;
  const bool unity = ratio == 1.0;
  const std::int64_t c0i = std::llround(cont_), b0 = std::llround(a) - range_;
  if (unity) {  // contiguous mono window: the search loops below then read plain floats
    const int len = 2 * range_ + hs_ + 8;
    for (int j = 0; j < len; ++j) seg_[static_cast<std::size_t>(j)] = mono_at(pcm, frames, b0 + j);
  }
  const auto cand = [&](int d, int k, int step) {
    return unity ? seg_[static_cast<std::size_t>(d + range_ + k * step)]
                 : mono_at(pcm, frames, static_cast<std::int64_t>(std::llround(a + d * ratio + k * step * ratio)));
  };
  const auto targ = [&](int k, int step) {
    return unity ? mono_at(pcm, frames, c0i + static_cast<std::int64_t>(k) * step)
                 : mono_at(pcm, frames, static_cast<std::int64_t>(std::llround(cont_ + k * step * ratio)));
  };
  double yy = 0;
  for (int k = 0; k < m; ++k) {
    const float y = targ(k, stride);
    tgt_[static_cast<std::size_t>(k)] = y;
    yy += static_cast<double>(y) * y;
  }
  if (yy < 1e-7 * m) return 0;  // silence: nothing to match

  int best = 0;
  double best_score = -1e30;
  for (int d = -range_; d <= range_; d += stride) {
    float xy = 0, xx = 0;
    for (int k = 0; k < m; ++k) {
      const float x = cand(d, k, stride);
      xy += x * tgt_[static_cast<std::size_t>(k)];
      xx += x * x;
    }
    const double score = xy / std::sqrt(static_cast<double>(xx) + 1e-9);
    if (score > best_score) {
      best_score = score;
      best = d;
    }
  }
  // Fine pass around the coarse winner on a 2x-decimated signal.
  const int m2 = hs_ / 2;
  for (int k = 0; k < m2; ++k) tgt2_[static_cast<std::size_t>(k)] = targ(k, 2);
  int fine = best;
  best_score = -1e30;
  for (int d = best - stride + 1; d < best + stride; ++d) {
    if (d < -range_ || d > range_) continue;
    float xy = 0, xx = 0;
    for (int k = 0; k < m2; ++k) {
      const float x = cand(d, k, 2);
      xy += x * tgt2_[static_cast<std::size_t>(k)];
      xx += x * x;
    }
    const double score = xy / std::sqrt(static_cast<double>(xx) + 1e-9);
    if (score > best_score) {
      best_score = score;
      fine = d;
    }
  }
  return fine;
}

// Builds the grain whose first half is added into the ring (w_ .. w_ + hs) and keeps its second half.
void Stretcher::produce(const float* pcm, std::int64_t frames, double ratio, double pos_w, double adv) {
  // Centre the grain on where the play position will be half a grain from now.
  const double a = pos_w + (adv - ratio) * hs_;
  int d = 0;
  if (have_ && !jump_) d = search(pcm, frames, ratio, a);
  const double start = a + d * ratio;
  const bool unity = ratio == 1.0;
  const std::int64_t i0 = static_cast<std::int64_t>(std::llround(start));

  for (int j = 0; j < n_; ++j) {
    float xl, xr;
    if (unity) {
      const std::int64_t k = i0 + j;
      if (k < 0 || k >= frames) {
        xl = xr = 0;
      } else {
        xl = pcm[k * 2];
        xr = pcm[k * 2 + 1];
      }
    } else {
      hermite_frame(pcm, frames, start + j * ratio, xl, xr);
    }
    float wgt = win_[static_cast<std::size_t>(j)];
    if (j < hs_ && jump_) wgt = std::min(1.0f, static_cast<float>(j) / kAttack);
    xl *= wgt;
    xr *= wgt;
    if (j < hs_) {
      const std::size_t idx = static_cast<std::size_t>((w_ + j) & mask_) * 2;
      const float bl = j < keep_ ? ring_[idx] : 0.0f, br = j < keep_ ? ring_[idx + 1] : 0.0f;
      ring_[idx] = bl + acc_[static_cast<std::size_t>(j) * 2] + xl;
      ring_[idx + 1] = br + acc_[static_cast<std::size_t>(j) * 2 + 1] + xr;
    } else {
      acc_[static_cast<std::size_t>(j - hs_) * 2] = xl;
      acc_[static_cast<std::size_t>(j - hs_) * 2 + 1] = xr;
    }
  }
  w_ += hs_;
  cont_ = start + hs_ * ratio;
  have_ = true;
  jump_ = false;
  keep_ = 0;
}

void Stretcher::next(const float* pcm, std::int64_t frames, double ratio, double pos, double v, double pitch,
                     float& l, float& r) {
  const double adv = v / pitch;
  while (w_ < static_cast<std::int64_t>(std::floor(rpos_)) + 4)
    produce(pcm, frames, ratio, pos + adv * (static_cast<double>(w_) - rpos_), adv);
  const std::int64_t i = static_cast<std::int64_t>(std::floor(rpos_));
  const double frac = rpos_ - static_cast<double>(i);
  if (frac == 0.0) {
    const std::size_t k = static_cast<std::size_t>(i & mask_) * 2;
    l = ring_[k];
    r = ring_[k + 1];
  } else {
    const float t = static_cast<float>(frac);
    float o[2];
    for (int c = 0; c < 2; ++c) {
      const auto at = [&](std::int64_t k) { return ring_[static_cast<std::size_t>(k & mask_) * 2 + static_cast<std::size_t>(c)]; };
      const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
      const float c1 = 0.5f * (x1 - xm1);
      const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
      const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
      o[c] = ((c3 * t + c2) * t + c1) * t + x0;
    }
    l = o[0];
    r = o[1];
  }
  rpos_ += pitch;
}

}  // namespace deckhand
