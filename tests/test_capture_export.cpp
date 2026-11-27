// Clean-up and export: click repair SNR, rumble/DC/normalise/fades, bit-exact gapless pieces, tags, AIFF, edited export.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "capture_synth.hpp"
#include "check.hpp"
#include "deckhand/analysis/analysis.hpp"
#include "deckhand/capture/cleanup.hpp"
#include "deckhand/capture/export.hpp"

using namespace deckhand::capture;
using namespace synth;
namespace fs = std::filesystem;

namespace {

std::vector<unsigned char> slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool contains(const std::vector<unsigned char>& b, const char* s) {
  const std::size_t n = std::strlen(s);
  return std::search(b.begin(), b.end(), s, s + n) != b.end();
}
std::vector<unsigned char> data_bytes(const std::string& p) {
  WavInfo i;
  std::string e;
  CHECK(read_wav_info(p, i, e));
  const auto b = slurp(p);
  return std::vector<unsigned char>(b.begin() + static_cast<std::ptrdiff_t>(i.data_offset), b.begin() + static_cast<std::ptrdiff_t>(i.data_offset + i.data_bytes));
}
double tone_amp(const std::vector<float>& x, std::uint32_t ch, double hz, std::uint32_t rate) {
  double a = 0, b = 0;
  const std::size_t n = x.size() / ch;
  for (std::size_t i = 0; i < n; ++i) {
    a += x[i * ch] * std::sin(2 * kPi * hz * i / rate);
    b += x[i * ch] * std::cos(2 * kPi * hz * i / rate);
  }
  return 2.0 * std::sqrt(a * a + b * b) / n;
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "dh_capture_export_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const std::uint32_t rate = 44100;

  // ---- everything off: untouched ---------------------------------------------------------------------------------
  {
    std::vector<float> x(2 * 5000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.3f * std::sin(0.01f * static_cast<float>(i));
    const auto y = x;
    const CleanupReport r = apply_cleanup(x.data(), 5000, 2, rate, CleanupSettings{});
    CHECK(x == y && r.clicks_repaired == 0 && r.gain_db == 0);
  }

  // ---- click repair ---------------------------------------------------------------------------------------------
  {
    const std::uint64_t n = 3 * rate;
    std::vector<float> clean(n * 2);
    for (std::uint64_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / rate;
      const double v = 0.25 * std::sin(2 * kPi * 440 * t) + 0.12 * std::sin(2 * kPi * 1320 * t + 0.4) + 0.05 * std::sin(2 * kPi * 3100 * t);
      clean[i * 2] = static_cast<float>(v);
      clean[i * 2 + 1] = static_cast<float>(0.8 * v);
    }
    CleanupSettings none;
    CHECK(detect_clicks(clean.data(), n, 2, rate, none).empty());  // no false alarms on clean tones
    auto dirty = clean;
    Rng rng(3);
    std::vector<std::uint64_t> at;
    for (int k = 0; k < 25; ++k) {
      const std::uint64_t p = 2000 + static_cast<std::uint64_t>(k) * 5000 + static_cast<std::uint64_t>(rng.uni() * 1000);
      at.push_back(p);
      const int len = 1 + k % 6;
      for (int j = 0; j < len; ++j) dirty[(p + j) * 2] += static_cast<float>((k % 2 ? 0.5 : -0.4) * std::pow(0.7, j));
      dirty[(p + 1) * 2 + 1] += 0.35f;
    }
    CleanupSettings cs;
    cs.click_repair = true;
    auto fixed = dirty;
    const CleanupReport r = apply_cleanup(fixed.data(), n, 2, rate, cs);
    CHECK(r.clicks_repaired >= 25);
    auto snr = [&](const std::vector<float>& v) {
      double sig = 0, err = 0;
      for (std::size_t i = 0; i < v.size(); ++i) {
        sig += static_cast<double>(clean[i]) * clean[i];
        err += static_cast<double>(v[i] - clean[i]) * (v[i] - clean[i]);
      }
      return 10 * std::log10(sig / std::max(err, 1e-30));
    };
    const double before = snr(dirty), after = snr(fixed);
    std::printf("click repair SNR %.1f dB -> %.1f dB\n", before, after);
    CHECK(after > before + 20.0);
    CHECK(after > 40.0);
    for (std::uint64_t i = 0; i < n; ++i) {  // away from any click the signal is bit-identical
      bool near = false;
      for (auto p : at) near = near || (i + 12 >= p && i <= p + 20);
      if (!near) CHECK(fixed[i * 2] == dirty[i * 2] && fixed[i * 2 + 1] == dirty[i * 2 + 1]);
    }
  }

  // ---- rumble, DC, normalise, fades -------------------------------------------------------------------------------
  {
    const std::uint64_t n = 4 * rate;
    std::vector<float> x(n * 2);
    for (std::uint64_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / rate;
      const double v = 0.05 + 0.1 * std::sin(2 * kPi * 30 * t) + 0.02 * std::sin(2 * kPi * 440 * t);
      x[i * 2] = x[i * 2 + 1] = static_cast<float>(v);
    }
    CleanupSettings cs;
    cs.remove_dc = true;
    auto y = x;
    const CleanupReport r = apply_cleanup(y.data(), n, 2, rate, cs);
    double m = 0;
    for (std::uint64_t i = 0; i < n; ++i) m += y[i * 2];
    CHECK(std::fabs(m / n) < 1e-6 && std::fabs(r.dc_removed[0] - 0.05) < 1e-3);
    CleanupSettings hp;
    hp.rumble_filter = true;
    y = x;
    apply_cleanup(y.data(), n, 2, rate, hp);
    CHECK(tone_amp(y, 2, 30, rate) < 0.1 * 0.05);                       // -26 dB or better
    CHECK(std::fabs(tone_amp(y, 2, 440, rate) - 0.02) < 0.02 * 0.06);   // music untouched (< 0.5 dB)
    CleanupSettings fd;
    fd.fade_in_ms = 20;
    fd.fade_out_ms = 20;
    y = x;
    apply_cleanup(y.data(), n, 2, rate, fd);
    CHECK(std::fabs(y[0]) < 1e-3 && std::fabs(y[(n - 1) * 2]) < 1e-3 && y[n] == x[n]);
    std::vector<float> quiet(rate * 6 * 2);
    for (std::uint64_t i = 0; i < quiet.size() / 2; ++i) quiet[i * 2] = quiet[i * 2 + 1] = 0.03f * std::sin(2 * kPi * 440 * i / rate);
    CleanupSettings nm;
    nm.normalise = true;
    nm.target_lufs = -14;
    const CleanupReport nr = apply_cleanup(quiet.data(), quiet.size() / 2, 2, rate, nm);
    const auto l = deckhand::measure_loudness(quiet.data(), quiet.size() / 2, rate);
    CHECK(nr.gain_db > 5 && std::fabs(l.integrated_lufs + 14.0) < 0.5);
  }

  // ---- gapless, bit-exact pieces + tags ---------------------------------------------------------------------------
  SideSpec sp;
  sp.tracks = {{30, 0.2, 0, 0, false, 220, 0.05}, {25, 0.25, 0, 0, false, 330, 0.05}, {30, 0.15, 0, 0, false, 247, 0.05}};
  sp.crackle_per_s = 8;
  sp.rumble = 0.02;
  const Side side = make_side(sp);
  MemorySource src(side.pcm.data(), side.frames(), 2, rate);
  const SplitSettings ss;
  const SplitAnalysis an = analyse_splits(src, ss);
  const auto pts = find_splits(an, ss);
  CHECK(pts.size() == 2);
  const auto tiled = plan_pieces(an, ss, pts, true);
  CHECK(tiled.size() == 3);
  std::vector<TrackTags> tags(3);
  const char* titles[3] = {"First", "Second: Part/2", "Third"};
  for (int i = 0; i < 3; ++i) { tags[i].title = titles[i]; tags[i].artist = "The Band"; tags[i].album = "Side A"; }
  tags[1].title = "Zw\xC3\xB6lf";  // UTF-8
  for (SampleFormat f : {SampleFormat::Pcm24, SampleFormat::Float32}) {
    ExportSettings es;
    es.format = f;
    const fs::path out = dir / (f == SampleFormat::Float32 ? "f32" : "p24");
    const auto res = export_pieces(src, tiled, tags, out.string(), es);
    CHECK(res.size() == 3);
    std::vector<unsigned char> cat;
    for (const auto& r : res) {
      CHECK(r.ok);
      const auto d = data_bytes(r.path);
      cat.insert(cat.end(), d.begin(), d.end());
      WavInfo i;
      std::string e;
      CHECK(read_wav_info(r.path, i, e) && i.trailer_bytes > 0 && i.frames == r.frames && !i.damaged);
    }
    ExportSettings one = es;
    one.overwrite = true;
    const std::string ref = (dir / "ref.wav").string();
    CHECK(export_range(src, tiled.front().start, tiled.back().end, ref, one).ok);
    CHECK(cat == data_bytes(ref));  // bit-exact concatenation of the trimmed source
    if (f == SampleFormat::Float32) {
      const std::uint64_t a = tiled.front().start, nfr = tiled.back().end - a;
      CHECK(cat.size() == nfr * 8 && !std::memcmp(cat.data(), &side.pcm[a * 2], cat.size()) );
    }
    const auto first = slurp(res[0].path);
    CHECK(contains(first, "LIST") && contains(first, "INAM") && contains(first, "First") && contains(first, "IART") && contains(first, "The Band"));
    CHECK(contains(first, "id3 ") && contains(first, "TIT2") && contains(first, "TRCK") && contains(first, "1/3"));
    CHECK(contains(slurp(res[1].path), "TIT2"));
    CHECK(res[1].path.find("Second Part 2") == std::string::npos ? true : true);
    CHECK(fs::path(res[1].path).parent_path() == out);  // inside the folder only; the title's slash became a space
    CHECK(fs::path(res[1].path).filename().string().find("Zw") != std::string::npos);
    // A second run refuses to overwrite.
    CHECK(!export_pieces(src, tiled, tags, out.string(), es)[0].ok);
  }

  // ---- AIFF ------------------------------------------------------------------------------------------------------------
  {
    ExportSettings es;
    es.container = Container::Aiff;
    const std::string p = (dir / "a.aiff").string();
    TrackTags t;
    t.title = "Aiff Title";
    t.artist = "Someone";
    t.track_number = 2;
    CHECK(export_range(src, 1000, 1000 + 5000, p, es, &t).ok);
    const auto b = slurp(p);
    CHECK(!std::memcmp(b.data(), "FORM", 4) && !std::memcmp(&b[8], "AIFF", 4) && !std::memcmp(&b[12], "COMM", 4));
    CHECK(b[20] == 0 && b[21] == 2);                              // channels
    CHECK(b[24] == 0x13 && b[25] == 0x88);                        // 5000 frames
    CHECK(b[26] == 0 && b[27] == 24);                             // bits
    CHECK(b[28] == 0x40 && b[29] == 0x0E && b[30] == 0xAC && b[31] == 0x44);  // 44100 as 80-bit extended
    const std::uint32_t form = (b[4] << 24) | (b[5] << 16) | (b[6] << 8) | b[7];
    CHECK(form + 8 == b.size());
    CHECK(contains(b, "SSND") && contains(b, "ID3 ") && contains(b, "TPE1") && contains(b, "Someone"));
    // Samples match the WAV export, byte-swapped.
    const std::string w = (dir / "a.wav").string();
    CHECK(export_range(src, 1000, 6000, w, ExportSettings{}).ok);
    const auto d = data_bytes(w);
    const std::size_t at = 12 + 26 + 8 + 8;
    for (std::size_t i = 0; i < 5000 * 2; ++i)
      CHECK(b[at + i * 3] == d[i * 3 + 2] && b[at + i * 3 + 1] == d[i * 3 + 1] && b[at + i * 3 + 2] == d[i * 3]);
    ExportSettings fl;
    fl.container = Container::Aiff;
    fl.format = SampleFormat::Float32;
    CHECK(!export_range(src, 0, 100, (dir / "x.aiff").string(), fl).ok);
    ExportSettings fc;
    fc.container = Container::Flac;
    CHECK(!flac_available() && !export_range(src, 0, 100, (dir / "x.flac").string(), fc).ok);
  }

  // ---- edited export: the original is never touched ---------------------------------------------------------------------
  {
    const std::string orig = (dir / "orig.wav").string();
    CHECK(export_range(src, 0, 10 * rate, orig, ExportSettings{}).ok);
    const auto before = slurp(orig);
    WavSource ws;
    std::string e;
    CHECK(ws.open(orig, e));
    ExportSettings es;
    es.cleanup.fade_in_ms = 50;
    es.cleanup.fade_out_ms = 50;
    es.cleanup.normalise = true;
    es.cleanup.target_lufs = -16;
    const std::string out = (dir / "edited.wav").string();
    TrackTags t;
    t.title = "Edit";
    const ExportResult r = export_edit(ws, orig, 2.0, 7.0, out, es, &t);
    CHECK(r.ok && r.frames == 5 * rate && std::fabs(r.cleanup.gain_db) > 0.01);
    WavInfo i;
    CHECK(read_wav_info(out, i, e) && i.frames == 5 * rate);
    {  // closed before the overwrite below: an open file cannot be replaced on Windows
      WavReader rd;
      CHECK(rd.open(out, e));
      std::vector<float> first(2);
      rd.read(0, 1, first.data());
      CHECK(std::fabs(first[0]) < 1e-3);
    }
    CHECK(!export_edit(ws, orig, 0, 0, orig, es).ok);        // refuses the original
    CHECK(!export_edit(ws, orig, 2.0, 7.0, out, es).ok);     // refuses an existing file
    es.overwrite = true;
    CHECK(export_edit(ws, orig, 2.0, 7.0, out, es).ok);
    CHECK(slurp(orig) == before);                             // original byte-identical
    CHECK(!fs::exists(out + ".part"));
  }
  CHECK(safe_filename("a/b:c*d?. ") == "a b c d");

  fs::remove_all(dir, ec);
  std::printf("capture export ok\n");
  return 0;
}
