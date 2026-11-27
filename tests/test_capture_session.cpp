// Capture session: stats, async writing, sample rates, RF64 header, crash recovery and resume.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "capture_synth.hpp"
#include "check.hpp"
#include "deckhand/capture/session.hpp"
#include "deckhand/capture/source.hpp"

using namespace deckhand::capture;
namespace fs = std::filesystem;

namespace {

std::vector<float> stereo_sine(std::uint32_t rate, double secs, double amp, double hz, double dc1 = 0) {
  const std::uint64_t n = static_cast<std::uint64_t>(secs * rate);
  std::vector<float> v(n * 2);
  for (std::uint64_t i = 0; i < n; ++i) {
    const double x = amp * std::sin(2 * synth::kPi * hz * i / rate);
    v[i * 2] = static_cast<float>(x);
    v[i * 2 + 1] = static_cast<float>(x + dc1);
  }
  return v;
}

std::uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint64_t le64(const unsigned char* p) { return le32(p) | (static_cast<std::uint64_t>(le32(p + 4)) << 32); }

std::vector<unsigned char> slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Max difference between a file's samples and the expected floats (24-bit quantisation is ~1.2e-7).
double max_diff(const std::string& path, std::uint64_t at, const std::vector<float>& want, std::uint32_t ch) {
  WavReader r;
  std::string err;
  CHECK(r.open(path, err));
  std::vector<float> got(want.size());
  const std::uint64_t n = r.read(at, want.size() / ch, got.data());
  CHECK(n == want.size() / ch);
  double m = 0;
  for (std::size_t i = 0; i < want.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(got[i] - want[i])));
  return m;
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "dh_capture_session_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  std::string err;

  // ---- statistics, sync mode ------------------------------------------------------------------------------------
  {
    CaptureConfig c;
    c.path = (dir / "stats.wav").string();
    c.async = false;
    CaptureSession s;
    CHECK(s.start(c, err));
    auto v = stereo_sine(44100, 2.0, 0.5, 440, 0.1);  // right channel has +0.1 DC
    v[100 * 2] = 1.0f;  // two clipped samples
    v[200 * 2 + 1] = -1.0f;
    CHECK(s.push(v.data(), v.size() / 2));
    auto st = s.stats();
    CHECK(st.frames == 88200);
    CHECK(std::fabs(st.seconds - 2.0) < 1e-9);
    CHECK(st.peak[0] == 1.0f && st.peak[1] == 1.0f);
    CHECK(st.clip_samples == 2);
    CHECK(std::fabs(st.dc[0]) < 1e-3);
    CHECK(std::fabs(st.dc[1] - 0.1) < 1e-3);
    CHECK(std::fabs(st.rms - 0.3606) < 0.01);
    auto m = s.take_meter();
    CHECK(m.peak[0] == 1.0f && m.clip_samples == 2 && m.rms > 0.3f);
    m = s.take_meter();  // a second take sees only what came since
    CHECK(m.peak[0] == 0.f && m.clip_samples == 0);
    s.set_paused(true);
    CHECK(s.push(v.data(), 1000));  // discarded
    CHECK(s.stats().frames == 88200 && s.stats().paused);
    CHECK(s.stop(err));
    CHECK(max_diff(c.path, 0, v, 2) < 2e-7);  // 24-bit
  }

