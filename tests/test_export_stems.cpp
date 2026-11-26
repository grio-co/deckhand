// Stems on the stick: synthetic stems written in the .dhstems layout, exported in every mode, read back.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "check.hpp"
#include "deckhand/export/export.hpp"
#include "deckhand/export/export_check.hpp"
#include "deckhand/export/export_preflight.hpp"
#include "deckhand/export/export_stems.hpp"

#if defined(_WIN32)
#include <process.h>
#define DH_PID _getpid()
#else
#include <unistd.h>
#define DH_PID getpid()
#endif

using namespace deckhand::usbexport;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

constexpr int kRate = 44100;
constexpr std::uint64_t kFrames = 3 * kRate;  // 3 s

fs::path unique_dir(const char* tag) {
  static std::atomic<int> n{0};
  const fs::path p = fs::temp_directory_path() / ("dh_stems_" + std::string(tag) + "_" + std::to_string(DH_PID) + "_" +
                                                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                                                  std::to_string(n++));
  fs::create_directories(p);
  return p;
}

// Value of stem `s` (0 vocals, 1 drums, 2 bass, 3 other), channel c, frame i, as a 24-bit integer.
std::int32_t sample(int s, int c, std::uint64_t i, int base) {
  static const int amp[4] = {1500000, 1100000, 900000, 700000};
  static const int period[4] = {97, 53, 211, 31};
  const int ph = static_cast<int>((i + static_cast<std::uint64_t>(base)) % static_cast<std::uint64_t>(period[s]));
  const double v = amp[s] * ((ph < period[s] / 2) ? 1.0 : -1.0) * (c == 0 ? 1.0 : 0.5);  // square tones
  return static_cast<std::int32_t>(v);
}

void put32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i)); }

// Writes a .dhstems file (layout in src/stems/include/deckhand/stems/cache.hpp), 24-bit, scale 1.
void write_stems(const fs::path& path, int base, std::uint64_t frames = kFrames, int rate = kRate) {
  Bytes h(128, 0);
  std::memcpy(h.data(), "DHSTEM01", 8);
  put32(&h[8], 1);
  put32(&h[12], 24);
  put32(&h[16], static_cast<std::uint32_t>(rate));
  put32(&h[20], 4);
  put32(&h[24], 2);
  const float one = 1.0f;
  std::memcpy(&h[28], &one, 4);
  put32(&h[32], static_cast<std::uint32_t>(frames));
  put32(&h[36], static_cast<std::uint32_t>(frames >> 32));
  std::memset(&h[40], 'a', 64);
  fs::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(h.data()), 128);
  Bytes col(frames * 3);
  for (int s = 0; s < 4; ++s)
    for (int c = 0; c < 2; ++c) {
      for (std::uint64_t i = 0; i < frames; ++i) {
        const std::uint32_t u = static_cast<std::uint32_t>(sample(s, c, i, base));
        col[i * 3] = static_cast<std::uint8_t>(u);
        col[i * 3 + 1] = static_cast<std::uint8_t>(u >> 8);
        col[i * 3 + 2] = static_cast<std::uint8_t>(u >> 16);
      }
      f.write(reinterpret_cast<const char*>(col.data()), static_cast<std::streamsize>(col.size()));
    }
  const char end[16] = {'D', 'H', 'E', 'N', 'D', 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  f.write(end, 16);
  CHECK(f.good());
}

ExportTrack make_track(int id, const fs::path& dir) {
  ExportTrack t;
  t.id = id;
  const fs::path p = dir / ("song" + std::to_string(id) + ".mp3");
  std::ofstream(p, std::ios::binary) << "audio" << id;
  t.source_path = p.string();
  t.title = "Song " + std::to_string(id);
  t.artist = "Artist " + std::to_string(id % 2);
  t.album = "Album";
  t.genre = "House";
  t.key = "8A";
  t.bpm = 120.0;
  t.duration_ms = 3000;
  t.sample_rate = kRate;
  t.bitrate = 320;
  for (int b = 0; b < 6; ++b) t.beats.push_back({std::int64_t(b) * 500 + 40, 120.0, b % 4 + 1});
  ExportCue hot;
  hot.hot = true;
  hot.slot = 1;
  hot.time_ms = 540;
  hot.label = "Drop";
  hot.has_rgb = true;
  hot.r = 200;
  hot.g = 20;
  hot.b = 90;
  ExportCue mem;
  mem.time_ms = 1040;
  ExportCue loop;
  loop.time_ms = 1540;
  loop.loop_end_ms = 2040;
  t.cues = {hot, mem, loop};
  t.waveform.assign(450, WaveSample{10, 20, 30});
  return t;
}

