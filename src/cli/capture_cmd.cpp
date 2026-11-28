// deckhandctl capture ...: offline tools for analogue-input recordings (no device is opened).
#include <cmath>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "deckhand/audio/track.hpp"
#include "deckhand/capture/export.hpp"
#include "deckhand/capture/session.hpp"
#include "deckhand/capture/split.hpp"

using namespace deckhand::capture;

namespace {

const char* opt(int argc, char** argv, const char* name, const char* fallback) {
  for (int i = 2; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], name)) return argv[i + 1];
  return fallback;
}

int usage_capture() {
  std::fprintf(stderr,
               "usage: deckhandctl capture <command>\n"
               "  info FILE                         format, length, peak, RMS, clip count and DC offset of a recording\n"
               "  repair FILE                       fix the header of a recording that was cut short (keeps every whole frame)\n"
               "  split FILE [--min-gap S] [--min-track S] [--min-depth DB] [--confidence C]   list the track boundaries found\n"
               "        [--export DIR] [--contiguous 1] [--format wav|aiff|flac] [--bits 16|24|32] [--artist A] [--album B] [--titles \"T1;T2;...\"]\n"
               "        [--dc 1] [--rumble HZ] [--declick 1] [--normalise LUFS] [--fade MS]   optional clean-up of the exported pieces\n"
               "  edit IN OUT [--start S] [--end S] [--fade-in MS] [--fade-out MS] [--normalise LUFS] [--format wav|aiff|flac] [--bits 16|24|32]\n"
               "                                    export an edited copy of a track; the original is never touched\n");
  return 2;
}

void fill_export(int argc, char** argv, ExportSettings& e) {
  e.container = !std::strcmp(opt(argc, argv, "--format", "wav"), "aiff") ? Container::Aiff : !std::strcmp(opt(argc, argv, "--format", "wav"), "flac") ? Container::Flac : Container::Wav;
  const int bits = std::atoi(opt(argc, argv, "--bits", "24"));
  e.format = bits == 16 ? SampleFormat::Pcm16 : bits == 32 ? SampleFormat::Float32 : SampleFormat::Pcm24;
  e.cleanup.remove_dc = !std::strcmp(opt(argc, argv, "--dc", "0"), "1");
  e.cleanup.click_repair = !std::strcmp(opt(argc, argv, "--declick", "0"), "1");
  if (std::strcmp(opt(argc, argv, "--rumble", ""), "")) {
    e.cleanup.rumble_filter = true;
    e.cleanup.rumble_hz = std::atof(opt(argc, argv, "--rumble", "40"));
  }
  if (std::strcmp(opt(argc, argv, "--normalise", ""), "")) {
    e.cleanup.normalise = true;
    e.cleanup.target_lufs = std::atof(opt(argc, argv, "--normalise", "-14"));
  }
  e.cleanup.fade_in_ms = e.cleanup.fade_out_ms = std::atof(opt(argc, argv, "--fade", "0"));
  if (std::strcmp(opt(argc, argv, "--fade-in", ""), "")) e.cleanup.fade_in_ms = std::atof(opt(argc, argv, "--fade-in", "0"));
  if (std::strcmp(opt(argc, argv, "--fade-out", ""), "")) e.cleanup.fade_out_ms = std::atof(opt(argc, argv, "--fade-out", "0"));
}

std::string stamp(double s) {
  char b[32];
  std::snprintf(b, sizeof(b), "%d:%06.3f", static_cast<int>(s / 60), s - 60.0 * static_cast<int>(s / 60));
  return b;
}

}  // namespace