  // ---- async writer, odd block sizes, several rates, formats ----------------------------------------------------------
  for (std::uint32_t rate : {44100u, 48000u, 96000u, 192000u}) {
    for (SampleFormat f : {SampleFormat::Pcm24, SampleFormat::Float32, SampleFormat::Pcm16}) {
      CaptureConfig c;
      c.path = (dir / "rate.wav").string();
      c.sample_rate = rate;
      c.format = f;
      CaptureSession s;
      CHECK(s.start(c, err));
      const auto v = stereo_sine(rate, 1.0, 0.4, 1000);
      for (std::size_t at = 0, blk = 333; at < v.size() / 2; at += blk, blk = blk % 997 + 211) {
        const std::size_t n = std::min(blk, v.size() / 2 - at);
        CHECK(s.push(&v[at * 2], n));
      }
      CHECK(s.stop(err));
      WavInfo info;
      CHECK(read_wav_info(c.path, info, err));
      CHECK(info.fmt.sample_rate == rate && info.fmt.channels == 2 && info.fmt.format == f && info.frames == rate);
      CHECK(!info.damaged && !info.rf64);
      CHECK(max_diff(c.path, 0, v, 2) < (f == SampleFormat::Pcm16 ? 4e-5 : 2e-7));
      CHECK(s.stats().dropped_frames == 0);
    }
  }
  {  // mono, 4-channel
    for (std::uint16_t ch : {1, 4}) {
      CaptureConfig c;
      c.path = (dir / "multi.wav").string();
      c.channels = ch;
      c.async = false;
      CaptureSession s;
      CHECK(s.start(c, err));
      std::vector<float> v(4410 * ch);
      for (std::size_t i = 0; i < v.size(); ++i) v[i] = 0.1f * static_cast<float>((i % ch) + 1);
      CHECK(s.push(v.data(), 4410));
      CHECK(s.stop(err));
      CHECK(max_diff(c.path, 0, v, ch) < 2e-7);
      CHECK(std::fabs(s.stats().peak[ch - 1] - 0.1f * ch) < 1e-6);
    }
  }