const ReadTrack* find_title(const ReadExport& r, const std::string& title) {
  for (const auto& t : r.tracks)
    if (t.track.title == title) return &t;
  return nullptr;
}

struct Wav {
  int rate = 0, bits = 0, channels = 0;
  std::uint64_t frames = 0;
  std::vector<std::int32_t> l, r;
};
Wav read_wav(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  CHECK(f.good());
  Bytes d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(d.size() >= 44);
  CHECK(std::memcmp(d.data(), "RIFF", 4) == 0 && std::memcmp(&d[8], "WAVEfmt ", 8) == 0 && std::memcmp(&d[36], "data", 4) == 0);
  auto le32 = [&](std::size_t o) { return static_cast<std::uint32_t>(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (std::uint32_t(d[o + 3]) << 24)); };
  CHECK(d[20] == 1 && d[21] == 0);
  Wav w;
  w.channels = d[22];
  w.rate = static_cast<int>(le32(24));
  w.bits = d[34];
  const std::uint32_t data = le32(40);
  CHECK(data + 44 == d.size());
  CHECK(le32(4) == data + 36);
  const int bps = w.bits / 8;
  w.frames = data / (2 * static_cast<std::uint64_t>(bps));
  for (std::uint64_t i = 0; i < w.frames; ++i)
    for (int c = 0; c < 2; ++c) {
      const std::size_t o = 44 + (i * 2 + static_cast<std::uint64_t>(c)) * static_cast<std::size_t>(bps);
      std::int32_t v = bps == 3 ? static_cast<std::int32_t>((std::uint32_t(d[o]) << 8 | std::uint32_t(d[o + 1]) << 16 | std::uint32_t(d[o + 2]) << 24)) >> 8
                                : static_cast<std::int16_t>(d[o] | (d[o + 1] << 8));
      (c == 0 ? w.l : w.r).push_back(v);
    }
  return w;
}

fs::path stick_file(const fs::path& root, const std::string& db_path) { return root / fs::path(db_path.substr(1)); }

struct Fixture {
  fs::path src, stick, cache;
  std::vector<ExportTrack> tracks;
  std::vector<ExportPlaylist> lists;
  std::map<int, std::string> stems;  // track id -> .dhstems
  ExportOptions opt;
  Fixture(const char* tag, int n = 3) {
    src = unique_dir((std::string(tag) + "_src").c_str());
    stick = unique_dir((std::string(tag) + "_stick").c_str());
    cache = unique_dir((std::string(tag) + "_cache").c_str());
    for (int i = 1; i <= n; ++i) {
      tracks.push_back(make_track(i, src));
      if (i != 3) {  // track 3 has no cached stems
        stems[i] = (cache / ("t" + std::to_string(i) + ".dhstems")).string();
        write_stems(stems[i], i * 7);
      }
    }
    ExportPlaylist folder;
    folder.id = 1;
    folder.name = "Sets";
    folder.is_folder = true;
    ExportPlaylist a;
    a.id = 2;
    a.name = "Warmup";
    a.parent_id = 1;
    a.track_ids = {1, 2};
    ExportPlaylist b;
    b.id = 3;
    b.name = "Peak";
    b.track_ids = {2, 3};
    lists = {folder, a, b};
    opt.destination = stick.string();
    opt.stems.lookup = [this](const ExportTrack& t) {
      const auto it = stems.find(t.id);
      return it == stems.end() ? std::string() : it->second;
    };
  }
  ~Fixture() {
    std::error_code ec;
    fs::remove_all(src, ec);
    fs::remove_all(stick, ec);
    fs::remove_all(cache, ec);
  }
};

