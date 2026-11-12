// Decoder checks for the Windows backend. Self-contained PCM files (WAV, AIFF, AIFC) are written to a temp folder
// and read by the built-in parsers; the Media Foundation path (MP3, AAC/M4A, FLAC, ...) is checked when
// DECKHAND_TEST_MEDIA names a folder holding test.{mp3,m4a,flac,wav,aiff} (3 s, 44.1 kHz; title "Tést ☃",
// artist "Ärtist", album "Albüm"). Set DECKHAND_TEST_NO_MF=1 where Media Foundation's codecs are absent (Wine):
// the compressed-format checks are then skipped and said to be.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "../../../tests/check.hpp"
#include "deckhand/audio/track.hpp"

namespace fs = std::filesystem;
using namespace deckhand;

namespace {

const double kPi = 3.14159265358979323846;

void le16(std::ofstream& f, std::uint16_t v) { f.put(char(v & 0xFF)).put(char(v >> 8)); }
void le32(std::ofstream& f, std::uint32_t v) { le16(f, v & 0xFFFF); le16(f, v >> 16); }
void be16(std::ofstream& f, std::uint16_t v) { f.put(char(v >> 8)).put(char(v & 0xFF)); }
void be32(std::ofstream& f, std::uint32_t v) { be16(f, v >> 16); be16(f, v & 0xFFFF); }

double wave(double hz, double amp, std::uint32_t rate, int i) { return amp * std::sin(2 * kPi * hz * i / rate); }
std::int16_t s16(double v) { return static_cast<std::int16_t>(std::lround(v * 32767.0)); }

void write_wav(const fs::path& p, std::uint32_t rate, int channels, int frames, int bits = 16, bool is_float = false,
               bool with_tags = false) {
  std::ofstream f(p, std::ios::binary);
  const int bps = bits / 8;
  const std::uint32_t bytes = static_cast<std::uint32_t>(frames * channels * bps);
  const std::string title = "T\xC3\xA9st";  // "Tést"
  const std::uint32_t list = with_tags ? 8 + 4 + 8 + (title.size() + (title.size() & 1)) + 8 + 6 : 0;
  f.write("RIFF", 4); le32(f, 4 + 8 + 16 + list + 8 + bytes); f.write("WAVE", 4);
  f.write("fmt ", 4); le32(f, 16); le16(f, is_float ? 3 : 1); le16(f, channels); le32(f, rate);
  le32(f, rate * channels * bps); le16(f, channels * bps); le16(f, bits);
  if (with_tags) {
    f.write("LIST", 4); le32(f, list - 8); f.write("INFO", 4);
    f.write("INAM", 4); le32(f, title.size()); f.write(title.data(), title.size()); if (title.size() & 1) f.put(0);
    f.write("IART", 4); le32(f, 6); f.write("Artist", 6);
  }
  f.write("data", 4); le32(f, bytes);
  for (int i = 0; i < frames; ++i) {
    for (int c = 0; c < channels; ++c) {
      const double v = c == 0 ? wave(440, 0.8, rate, i) : wave(220, 0.4, rate, i);
      if (is_float) { const float x = static_cast<float>(v); std::uint32_t u; std::memcpy(&u, &x, 4); le32(f, u); }
      else if (bits == 16) le16(f, static_cast<std::uint16_t>(s16(v)));
      else if (bits == 24) { const std::int32_t x = static_cast<std::int32_t>(std::lround(v * 8388607.0)); f.put(char(x & 0xFF)).put(char((x >> 8) & 0xFF)).put(char((x >> 16) & 0xFF)); }
      else if (bits == 8) f.put(static_cast<char>(static_cast<int>(std::lround(v * 127.0)) + 128));
    }
  }
}

// AIFF / AIFC, 16-bit stereo, 44.1 or 48 kHz. `compression`: nullptr = plain AIFF, "NONE" or "sowt" = AIFC.
void write_aiff(const fs::path& p, std::uint32_t rate, int frames, const char* compression = nullptr) {
  std::ofstream f(p, std::ios::binary);
  const std::uint32_t bytes = static_cast<std::uint32_t>(frames * 4);
  const bool aifc = compression != nullptr;
  const std::uint32_t comm = aifc ? 18 + 4 + 2 : 18;  // AIFC adds the type and an empty pascal string
  f.write("FORM", 4); be32(f, 4 + 8 + comm + 8 + 8 + bytes); f.write(aifc ? "AIFC" : "AIFF", 4);
  f.write("COMM", 4); be32(f, comm); be16(f, 2); be32(f, frames); be16(f, 16);
  be16(f, 0x400E); be16(f, rate == 48000 ? 0xBB80 : 0xAC44); for (int i = 0; i < 6; ++i) f.put(0);  // 80-bit rate
  if (aifc) { f.write(compression, 4); f.put(0); f.put(0); }
  f.write("SSND", 4); be32(f, 8 + bytes); be32(f, 0); be32(f, 0);
  const bool little = aifc && std::string(compression) == "sowt";
  for (int i = 0; i < frames; ++i) {
    for (const std::int16_t v : {s16(wave(440, 0.8, rate, i)), s16(wave(220, 0.4, rate, i))}) {
      if (little) le16(f, static_cast<std::uint16_t>(v)); else be16(f, static_cast<std::uint16_t>(v));
    }
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
  CHECK(a && std::fabs(peak(*a, 0) - 0.8f) < 0.01f && std::fabs(peak(*a, 1) - 0.4f) < 0.01f);
  CHECK(a && !a->wave.empty());  // build_waveform ran

  // The file's own sample rate is kept (no resampling).
  write_wav(dir / "r48.wav", 48000, 2, 24000);
  a = decode_file((dir / "r48.wav").string(), err);
  CHECK(a && a->sample_rate == 48000 && a->frames == 24000);

  // Mono is duplicated to both channels at full level.
  write_wav(dir / "mono.wav", 44100, 1, 22050);
  a = decode_file((dir / "mono.wav").string(), err);
  CHECK(a && a->frames == 22050 && a->pcm[2 * 100] == a->pcm[2 * 100 + 1] && std::fabs(peak(*a, 0) - 0.8f) < 0.01f);

  // Sample formats: 24-bit, 8-bit, 32-bit float.
  write_wav(dir / "s24.wav", 44100, 2, 4410, 24);
  a = decode_file((dir / "s24.wav").string(), err);
  CHECK(a && a->frames == 4410 && std::fabs(peak(*a, 0) - 0.8f) < 0.001f && std::fabs(peak(*a, 1) - 0.4f) < 0.001f);
  write_wav(dir / "u8.wav", 44100, 2, 4410, 8);
  a = decode_file((dir / "u8.wav").string(), err);
  CHECK(a && std::fabs(peak(*a, 0) - 0.8f) < 0.02f);
  write_wav(dir / "f32.wav", 44100, 2, 4410, 32, true);
  a = decode_file((dir / "f32.wav").string(), err);
  CHECK(a && std::fabs(peak(*a, 0) - 0.8f) < 0.001f && std::fabs(peak(*a, 1) - 0.4f) < 0.001f);

  write_aiff(dir / "t.aiff", 44100, 22050);
  a = decode_file((dir / "t.aiff").string(), err);
  CHECK(a && a->sample_rate == 44100 && a->frames == 22050 && std::fabs(peak(*a, 1) - 0.4f) < 0.01f);
  write_aiff(dir / "t48.aiff", 48000, 4800);
  a = decode_file((dir / "t48.aiff").string(), err);
  CHECK(a && a->sample_rate == 48000 && a->frames == 4800);
  write_aiff(dir / "none.aifc", 44100, 4410, "NONE");
  a = decode_file((dir / "none.aifc").string(), err);
  CHECK(a && a->frames == 4410 && std::fabs(peak(*a, 0) - 0.8f) < 0.01f);
  write_aiff(dir / "sowt.aifc", 44100, 4410, "sowt");  // little-endian samples
  a = decode_file((dir / "sowt.aifc").string(), err);
  CHECK(a && a->frames == 4410 && std::fabs(peak(*a, 0) - 0.8f) < 0.01f && std::fabs(peak(*a, 1) - 0.4f) < 0.01f);

  // A folder with a non-ASCII name and one with spaces, '#' and '&', as real music folders have. The narrow path is
  // UTF-8 only under the UTF-8 manifest (activeCodePage), so it is built from the wide path the way the shell does.
  {
    const fs::path folder = dir / L"M\u00fcsik #1 & more";
    fs::create_directories(folder);
    fs::copy_file(dir / "st.wav", folder / L"Tr\u00e4ume.wav");
    CHECK(fs::exists(folder / L"Tr\u00e4ume.wav"));
    const std::wstring w = (folder / L"Tr\u00e4ume.wav").wstring();
    std::string utf8(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), utf8.data(), (int)utf8.size(), nullptr, nullptr);
    a = decode_file(utf8, err);
    CHECK(a && a->frames == 44100);
  }

  // Failures return nullptr with a message, never throw.
  err.clear();
  CHECK(!decode_file((dir / "missing.mp3").string(), err) && !err.empty());
  { std::ofstream(dir / "junk.mp3") << "this is not audio at all, just text"; }
  err.clear();
  CHECK(!decode_file((dir / "junk.mp3").string(), err) && !err.empty());
  { std::ofstream f(dir / "trunc.wav", std::ios::binary); f.write("RIFF\x10\0\0\0WAVEfmt ", 16); }  // header cut short
  err.clear();
  CHECK(!decode_file((dir / "trunc.wav").string(), err) && !err.empty());

  FileTags tags;
  CHECK(!read_file_tags((dir / "missing.mp3").string(), tags));
  CHECK(read_file_tags((dir / "st.wav").string(), tags) && std::llabs(tags.duration_ms - 1000) <= 5);
  write_wav(dir / "tagged.wav", 44100, 2, 44100, 16, false, true);
  tags = {};
  CHECK(read_file_tags((dir / "tagged.wav").string(), tags) && tags.title == "T\xC3\xA9st" && tags.artist == "Artist");

  if (std::getenv("DECKHAND_TEST_NO_MF")) std::puts("skip: Media Foundation checks (DECKHAND_TEST_NO_MF set)");
  if (const char* media = std::getenv("DECKHAND_TEST_MEDIA")) {
    for (const char* ext : {"mp3", "m4a", "flac", "wav", "aiff"}) {
      const std::string path = std::string(media) + "/test." + ext;
      if (!fs::exists(path)) {
        std::printf("skip %s (not present)\n", ext);
        continue;
      }
      const bool pcm = std::string(ext) == "wav" || std::string(ext) == "aiff";
      if (!pcm && std::getenv("DECKHAND_TEST_NO_MF")) {
        std::printf("skip %s (no Media Foundation codecs)\n", ext);
        continue;
      }
      a = decode_file(path, err);
      if (!a) std::fprintf(stderr, "%s: %s\n", ext, err.c_str());
      CHECK(a);
      const std::uint32_t rate = a->sample_rate;
      CHECK(rate == 44100 || rate == 48000);
      CHECK(std::llabs(static_cast<long long>(a->frames) - 3LL * rate) < rate / 10);  // codec padding: 3 s +- 100 ms
      CHECK(peak(*a, 0) > 0.1f && !a->wave.empty());
      FileTags t;
      CHECK(read_file_tags(path, t));
      CHECK(std::llabs(t.duration_ms - 3000) < 120);
      if (!pcm) CHECK(t.title == "T\xC3\xA9st \xE2\x98\x83" && t.artist == "\xC3\x84rtist" && t.album == "Alb\xC3\xBCm");
      std::printf("ok %s: %u Hz, %llu frames, \"%s\"\n", ext, rate, static_cast<unsigned long long>(a->frames), t.title.c_str());
    }
  }
  fs::remove_all(dir);
  std::puts("test_windows_decoder ok");
  return 0;
}
