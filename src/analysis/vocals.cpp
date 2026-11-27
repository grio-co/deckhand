// Vocal position detection (see vocals.hpp).
#include "deckhand/analysis/vocals.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include "deckhand/stems/cache.hpp"

namespace deckhand {

VocalDetector::VocalDetector(int sample_rate, const VocalOptions& options)
    : o_(options), rate_(std::max(8000, sample_rate)),
      frame_len_(std::max<std::size_t>(16, static_cast<std::size_t>(std::lround(std::max(5.0, options.frame_ms) * std::max(8000, sample_rate) / 1000.0)))) {}

void VocalDetector::add(const float* mono, std::size_t frames) { add_stereo(mono, nullptr, frames); }

void VocalDetector::add_stereo(const float* l, const float* r, std::size_t frames) {
  for (std::size_t i = 0; i < frames; ++i) {
    const double v = r ? 0.5 * (static_cast<double>(l[i]) + r[i]) : static_cast<double>(l[i]);
    acc_ += v * v;
    if (++in_frame_ == frame_len_) {
      const double rms = std::sqrt(acc_ / static_cast<double>(frame_len_));
      db_.push_back(static_cast<float>(20.0 * std::log10(std::max(rms, 1e-9))));
      acc_ = 0;
      in_frame_ = 0;
    }
  }
}

std::vector<VocalRegion> VocalDetector::finish() {
  std::vector<VocalRegion> out;
  if (in_frame_ > frame_len_ / 2) {  // a partial last frame counts when it is more than half full
    db_.push_back(static_cast<float>(20.0 * std::log10(std::max(std::sqrt(acc_ / static_cast<double>(in_frame_)), 1e-9))));
  }
  acc_ = 0;
  in_frame_ = 0;
  const std::size_t n = db_.size();
  if (n == 0) return out;
  const double fms = static_cast<double>(frame_len_) * 1000.0 / rate_;
  // The stem's loud level: the 95th percentile of the frame levels (a few clipped peaks do not set it).
  std::vector<float> sorted = db_;
  const std::size_t k = std::min(n - 1, static_cast<std::size_t>(0.95 * static_cast<double>(n)));
  std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k), sorted.end());
  const double loud = sorted[k];
  if (loud < o_.floor_db) return out;
  const double on = std::max(o_.floor_db, loud - o_.on_below_peak_db);
  const double off = on - std::max(0.0, o_.hysteresis_db);
  const std::size_t hold = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(o_.hold_ms / fms)));

  std::vector<VocalRegion> raw;
  bool active = false;
  std::size_t first = 0, last = 0;  // frames of the open region; last = the last frame that kept it open
  for (std::size_t i = 0; i < n; ++i) {
    if (!active) {
      if (db_[i] >= on) {
        active = true;
        first = last = i;
      }
    } else if (db_[i] >= off) {
      last = i;
    } else if (i - last >= hold) {
      raw.push_back({static_cast<double>(first) * fms, static_cast<double>(last + 1) * fms});
      active = false;
    }
  }
  if (active) raw.push_back({static_cast<double>(first) * fms, static_cast<double>(last + 1) * fms});

  const double total_ms = static_cast<double>(n) * fms;
  for (const VocalRegion& r : raw) {
    if (!out.empty() && r.start_ms - out.back().end_ms < o_.merge_gap_ms) out.back().end_ms = r.end_ms;
    else out.push_back(r);
  }
  std::erase_if(out, [&](const VocalRegion& r) { return r.end_ms - r.start_ms < o_.min_length_ms; });
  if (o_.pad_ms > 0) {
    for (VocalRegion& r : out) {
      r.start_ms = std::max(0.0, r.start_ms - o_.pad_ms);
      r.end_ms = std::min(total_ms, r.end_ms + o_.pad_ms);
    }
    std::vector<VocalRegion> merged;
    for (const VocalRegion& r : out) {
      if (!merged.empty() && r.start_ms <= merged.back().end_ms) merged.back().end_ms = std::max(merged.back().end_ms, r.end_ms);
      else merged.push_back(r);
    }
    out = std::move(merged);
  }
  return out;
}

std::vector<VocalRegion> detect_vocal_regions(const float* pcm, std::size_t frames, int channels, int sample_rate, const VocalOptions& options) {
  VocalDetector d(sample_rate, options);
  if (!pcm || channels < 1) return {};
  std::vector<float> mono(4096);
  for (std::size_t at = 0; at < frames;) {
    const std::size_t n = std::min(mono.size(), frames - at);
    for (std::size_t i = 0; i < n; ++i) {
      double s = 0;
      for (int c = 0; c < channels; ++c) s += pcm[(at + i) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
      mono[i] = static_cast<float>(s / channels);
    }
    d.add(mono.data(), n);
    at += n;
  }
  return d.finish();
}

bool detect_vocals_in_cache(const std::string& cache_root, const std::string& key, std::vector<VocalRegion>& out, std::string* error,
                            const VocalOptions& options) {
  out.clear();
  stems::CacheConfig cfg;
  cfg.root = cache_root;
  cfg.cap_bytes = 0;  // reading only: never evict
  stems::StemCache cache(cfg);
  std::string err;
  std::unique_ptr<stems::StemReader> rd = cache.open(key, &err);
  if (!rd) {
    if (error) *error = err.empty() ? "no cached stems for this track" : err;
    return false;
  }
  VocalDetector det(rd->sample_rate(), options);
  constexpr std::size_t kChunk = 1 << 16;
  std::vector<float> l(kChunk), r(kChunk);
  for (std::uint64_t at = 0; at < rd->frames(); at += kChunk) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, rd->frames() - at));
    if (!rd->read(stems::Vocals, 0, at, n, l.data()) || !rd->read(stems::Vocals, 1, at, n, r.data())) {
      if (error) *error = "the cached stems could not be read";
      return false;
    }
    det.add_stereo(l.data(), r.data(), n);
  }
  out = det.finish();
  return true;
}

}  // namespace deckhand