void check_stem_track(const Fixture& fx, const ReadExport& rd, const std::string& parent_title, const std::string& suffix,
                      const std::vector<int>& stems_in_mix, int bits) {
  const ReadTrack* p = find_title(rd, parent_title);
  const ReadTrack* s = find_title(rd, parent_title + " (" + suffix + ")");
  CHECK(p && s);
  CHECK(s->track.artist == p->track.artist && s->track.album == p->track.album && s->track.genre == p->track.genre);
  CHECK(s->track.key == p->track.key && s->track.bpm == p->track.bpm);
  CHECK(s->track.duration_ms == p->track.duration_ms);
  // same grid, same cues and loops
  CHECK(s->track.beats.size() == p->track.beats.size());
  for (std::size_t i = 0; i < p->track.beats.size(); ++i) {
    CHECK(s->track.beats[i].time_ms == p->track.beats[i].time_ms);
    CHECK(s->track.beats[i].beat_in_bar == p->track.beats[i].beat_in_bar);
    CHECK(s->track.beats[i].bpm == p->track.beats[i].bpm);
  }
  CHECK(s->track.cues.size() == p->track.cues.size() && s->track.cues.size() == 3);
  for (std::size_t i = 0; i < p->track.cues.size(); ++i) {
    const auto &a = p->track.cues[i], &b = s->track.cues[i];
    CHECK(a.hot == b.hot && a.slot == b.slot && a.time_ms == b.time_ms && a.loop_end_ms == b.loop_end_ms && a.label == b.label);
  }
  CHECK(s->dat_cues.size() == p->dat_cues.size());
  CHECK(s->pwav.size() == p->pwav.size() && !s->pwav.empty() && !s->pwv3.empty() && s->pwv3.size() == p->pwv3.size());
  // the file is a valid WAV and the exact sum of its stems
  const Wav w = read_wav(stick_file(fx.stick, s->file_path));
  CHECK(w.channels == 2 && w.rate == kRate && w.bits == bits && w.frames == kFrames);
  const int base = (parent_title.back() - '0') * 7;
  for (std::uint64_t i = 0; i < kFrames; i += 1) {
    std::int64_t el = 0, er = 0;
    for (int st : stems_in_mix) {
      el += sample(st, 0, i, base);
      er += sample(st, 1, i, base);
    }
    if (bits == 24) CHECK(w.l[i] == el && w.r[i] == er);
    else {
      std::int64_t rl = 0, rr2 = 0;
      for (int st : stems_in_mix) { rl += sample(st, 0, i, base); rr2 += sample(st, 1, i, base); }
      const double xl = static_cast<double>(rl) / 8388608.0 * 32768.0, xr = static_cast<double>(rr2) / 8388608.0 * 32768.0;
      CHECK(w.l[i] == static_cast<std::int32_t>(std::llround(xl)) && w.r[i] == static_cast<std::int32_t>(std::llround(xr)));
    }
  }
}

std::size_t count_files(const fs::path& root) {
  std::size_t n = 0;
  for (auto& e : fs::recursive_directory_iterator(root))
    if (e.is_regular_file()) ++n;
  return n;
}

}  // namespace

