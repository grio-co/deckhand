// The exporter reads ".dhstems" files with its own parser (it must not depend on the stems module). This test writes
// the stems through the real StemCache and exports from that file, so the two layouts cannot drift apart: what the
// stem player (StemReader) hears is what lands on the stick. Links dh_stems; only this test does.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#include "check.hpp"
#include "deckhand/audio/track.hpp"
#include "deckhand/export/export.hpp"
#include "deckhand/export/export_check.hpp"
#include "deckhand/export/export_preflight.hpp"
#include "deckhand/stems/cache.hpp"
#include "stem_wav.hpp"  // src/export (internal)

#if defined(_WIN32)
#include <process.h>
#define DH_PID _getpid()
#else
#include <unistd.h>
#define DH_PID getpid()
#endif

namespace fs = std::filesystem;
namespace ue = deckhand::usbexport;
namespace st = deckhand::stems;

namespace {

constexpr int kRate = 44100;
constexpr std::size_t kFrames = 2 * kRate;

fs::path unique_dir(const char* tag) {
  static std::atomic<int> n{0};
  const fs::path p = fs::temp_directory_path() / ("dh_stemcache_" + std::string(tag) + "_" + std::to_string(DH_PID) + "_" +
                                                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(n++));
  fs::create_directories(p);
  return p;
}

// Distinct, non-trivial content per stem, per channel; peaks stay below 1 so the cache's scale is not 1.
st::Stems make_stems() {
  st::Stems s;
  for (int k = 0; k < st::kStemCount; ++k) {
    s[static_cast<std::size_t>(k)].sample_rate = kRate;
    auto& a = s[static_cast<std::size_t>(k)];
    a.l.resize(kFrames);
    a.r.resize(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
      const double t = static_cast<double>(i);
      a.l[i] = static_cast<float>(0.2 * std::sin(t * (0.01 + 0.013 * k)) + 0.05 * k * std::cos(t * 0.31));
      a.r[i] = static_cast<float>(0.15 * std::sin(t * (0.017 + 0.011 * k) + 1.0) - 0.03 * k);
    }
  }
  return s;
}

struct Wav {
  int bits = 0, channels = 0, rate = 0;
  std::uint64_t frames = 0;
  std::vector<std::int32_t> l, r;
};
Wav read_wav(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  CHECK(f.good());
  std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(d.size() >= 44 && std::memcmp(d.data(), "RIFF", 4) == 0);
  Wav w;
  w.channels = d[22];
  w.rate = d[24] | (d[25] << 8) | (d[26] << 16);
  w.bits = d[34];
  const int bps = w.bits / 8;
  w.frames = (d.size() - 44) / (2 * static_cast<std::size_t>(bps));
  for (std::uint64_t i = 0; i < w.frames; ++i)
    for (int c = 0; c < 2; ++c) {
      const std::size_t o = 44 + (i * 2 + static_cast<std::uint64_t>(c)) * static_cast<std::size_t>(bps);
      const std::int32_t v = bps == 3 ? static_cast<std::int32_t>((std::uint32_t(d[o]) << 8 | std::uint32_t(d[o + 1]) << 16 | std::uint32_t(d[o + 2]) << 24)) >> 8
                                      : static_cast<std::int16_t>(d[o] | (d[o + 1] << 8));
      (c == 0 ? w.l : w.r).push_back(v);
    }
  return w;
}

void run(int cache_bits, int export_bits) {
  const fs::path root = unique_dir("run");
  const fs::path src = root / "src", stick = root / "stick", cache_dir = root / "cache";
  fs::create_directories(src);
  st::CacheConfig cc;
  cc.root = cache_dir;
  cc.bits = cache_bits;
  st::StemCache cache(cc);
  const std::string key(64, 'b');
  const st::Stems stems = make_stems();
  std::string err;
  CHECK(cache.store(key, stems, &err));
  CHECK(cache.has_stems(key));

  ue::ExportTrack t;
  t.id = 1;
  t.source_path = (src / "song.mp3").string();
  std::ofstream(t.source_path, std::ios::binary) << "audio";
  t.title = "Song";
  t.artist = "Artist";
  t.album = "Album";
  t.bpm = 120;
  t.duration_ms = 2000;
  t.sample_rate = kRate;
  t.bitrate = 320;
  for (int b = 0; b < 4; ++b) t.beats.push_back({std::int64_t(b) * 500, 120.0, b % 4 + 1});
  t.waveform.assign(300, ue::WaveSample{5, 6, 7});

  ue::ExportOptions o;
  o.destination = stick.string();
  o.stems.mixes = ue::kMixesAcapellaInstrumental;
  o.stems.bits = export_bits;
  const std::string file = cache.path_for(key).string();
  o.stems.lookup = [&](const ue::ExportTrack&) { return file; };
  ue::ExportResult res;
  CHECK(ue::export_usb({t}, {}, o, err, res));
  CHECK(res.stems.tracks == 2 && res.stems.no_stems.empty());
  ue::ValidationReport vr;
  CHECK(ue::validate_export(stick.string(), vr) && vr.ok());
  ue::ReadExport rd;
  CHECK(ue::read_export(stick.string(), rd, err));
  CHECK(rd.tracks.size() == 3);

  // What the stem player hears, summed from the reader the application uses.
  std::unique_ptr<st::StemReader> reader = cache.open(key, &err);
  CHECK(reader && reader->frames() == kFrames && reader->sample_rate() == kRate);
  std::vector<float> ref[4][2];
  for (int s = 0; s < 4; ++s)
    for (int c = 0; c < 2; ++c) {
      ref[s][c].resize(kFrames);
      CHECK(reader->read(s, c, 0, kFrames, ref[s][c].data()));
    }
  for (const auto& rt : rd.tracks) {
    std::vector<int> in_mix;
    if (rt.track.title == "Song (Acapella)") in_mix = {0};
    else if (rt.track.title == "Song (Instrumental)") in_mix = {1, 2, 3};
    else continue;
    const Wav w = read_wav(fs::path(stick) / fs::path(rt.file_path.substr(1)));
    CHECK(w.bits == export_bits && w.channels == 2 && w.rate == kRate && w.frames == kFrames);
    const double full = export_bits == 24 ? 8388608.0 : 32768.0;
    double worst = 0;
    for (std::size_t i = 0; i < kFrames; ++i)
      for (int c = 0; c < 2; ++c) {
        double sum = 0;
        for (int s : in_mix) sum += ref[s][c][i];
        const double got = static_cast<double>((c == 0 ? w.l : w.r)[i]) / full;
        worst = std::max(worst, std::fabs(got - std::max(-1.0, std::min(1.0, sum))));
      }
    // within two least significant bits of the exported depth: both sides decode the same layout
    CHECK(worst <= 2.0 / full + 1e-9);
  }
  // The export's waveform of a stem mix equals the application's build_waveform on the same PCM.
  {
    ue::detail::StemFile sf;
    CHECK(sf.open(file, err));
    std::vector<double> l, r;
    CHECK(sf.read_mix(1u | 2u | 8u, 0, kFrames, l, r));
    deckhand::TrackAudio audio;
    audio.sample_rate = kRate;
    audio.frames = kFrames;
    audio.pcm.resize(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; ++i) {
      audio.pcm[i * 2] = static_cast<float>(l[i]);
      audio.pcm[i * 2 + 1] = static_cast<float>(r[i]);
    }
    deckhand::build_waveform(audio);
    ue::StemRender render;
    render.stems_file = file;
    render.stem_mask = 1u | 2u | 8u;
    render.bits = 24;
    render.frames = kFrames;
    render.sample_rate = kRate;
    std::vector<ue::WaveSample> wave;
    CHECK(ue::detail::stem_waveform(render, wave, err));
    CHECK(wave.size() == audio.wave.size() && !wave.empty());
    for (std::size_t i = 0; i < wave.size(); ++i)
      CHECK(wave[i].low == audio.wave[i].low && wave[i].mid == audio.wave[i].mid && wave[i].high == audio.wave[i].high);
  }
  reader.reset();
  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace

int main() {
  run(24, 24);
  run(16, 24);  // a 16-bit cache file exported as a 24-bit WAV
  run(24, 16);
  std::puts("test_export_stems_cache: ok");
  return 0;
}