int capture_command(int argc, char** argv) {
  if (argc < 4) return usage_capture();
  const std::string sub = argv[2], file = argv[3];
  std::string err;
  if (sub == "info") {
    WavInfo info;
    if (!read_wav_info(file, info, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    std::printf("%u Hz, %u channels, %u-bit%s, %llu frames (%s)%s%s\n", info.fmt.sample_rate, info.fmt.channels, bytes_per_sample(info.fmt.format) * 8,
                info.fmt.format == SampleFormat::Float32 ? " float" : "", static_cast<unsigned long long>(info.frames),
                stamp(static_cast<double>(info.frames) / info.fmt.sample_rate).c_str(), info.rf64 ? ", RF64" : "",
                info.damaged ? ", header out of date (run repair)" : "");
    WavReader r;
    if (!r.open(file, err)) return 1;
    const std::uint32_t ch = info.fmt.channels;
    std::vector<float> buf(8192 * ch), peak(ch, 0.f);
    std::vector<double> sum(ch, 0.0);
    double sq = 0;
    std::uint64_t clips = 0;
    for (std::uint64_t at = 0; at < info.frames;) {
      const std::uint64_t n = r.read(at, 8192, buf.data());
      if (!n) break;
      for (std::uint64_t i = 0; i < n; ++i)
        for (std::uint32_t c = 0; c < ch; ++c) {
          const float x = buf[i * ch + c], a = x < 0 ? -x : x;
          if (a > peak[c]) peak[c] = a;
          sum[c] += x;
          sq += static_cast<double>(x) * x;
          if (a >= 0.9999f) ++clips;
        }
      at += n;
    }
    const double frames = static_cast<double>(info.frames ? info.frames : 1);
    for (std::uint32_t c = 0; c < ch; ++c) std::printf("channel %u: peak %.4f  DC offset %+.6f\n", c, peak[c], sum[c] / frames);
    std::printf("RMS %.4f  clipped samples %llu\n", std::sqrt(sq / (frames * ch)), static_cast<unsigned long long>(clips));
    return 0;
  }
  if (sub == "repair") {
    const RepairResult r = repair_wav(file);
    if (!r.ok) { std::fprintf(stderr, "error: %s\n", r.error.c_str()); return 1; }
    std::printf("%s: %llu frames kept%s, %llu stray bytes dropped\n", file.c_str(), static_cast<unsigned long long>(r.frames),
                r.was_damaged ? " (header was out of date)" : "", static_cast<unsigned long long>(r.dropped_bytes));
    return 0;
  }
  if (sub == "split") {
    WavSource src;
    if (!src.open(file, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    SplitSettings ss;
    ss.min_gap_s = std::atof(opt(argc, argv, "--min-gap", "1.0"));
    ss.min_track_s = std::atof(opt(argc, argv, "--min-track", "20"));
    ss.min_depth_db = std::atof(opt(argc, argv, "--min-depth", "15"));
    ss.min_confidence = std::atof(opt(argc, argv, "--confidence", "0.25"));
    const SplitAnalysis an = analyse_splits(src, ss);
    const SplitLevels lv = split_levels(an, ss);
    std::printf("noise floor %.1f dB, programme %.1f dB%s\n", lv.floor_db, lv.programme_db, lv.splittable ? "" : "  (no gaps deep enough: continuous)");
    const auto pts = find_splits(an, ss);
    for (std::size_t i = 0; i < pts.size(); ++i)
      std::printf("split %zu  %s  confidence %.2f  gap %s..%s\n", i + 1, stamp(static_cast<double>(pts[i].frame) / an.sample_rate).c_str(), pts[i].confidence,
                  stamp(static_cast<double>(pts[i].gap_start) / an.sample_rate).c_str(), stamp(static_cast<double>(pts[i].gap_end) / an.sample_rate).c_str());
    const bool contiguous = !std::strcmp(opt(argc, argv, "--contiguous", "0"), "1");
    const auto pieces = plan_pieces(an, ss, pts, contiguous);
    int n = 0;
    for (const auto& p : pieces)
      if (!p.empty) std::printf("track %d  %s - %s\n", ++n, stamp(static_cast<double>(p.start) / an.sample_rate).c_str(), stamp(static_cast<double>(p.end) / an.sample_rate).c_str());
    const std::string dir = opt(argc, argv, "--export", "");
    if (dir.empty()) return 0;
    ExportSettings es;
    fill_export(argc, argv, es);
    std::vector<TrackTags> tags;
    const std::string titles = opt(argc, argv, "--titles", "");
    for (int i = 0; i < n; ++i) {
      TrackTags t;
      t.artist = opt(argc, argv, "--artist", "");
      t.album = opt(argc, argv, "--album", "");
      tags.push_back(t);
    }
    for (std::size_t at = 0, i = 0; at <= titles.size() && i < tags.size() && !titles.empty(); ++i) {
      const auto semi = titles.find(';', at);
      tags[i].title = titles.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
      if (semi == std::string::npos) break;
      at = semi + 1;
    }
    int bad = 0;
    for (const auto& r : export_pieces(src, pieces, tags, dir, es)) {
      std::printf("%s %s%s%s\n", r.ok ? "wrote" : "FAILED", r.path.c_str(), r.ok ? "" : ": ", r.error.c_str());
      bad += !r.ok;
    }
    return bad ? 1 : 0;
  }
  if (sub == "edit") {
    if (argc < 5) return usage_capture();
    const std::string out = argv[4];
    std::shared_ptr<deckhand::TrackAudio> t = deckhand::decode_file(file, err);
    if (!t) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    MemorySource src(t->data(), t->frames, 2, t->sample_rate);
    ExportSettings es;
    fill_export(argc, argv, es);
    const ExportResult r = export_edit(src, file, std::atof(opt(argc, argv, "--start", "0")), std::atof(opt(argc, argv, "--end", "0")), out, es);
    if (!r.ok) { std::fprintf(stderr, "error: %s\n", r.error.c_str()); return 1; }
    std::printf("wrote %s (%llu frames)\n", r.path.c_str(), static_cast<unsigned long long>(r.frames));
    return 0;
  }
  return usage_capture();
}
