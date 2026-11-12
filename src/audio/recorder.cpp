#include "recorder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace deckhand {
namespace {

constexpr std::uint32_t kHeaderBytes = 80;
constexpr std::uint64_t kRf64Threshold = 4000000000ull;  // switch to RF64 before the 32-bit sizes overflow

void put32(unsigned char* p, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
void put64(unsigned char* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
void put16(unsigned char* p, std::uint16_t v) {
  p[0] = static_cast<unsigned char>(v);
  p[1] = static_cast<unsigned char>(v >> 8);
}

}  // namespace

// Layout: RIFF/RF64 header, a 28-byte JUNK (becomes ds64 for RF64), fmt, data. 80 bytes in all.
void Recorder::write_header(bool) {
  if (!file_) return;
  const std::uint64_t frames = frames_.load();
  const std::uint64_t data = frames * 6;
  unsigned char h[kHeaderBytes];
  std::memset(h, 0, sizeof(h));
  const bool rf64 = data > kRf64Threshold;
  std::memcpy(h, rf64 ? "RF64" : "RIFF", 4);
  put32(h + 4, rf64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(kHeaderBytes - 8 + data));
  std::memcpy(h + 8, "WAVE", 4);
  std::memcpy(h + 12, rf64 ? "ds64" : "JUNK", 4);
  put32(h + 16, 28);
  if (rf64) {
    put64(h + 20, kHeaderBytes - 8 + data);
    put64(h + 28, data);
    put64(h + 36, frames);
    put32(h + 44, 0);
  }
  std::memcpy(h + 48, "fmt ", 4);
  put32(h + 52, 16);
  put16(h + 56, 1);  // PCM
  put16(h + 58, 2);
  put32(h + 60, sr_);
  put32(h + 64, sr_ * 6);
  put16(h + 68, 6);
  put16(h + 70, 24);
  std::memcpy(h + 72, "data", 4);
  put32(h + 76, rf64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(data));
  fseeko(file_, 0, SEEK_SET);
  if (std::fwrite(h, 1, sizeof(h), file_) != sizeof(h)) error_ = true;
  fseeko(file_, 0, SEEK_END);
  std::fflush(file_);
}

bool Recorder::open(const std::string& path, std::uint32_t sample_rate, std::string& error) {
  const int s = state_.load();
  if (s == Armed || s == Recording || s == Stopping) {
    error = "already recording";
    return false;
  }
  close();  // joins a finished writer
  file_ = std::fopen(path.c_str(), "wb");
  if (!file_) {
    error = "could not create " + path;
    return false;
  }
  sr_ = sample_rate;
  frames_ = 0;
  dropped_ = 0;
  error_ = false;
  finished_ = false;
  std::size_t cap = 1;
  while (cap < static_cast<std::size_t>(sample_rate) * 8) cap <<= 1;  // ~8 s of slack for slow disks
  ring_.init(cap);
  write_header(false);
  state_.store(Armed);
  thread_ = std::thread([this] { run(); });
  return true;
}

void Recorder::run() {
  std::vector<float> in(4096 * 2);
  std::vector<unsigned char> out(4096 * 6);
  std::uint64_t last_header = 0;
  for (;;) {
    const std::size_t n = ring_.pop(in.data(), 4096);
    if (n > 0) {
      for (std::size_t i = 0; i < n * 2; ++i) {
        const float x = std::clamp(in[i], -1.0f, 1.0f);
        const std::int32_t v = static_cast<std::int32_t>(std::lrintf(x * 8388607.0f));
        out[i * 3] = static_cast<unsigned char>(v);
        out[i * 3 + 1] = static_cast<unsigned char>(v >> 8);
        out[i * 3 + 2] = static_cast<unsigned char>(v >> 16);
      }
      if (std::fwrite(out.data(), 6, n, file_) != n) error_ = true;
      frames_.fetch_add(n);
      if (frames_.load() - last_header >= sr_) {  // about once per second of audio
        last_header = frames_.load();
        write_header(false);
      }
      continue;
    }
    if (state_.load(std::memory_order_acquire) == Stopping && ring_.size() == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  write_header(true);
  std::fclose(file_);
  file_ = nullptr;
  finished_ = true;
  state_.store(Done);
}

void Recorder::close() {
  if (thread_.joinable()) thread_.join();
  if (state_.load() == Done) state_.store(Idle);
}

void Recorder::abort() {
  int s = Recording;
  if (!state_.compare_exchange_strong(s, Stopping)) {
    s = Armed;
    state_.compare_exchange_strong(s, Stopping);
  }
  close();
}

}  // namespace deckhand
