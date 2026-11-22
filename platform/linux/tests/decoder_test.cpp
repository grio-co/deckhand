// Decoder checks for the FFmpeg backend. Self-contained WAV/AIFF files are written to a temp folder; if
// DECKHAND_TEST_MEDIA names a folder holding test.{mp3,m4a,flac,ogg,opus,wav,aiff} (made by
// scripts/linux/smoke.sh with ffmpeg: 3 s, 44.1 kHz, title "Tést ☃", artist "Ärtist", album "Albüm") those are checked too.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include "../../../tests/check.hpp"
#include "deckhand/audio/track.hpp"

namespace fs = std::filesystem;
using namespace deckhand;

namespace {

void le16(std::ofstream& f, std::uint16_t v) { f.put(char(v & 0xFF)).put(char(v >> 8)); }
void le32(std::ofstream& f, std::uint32_t v) { le16(f, v & 0xFFFF); le16(f, v >> 16); }
void be16(std::ofstream& f, std::uint16_t v) { f.put(char(v >> 8)).put(char(v & 0xFF)); }

std::int16_t sample(double hz, double amp, std::uint32_t rate, int i) {
  return static_cast<std::int16_t>(std::lround(amp * 32767.0 * std::sin(2 * M_PI * hz * i / rate)));
}

void write_wav(const fs::path& p, std::uint32_t rate, int channels, int frames) {
  std::ofstream f(p, std::ios::binary);
  const std::uint32_t bytes = static_cast<std::uint32_t>(frames * channels * 2);
  f.write("RIFF", 4); le32(f, 36 + bytes); f.write("WAVEfmt ", 8); le32(f, 16); le16(f, 1); le16(f, channels);
  le32(f, rate); le32(f, rate * channels * 2); le16(f, channels * 2); le16(f, 16); f.write("data", 4); le32(f, bytes);
  for (int i = 0; i < frames; ++i) {
    le16(f, static_cast<std::uint16_t>(sample(440, 0.8, rate, i)));
    if (channels == 2) le16(f, static_cast<std::uint16_t>(sample(220, 0.4, rate, i)));
  }
}

void write_aiff(const fs::path& p, std::uint32_t rate, int frames) {  // 16-bit stereo, 44.1 or 48 kHz
  std::ofstream f(p, std::ios::binary);
  const std::uint32_t bytes = static_cast<std::uint32_t>(frames * 4);
  auto b32 = [&](std::uint32_t v) { be16(f, v >> 16); be16(f, v & 0xFFFF); };
  f.write("FORM", 4); b32(4 + 8 + 18 + 8 + 8 + bytes); f.write("AIFFCOMM", 8); b32(18); be16(f, 2); b32(frames);
  be16(f, 16);
  // 80-bit extended sample rate for 44100 (0x400E AC44 0...) and 48000 (0x400E BB80 0...)
  be16(f, 0x400E); be16(f, rate == 48000 ? 0xBB80 : 0xAC44); for (int i = 0; i < 6; ++i) f.put(0);
  f.write("SSND", 4); b32(8 + bytes); b32(0); b32(0);
  for (int i = 0; i < frames; ++i) {
    be16(f, static_cast<std::uint16_t>(sample(440, 0.8, rate, i)));
    be16(f, static_cast<std::uint16_t>(sample(220, 0.4, rate, i)));
  }
}

float peak(const TrackAudio& a, int channel) {
  float m = 0;
  for (std::size_t i = static_cast<std::size_t>(channel); i < a.pcm.size(); i += 2) m = std::fmax(m, std::fabs(a.pcm[i]));
  return m;
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / ("dh_decoder_test_" + std::to_string(std::rand()));
  fs::create_directories(dir);
  std::string err;

  // Stereo WAV: exact frame count and rate, channels not swapped, levels preserved.
  write_wav(dir / "st.wav", 44100, 2, 44100);
  auto a = decode_file((dir / "st.wav").string(), err);
  CHECK(a && a->sample_rate == 44100 && a->frames == 44100 && a->pcm.size() == 88200);
  CHECK(std::fabs(peak(*a, 0) - 0.8f) < 0.01f && std::fabs(peak(*a, 1) - 0.4f) < 0.01f);
  CHECK(!a->wave.empty());  // build_waveform ran

  // The file's own sample rate is kept (no resampling).
  write_wav(dir / "r48.wav", 48000, 2, 24000);
  a = decode_file((dir / "r48.wav").string(), err);
  CHECK(a && a->sample_rate == 48000 && a->frames == 24000);

  // Mono is duplicated to both channels at full level.
  write_wav(dir / "mono.wav", 44100, 1, 22050);
  a = decode_file((dir / "mono.wav").string(), err);
  CHECK(a && a->frames == 22050 && a->pcm[2 * 100] == a->pcm[2 * 100 + 1] && std::fabs(peak(*a, 0) - 0.8f) < 0.01f);

  write_aiff(dir / "t.aiff", 44100, 22050);
  a = decode_file((dir / "t.aiff").string(), err);
  CHECK(a && a->sample_rate == 44100 && a->frames == 22050 && std::fabs(peak(*a, 1) - 0.4f) < 0.01f);

  // Paths with a colon are files, not protocols.
  fs::copy_file(dir / "st.wav", dir / "a:b.wav");
  CHECK(decode_file((dir / "a:b.wav").string(), err));

  // Failures return nullptr with a message, never throw.
  err.clear();
  CHECK(!decode_file((dir / "missing.mp3").string(), err) && !err.empty());
  { std::ofstream(dir / "junk.mp3") << "this is not audio at all, just text"; }
  err.clear();
  CHECK(!decode_file((dir / "junk.mp3").string(), err) && !err.empty());
  FileTags tags;
  CHECK(!read_file_tags((dir / "missing.mp3").string(), tags));
  CHECK(read_file_tags((dir / "st.wav").string(), tags) && std::llabs(tags.duration_ms - 1000) <= 5);

  if (const char* media = std::getenv("DECKHAND_TEST_MEDIA")) {
    for (const char* ext : {"mp3", "m4a", "flac", "ogg", "opus", "wav", "aiff"}) {
      const std::string path = std::string(media) + "/test." + ext;
      if (!fs::exists(path)) {
        std::printf("skip %s (not present)\n", ext);
        continue;
      }
      a = decode_file(path, err);
      if (!a) std::fprintf(stderr, "%s: %s\n", ext, err.c_str());
      CHECK(a);
      const std::uint32_t rate = a->sample_rate;
      // Lossy codecs pad by up to a frame or two of encoder delay; 3 s +- 50 ms.
      CHECK(rate == 44100 || rate == 48000);
      CHECK(std::llabs(static_cast<long long>(a->frames) - 3LL * rate) < rate / 20);
      CHECK(peak(*a, 0) > 0.1f && !a->wave.empty());
      FileTags t;
      CHECK(read_file_tags(path, t));
      CHECK(std::llabs(t.duration_ms - 3000) < 60);
      if (std::string(ext) != "wav" && std::string(ext) != "aiff") {
        CHECK(t.title == "T\xC3\xA9st \xE2\x98\x83");  // "Tést ☃"
        CHECK(t.artist == "\xC3\x84rtist" && t.album == "Alb\xC3\xBCm");
      }
      std::printf("ok %s: %u Hz, %llu frames, \"%s\"\n", ext, rate, static_cast<unsigned long long>(a->frames), t.title.c_str());
    }
  }
  fs::remove_all(dir);
  std::puts("test_linux_decoder ok");
  return 0;
}
