#include "deckhand/capture/session.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "deckhand/capture/source.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand::capture {
namespace {
double to_db(double lin) { return lin > 1e-10 ? 20.0 * std::log10(lin) : -200.0; }
}

bool CaptureSession::open_common(std::string& error) {
  const std::size_t ch = cfg_.channels;
  st_ = CaptureStats{};
  st_.peak.assign(ch, 0.f);
  st_.dc.assign(ch, 0.0);
  sum_.assign(ch, 0.0);
  sum_sq_ = 0;
  meter_ = CaptureMeter{};
  meter_.peak.assign(ch, 0.f);
  meter_sq_ = 0;
  meter_frames_ = 0;
  since_flush_ = 0;
  head_ = tail_ = dropped_ = 0;
  stopping_ = false;
  flush_req_ = false;
  paused_ = false;
  if (cfg_.async) {
    cap_ = 1;
    while (cap_ < static_cast<std::uint64_t>(cfg_.sample_rate * std::max(1.0, cfg_.ring_seconds))) cap_ <<= 1;
    ring_.assign(static_cast<std::size_t>(cap_ * ch), 0.f);
    thread_ = std::thread([this] { run(); });
  }
  open_ = true;
  (void)error;
  return true;
}

bool CaptureSession::start(const CaptureConfig& cfg, std::string& error) {
  if (open_) { error = "already recording"; return false; }
  if (cfg.channels == 0 || cfg.channels > 16 || cfg.sample_rate < 8000) { error = "bad format"; return false; }
  cfg_ = cfg;
  WavFormat f;
  f.sample_rate = cfg.sample_rate;
  f.channels = cfg.channels;
  f.format = cfg.format;
  if (!writer_.create(cfg.path, f, error, cfg.rf64_threshold)) return false;
  return open_common(error);
}

bool CaptureSession::resume(const std::string& path, std::string& error, bool rescan, const CaptureConfig* options) {
  if (open_) { error = "already recording"; return false; }
  CaptureConfig cfg = options ? *options : CaptureConfig{};
  cfg.path = path;
  struct Scan { std::vector<float> peak; std::vector<double> sum; double sq = 0; std::uint64_t clip = 0, frames = 0; } scan;
  WavInfo info;
  if (!read_wav_info(path, info, error)) return false;
  cfg.sample_rate = info.fmt.sample_rate;
  cfg.channels = info.fmt.channels;
  cfg.format = info.fmt.format;
  const std::size_t ch = cfg.channels;
  scan.peak.assign(ch, 0.f);
  scan.sum.assign(ch, 0.0);
  if (rescan && info.frames > 0) {
    WavReader r;
    if (!r.open(path, error)) return false;
    std::vector<float> buf(8192 * ch);
    for (std::uint64_t at = 0; at < info.frames;) {
      const std::uint64_t n = r.read(at, 8192, buf.data());
      if (n == 0) break;
      for (std::uint64_t i = 0; i < n; ++i)
        for (std::size_t c = 0; c < ch; ++c) {
          const float x = buf[i * ch + c];
          const float a = std::fabs(x);
          scan.peak[c] = std::max(scan.peak[c], a);
          scan.sum[c] += x;
          scan.sq += static_cast<double>(x) * x;
          if (a >= cfg.clip_level) ++scan.clip;
        }
      at += n;
      scan.frames += n;
    }
    r.close();
  }
  if (!writer_.open_append(path, error, cfg.rf64_threshold)) return false;
  cfg_ = cfg;
  if (!open_common(error)) return false;
  std::lock_guard<std::mutex> lk(mu_);
  if (rescan) {
    st_.peak = scan.peak;
    sum_ = scan.sum;
    sum_sq_ = scan.sq;
    st_.clip_samples = scan.clip;
  }
  st_.frames = writer_.frames();
  return true;
}

bool CaptureSession::push(const float* in, std::uint64_t frames) {
  if (!open_ || frames == 0 || paused_.load(std::memory_order_relaxed)) return true;
  if (!cfg_.async) {
    consume(in, frames);
    return true;
  }
  const std::size_t ch = cfg_.channels;
  const std::uint64_t h = head_.load(std::memory_order_relaxed), t = tail_.load(std::memory_order_acquire);
  const std::uint64_t n = std::min(frames, cap_ - (h - t));
  const std::uint64_t at = h & (cap_ - 1), first = std::min(n, cap_ - at);
  std::memcpy(&ring_[at * ch], in, static_cast<std::size_t>(first * ch * sizeof(float)));
  if (n > first) std::memcpy(&ring_[0], in + first * ch, static_cast<std::size_t>((n - first) * ch * sizeof(float)));
  head_.store(h + n, std::memory_order_release);
  if (n < frames) {
    dropped_.fetch_add(frames - n, std::memory_order_relaxed);
    return false;
  }
  return true;
}