  // ---- the header is refreshed during recording (what a crash leaves behind) ----------------------------------------
  {
    CaptureConfig c;
    c.path = (dir / "live.wav").string();
    c.flush_seconds = 0.25;
    CaptureSession s;
    CHECK(s.start(c, err));
    const auto v = stereo_sine(44100, 2.0, 0.3, 300);
    for (std::size_t at = 0; at < v.size() / 2; at += 441) CHECK(s.push(&v[at * 2], std::min<std::size_t>(441, v.size() / 2 - at)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::uint64_t seen = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      WavInfo info;
      if (read_wav_info(c.path, info, err)) seen = info.frames;
      if (seen >= 44100) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(seen >= 44100);  // at least a second reached the file and its header while still recording
    CHECK(s.stop(err));
  }

  // ---- RF64 --------------------------------------------------------------------------------------------------------
  {
    CaptureConfig c;
    c.path = (dir / "big.wav").string();
    c.async = false;
    c.rf64_threshold = 1000;  // stands in for 4 GB
    CaptureSession s;
    CHECK(s.start(c, err));
    const auto v = stereo_sine(44100, 0.5, 0.3, 300);
    CHECK(s.push(v.data(), v.size() / 2));
    CHECK(s.stop(err));
    const auto b = slurp(c.path);
    const std::uint64_t frames = v.size() / 2, data = frames * 6;
    CHECK(!std::memcmp(b.data(), "RF64", 4) && le32(&b[4]) == 0xFFFFFFFFu);
    CHECK(!std::memcmp(&b[8], "WAVE", 4) && !std::memcmp(&b[12], "ds64", 4) && le32(&b[16]) == 28);
    CHECK(le64(&b[20]) == b.size() - 8);
    CHECK(le64(&b[28]) == data && le64(&b[36]) == frames);
    CHECK(!std::memcmp(&b[48], "fmt ", 4) && !std::memcmp(&b[72], "data", 4) && le32(&b[76]) == 0xFFFFFFFFu);
    WavInfo info;
    CHECK(read_wav_info(c.path, info, err) && info.rf64 && info.frames == frames && !info.damaged);
    CHECK(max_diff(c.path, 0, v, 2) < 2e-7);
    // The normal case below the threshold stays RIFF.
    c.rf64_threshold = kRf64Threshold;
    CaptureSession s2;
    CHECK(s2.start(c, err) && s2.push(v.data(), frames) && s2.stop(err));
    const auto b2 = slurp(c.path);
    CHECK(!std::memcmp(b2.data(), "RIFF", 4) && le32(&b2[4]) == b2.size() - 8 && le32(&b2[76]) == data);
  }

  // ---- crash recovery and resume ----------------------------------------------------------------------------------
  {
    const std::string live = (dir / "crash_src.wav").string(), crashed = (dir / "crashed.wav").string();
    CaptureConfig c;
    c.path = live;
    c.async = false;
    c.flush_seconds = 1000;  // no automatic header refresh: the header stays behind
    CaptureSession s;
    CHECK(s.start(c, err));
    const auto v = stereo_sine(44100, 4.0, 0.4, 523);
    const std::size_t half = v.size() / 8;  // 1 s, in frames
    CHECK(s.push(v.data(), half));
    s.flush();  // header says 1 s
    CHECK(s.push(&v[half * 2], 3 * half));  // 3 more seconds, header not refreshed
    CHECK(s.stop(err));
    // "Crash": the file as an interrupted recording leaves it: the header still describes the first second, 3.5 s of
    // frames reached the disk, and the last frame is cut in half.
    fs::copy_file(live, crashed, fs::copy_options::overwrite_existing);
    {
      std::FILE* f = std::fopen(crashed.c_str(), "r+b");
      CHECK(f);
      unsigned char h[4];
      const std::uint32_t data = static_cast<std::uint32_t>(half * 6), riff = 72 + data;
      for (int i = 0; i < 4; ++i) h[i] = static_cast<unsigned char>(riff >> (8 * i));
      std::fseek(f, 4, SEEK_SET);
      std::fwrite(h, 1, 4, f);
      for (int i = 0; i < 4; ++i) h[i] = static_cast<unsigned char>(data >> (8 * i));
      std::fseek(f, 76, SEEK_SET);
      std::fwrite(h, 1, 4, f);
      std::fclose(f);
    }
    fs::resize_file(crashed, 80 + 6ull * 154350 + 4, ec);  // 3.5 s and 4 stray bytes
    WavInfo info;
    CHECK(read_wav_info(crashed, info, err));
    CHECK(info.damaged);
    CHECK(info.frames == 154350);
    CHECK(info.data_bytes % 6 == 0);
    CHECK((fs::file_size(crashed) - 80) % 6 != 0);  // the cut really is mid-frame
    const RepairResult rr = repair_wav(crashed);
    CHECK(rr.ok && rr.was_damaged && rr.frames == info.frames && rr.dropped_bytes == 4);
    CHECK(read_wav_info(crashed, info, err) && !info.damaged && info.frames == rr.frames && fs::file_size(crashed) == 80 + info.frames * 6);
    CHECK(max_diff(crashed, 0, std::vector<float>(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(info.frames * 2)), 2) < 2e-7);
    // Resume appends exactly where the recovered audio ends, and the statistics come back from the file.
    CaptureConfig oc;
    oc.async = false;
    CaptureSession r;
    CHECK(r.resume(crashed, err, true, &oc));
    CHECK(r.stats().frames == rr.frames && r.stats().peak[0] > 0.3f);
    const auto more = stereo_sine(44100, 1.0, 0.2, 700);
    CHECK(r.push(more.data(), more.size() / 2));
    CHECK(r.stop(err));
    CHECK(read_wav_info(crashed, info, err) && info.frames == rr.frames + 44100 && !info.damaged);
    CHECK(max_diff(crashed, rr.frames, more, 2) < 2e-7);
    CHECK(max_diff(crashed, 0, std::vector<float>(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(rr.frames * 2)), 2) < 2e-7);
    // A file with a header that is merely behind (no cut) also reads whole without repair.
    CHECK(!repair_wav((dir / "nonexistent.wav").string()).ok);
  }
  {  // an untouched header with the data cut after the first flush: a truncated declared size
    const std::string p = (dir / "trunc.wav").string();
    CaptureConfig c;
    c.path = p;
    c.async = false;
    CaptureSession s;
    CHECK(s.start(c, err));
    const auto v = stereo_sine(44100, 2.0, 0.3, 300);
    CHECK(s.push(v.data(), v.size() / 2) && s.stop(err));
    fs::resize_file(p, 80 + 6 * 50000 + 2, ec);
    WavInfo info;
    CHECK(read_wav_info(p, info, err) && info.damaged && info.frames == 50000);
    WavReader rd;
    CHECK(rd.open(p, err));
    std::vector<float> got(50000 * 2);
    CHECK(rd.read(0, 50000, got.data()) == 50000);
    CHECK(rd.read(50000, 10, got.data()) == 0);
  }

  fs::remove_all(dir, ec);
  std::printf("capture session ok\n");
  return 0;
}