int main() {
  // mode parsing
  {
    unsigned m = 99;
    std::string err;
    CHECK(parse_stem_mixes("off", m, err) && m == 0);
    CHECK(parse_stem_mixes("acapella+instrumental", m, err) && m == kMixesAcapellaInstrumental);
    CHECK(parse_stem_mixes("all", m, err) && m == kMixesFourStems);
    CHECK(parse_stem_mixes("Drums, bass", m, err) && m == (kMixDrums | kMixBass));
    CHECK(!parse_stem_mixes("guitar", m, err) && !err.empty());
    CHECK(stem_mixes_text(kMixesAcapellaInstrumental) == "acapella+instrumental" && stem_mixes_text(0) == "off");
  }

  // acapella + instrumental, mirror folder (default)
  {
    Fixture fx("ai");
    fx.opt.stems.mixes = kMixesAcapellaInstrumental;
    std::string err;
    ExportResult res;
    CHECK(export_usb(fx.tracks, fx.lists, fx.opt, err, res));
    CHECK(res.stems.tracks == 4);
    CHECK(res.stems.no_stems.size() == 1 && res.stems.no_stems[0].track_id == 3 && res.stems.no_stems[0].reason == "no stems cached");
    CHECK(res.required_bytes > 0 && res.available_bytes >= res.required_bytes);
    ValidationReport v;
    CHECK(validate_export(fx.stick.string(), v));
    ReadExport rd;
    CHECK(read_export(fx.stick.string(), rd, err));
    CHECK(rd.tracks.size() == 3 + 4);
    for (const char* t : {"Song 1", "Song 2"}) {
      check_stem_track(fx, rd, t, "Acapella", {0}, 24);
      check_stem_track(fx, rd, t, "Instrumental", {1, 2, 3}, 24);
    }
    CHECK(!find_title(rd, "Song 3 (Acapella)"));
    // playlists: originals unchanged, plus a "Stems" folder mirroring the tree
    std::map<std::string, const ExportPlaylist*> by_name;
    std::map<int, const ExportPlaylist*> by_id;
    for (const auto& p : rd.playlists) { by_id[p.id] = &p; }
    const ExportPlaylist *stems_folder = nullptr, *sets_mirror = nullptr, *warm_mirror = nullptr, *peak_mirror = nullptr, *warm = nullptr;
    for (const auto& p : rd.playlists) {
      if (p.name == "Stems" && p.is_folder) stems_folder = &p;
    }
    CHECK(stems_folder && stems_folder->parent_id == 0);
    for (const auto& p : rd.playlists) {
      if (p.name == "Sets" && p.parent_id == stems_folder->id) sets_mirror = &p;
      if (p.name == "Peak" && p.parent_id == stems_folder->id) peak_mirror = &p;
      if (p.name == "Warmup" && p.parent_id != 1) warm_mirror = &p;
      if (p.name == "Warmup" && p.parent_id == 1) warm = &p;
    }
    CHECK(sets_mirror && sets_mirror->is_folder && peak_mirror && warm_mirror && warm);
    CHECK(warm_mirror->parent_id == sets_mirror->id);
    CHECK(warm->track_ids == std::vector<int>({1, 2}));  // normal playlists stay uncluttered
    CHECK(warm_mirror->track_ids.size() == 4 && peak_mirror->track_ids.size() == 2);  // track 3 has none
    for (int id : warm_mirror->track_ids) CHECK(id >= 1000000000);

    // same grid and cue ids for the validator, and a second export to the same stick duplicates nothing
    const std::size_t files = count_files(fx.stick);
    ExportResult res2;
    CHECK(export_usb(fx.tracks, fx.lists, fx.opt, err, res2));
    CHECK(count_files(fx.stick) == files);
    CHECK(res2.stems.reused == 4 && res2.stems.bytes == 0);
    ReadExport rd2;
    CHECK(read_export(fx.stick.string(), rd2, err));
    CHECK(rd2.tracks.size() == rd.tracks.size());
    for (const auto& t : rd2.tracks) CHECK(t.file_path.find(" (2)") == std::string::npos);
    CHECK(validate_export(fx.stick.string(), v));
    // a stale stem file (stems were re-separated) is replaced under the same name, not duplicated
    write_stems(fx.stems[1], 1234);
    CHECK(export_usb(fx.tracks, fx.lists, fx.opt, err, res2));
    CHECK(count_files(fx.stick) == files && res2.stems.reused == 2);
    CHECK(validate_export(fx.stick.string(), v));
  }

  // all four, 16-bit, inline placement
  {
    Fixture fx("four16");
    fx.opt.stems.mixes = kMixesFourStems;
    fx.opt.stems.bits = 16;
    fx.opt.stems.placement = StemPlacement::Inline;
    std::string err;
    ExportResult res;
    CHECK(export_usb(fx.tracks, fx.lists, fx.opt, err, res));
    CHECK(res.stems.tracks == 8);
    ReadExport rd;
    CHECK(read_export(fx.stick.string(), rd, err));
    CHECK(rd.tracks.size() == 11);
    check_stem_track(fx, rd, "Song 1", "Acapella", {0}, 16);
    check_stem_track(fx, rd, "Song 2", "Drums", {1}, 16);
    check_stem_track(fx, rd, "Song 2", "Bass", {2}, 16);
    check_stem_track(fx, rd, "Song 1", "Other", {3}, 16);
    for (const auto& p : rd.playlists) CHECK(p.name != "Stems");
    for (const auto& p : rd.playlists)
      if (p.name == "Warmup") {
        CHECK(p.track_ids.size() == 10);  // 1, its 4 stems, 2, its 4 stems
        CHECK(p.track_ids[0] == 1 && p.track_ids[5] == 2);
      }
    ValidationReport v;
    CHECK(validate_export(fx.stick.string(), v));
  }

  // chosen set + per-playlist override
  {
    Fixture fx("chosen");
    fx.opt.stems.mixes = kMixDrums | kMixBass;
    fx.opt.stems.mixes_by_playlist[3] = 0;                // "Peak": no stems
    fx.opt.stems.mixes_by_playlist[2] = kMixAcapella;     // "Warmup": acapella only
    std::string err;
    ExportResult res;
    CHECK(export_usb(fx.tracks, fx.lists, fx.opt, err, res));
    ReadExport rd;
    CHECK(read_export(fx.stick.string(), rd, err));
    // track 1 and 2 are in Warmup (acapella); track 2 is also in Peak (off) -> union is acapella only
    CHECK(find_title(rd, "Song 1 (Acapella)") && find_title(rd, "Song 2 (Acapella)"));
    CHECK(!find_title(rd, "Song 1 (Drums)") && !find_title(rd, "Song 2 (Bass)"));
    CHECK(rd.tracks.size() == 5);
    ValidationReport v;
    CHECK(validate_export(fx.stick.string(), v));
  }

  // off: byte-for-byte the normal export (no stem tracks, no extra folder)
  {
    Fixture fx("off");
    std::string err;
    ExportResult res;
    CHECK(export_usb(fx.tracks, fx.lists, fx.opt, err, res));
    ReadExport rd;
    CHECK(read_export(fx.stick.string(), rd, err));
    CHECK(rd.tracks.size() == 3 && res.stems.tracks == 0 && res.required_bytes > 0);
  }

  // free space: refuse before writing anything, report the required bytes
  {
    Fixture fx("space");
    fx.opt.stems.mixes = kMixesFourStems;
    fx.opt.available_bytes_override = 1000000;  // 1 MB
    std::string err;
    ExportResult res;
    CHECK(!export_usb(fx.tracks, fx.lists, fx.opt, err, res));
    CHECK(err.find("not enough free space") != std::string::npos);
    CHECK(res.required_bytes > 1000000 && res.available_bytes == 1000000);
    CHECK(res.stems.bytes == 8ull * (44 + kFrames * 6));
    std::error_code ec;
    CHECK(!fs::exists(fx.stick / "PIONEER", ec) && !fs::exists(fx.stick / "Contents", ec));
    // the same export without stems fits in the same space
    fx.opt.stems.mixes = 0;
    fx.opt.available_bytes_override = 10000000;
    CHECK(export_usb(fx.tracks, fx.lists, fx.opt, err, res));
    // with stems it needs more than the audio-only run
    const std::uint64_t plain = res.required_bytes;
    fx.opt.stems.mixes = kMixesFourStems;
    fx.opt.destination = (fx.stick / "second").string();
    CHECK(export_usb(fx.tracks, fx.lists, fx.opt, err, res));
    CHECK(res.required_bytes > plain + 8ull * kFrames * 6 - 1);
  }

  // stems that do not match the track are listed, not exported
  {
    Fixture fx("mismatch");
    write_stems(fx.stems[2], 3, kFrames / 2);  // half the length
    fx.opt.stems.mixes = kMixAcapella;
    std::string err;
    ExportResult res;
    CHECK(export_usb(fx.tracks, fx.lists, fx.opt, err, res));
    CHECK(res.stems.tracks == 1 && res.stems.no_stems.size() == 2);
    // a damaged file (truncated) is also listed
    std::error_code ec;
    fs::resize_file(fx.stems[1], 500, ec);
    CHECK(export_usb(fx.tracks, fx.lists, fx.opt, err, res));
    CHECK(res.stems.tracks == 0 && res.stems.no_stems.size() == 3);
    CHECK(res.stems.no_stems[0].reason.find("stems file unreadable") != std::string::npos);
  }

  std::printf("test_export_stems: ok\n");
  return 0;
}