std::uint64_t CaptureSession::pop(float* out, std::uint64_t frames) {
  const std::size_t ch = cfg_.channels;
  const std::uint64_t t = tail_.load(std::memory_order_relaxed), h = head_.load(std::memory_order_acquire);
  const std::uint64_t n = std::min(frames, h - t);
  const std::uint64_t at = t & (cap_ - 1), first = std::min(n, cap_ - at);
  std::memcpy(out, &ring_[at * ch], static_cast<std::size_t>(first * ch * sizeof(float)));
  if (n > first) std::memcpy(out + first * ch, &ring_[0], static_cast<std::size_t>((n - first) * ch * sizeof(float)));
  tail_.store(t + n, std::memory_order_release);
  return n;
}

void CaptureSession::consume(const float* b, std::uint64_t frames) {
  const std::size_t ch = cfg_.channels;
  const float clip = static_cast<float>(cfg_.clip_level);
  std::lock_guard<std::mutex> lk(mu_);
  for (std::uint64_t i = 0; i < frames; ++i)
    for (std::size_t c = 0; c < ch; ++c) {
      const float x = b[i * ch + c];
      const float a = std::fabs(x);
      st_.peak[c] = std::max(st_.peak[c], a);
      meter_.peak[c] = std::max(meter_.peak[c], a);
      sum_[c] += x;
      const double sq = static_cast<double>(x) * x;
      sum_sq_ += sq;
      meter_sq_ += sq;
      if (a >= clip) { ++st_.clip_samples; ++meter_.clip_samples; }
    }
  meter_frames_ += frames;
  if (!writer_.write(b, frames)) st_.write_error = true;
  st_.frames = writer_.frames();
  since_flush_ += frames;
  if (since_flush_ >= static_cast<std::uint64_t>(cfg_.flush_seconds * cfg_.sample_rate) || flush_req_.exchange(false)) {
    since_flush_ = 0;
    if (!writer_.flush()) st_.write_error = true;
  }
}

void CaptureSession::run() {
  const std::size_t ch = cfg_.channels;
  std::vector<float> blk(4096 * ch);
  for (;;) {
    const std::uint64_t n = pop(blk.data(), 4096);
    if (n > 0) {
      consume(blk.data(), n);
      continue;
    }
    if (stopping_.load(std::memory_order_acquire) && head_.load(std::memory_order_acquire) == tail_.load()) break;
    if (flush_req_.exchange(false)) {
      std::lock_guard<std::mutex> lk(mu_);
      if (!writer_.flush()) st_.write_error = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
}

void CaptureSession::flush() {
  if (!open_) return;
  if (cfg_.async) {
    flush_req_ = true;
  } else {
    std::lock_guard<std::mutex> lk(mu_);
    if (!writer_.flush()) st_.write_error = true;
  }
}

bool CaptureSession::stop(std::string& error) {
  if (!open_) return true;
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
  open_ = false;
  const bool ok = writer_.close();
  if (!ok) error = "could not finish " + cfg_.path;
  return ok && !st_.write_error;
}

CaptureStats CaptureSession::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  CaptureStats s = st_;
  s.seconds = cfg_.sample_rate ? static_cast<double>(s.frames) / cfg_.sample_rate : 0;
  const double n = static_cast<double>(s.frames) * cfg_.channels;
  s.rms = n > 0 ? std::sqrt(sum_sq_ / n) : 0;
  s.rms_db = to_db(s.rms);
  s.peak_all = 0;
  for (float p : s.peak) s.peak_all = std::max(s.peak_all, p);
  s.peak_db = to_db(s.peak_all);
  for (std::size_t c = 0; c < s.dc.size(); ++c) s.dc[c] = s.frames ? sum_[c] / static_cast<double>(s.frames) : 0;
  s.dropped_frames = dropped_.load();
  s.paused = paused_.load();
  return s;
}

CaptureMeter CaptureSession::take_meter() {
  std::lock_guard<std::mutex> lk(mu_);
  CaptureMeter m = meter_;
  const double n = static_cast<double>(meter_frames_) * cfg_.channels;
  m.rms = n > 0 ? static_cast<float>(std::sqrt(meter_sq_ / n)) : 0.f;
  std::fill(meter_.peak.begin(), meter_.peak.end(), 0.f);
  meter_.clip_samples = 0;
  meter_sq_ = 0;
  meter_frames_ = 0;
  return m;
}

}  // namespace deckhand::capture
