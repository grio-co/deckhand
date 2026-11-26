// The glue between the library and the USB writer (services/usb_export): what is selected, what is carried over, and
// what happens when a file is missing or the caller cancels. The stick format itself is covered by test_export.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/export/export.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/services/usb_export.hpp"

using namespace deckhand;
namespace fs = std::filesystem;
namespace ux = deckhand::usbexport;

namespace {

void le(std::ofstream& o, std::uint32_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) o.put(static_cast<char>((v >> (8 * i)) & 255));
}
// Four seconds of 16-bit stereo at 44.1 kHz.
void write_wav(const fs::path& p) {
  const std::uint32_t frames = 4 * 44100, data = frames * 4;
  std::ofstream o(p, std::ios::binary);
  o << "RIFF";
  le(o, 36 + data, 4);
  o << "WAVEfmt ";
  le(o, 16, 4), le(o, 1, 2), le(o, 2, 2), le(o, 44100, 4), le(o, 44100 * 4, 4), le(o, 4, 2), le(o, 16, 2);
  o << "data";
  le(o, data, 4);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const auto v = static_cast<std::uint32_t>(static_cast<std::int16_t>(8000 * std::sin(i * 0.05)));
    le(o, v, 2), le(o, v, 2);
  }
}

const ux::ExportPlaylist* named(const ux::ReadExport& x, const std::string& name) {
  for (const auto& p : x.playlists)
    if (p.name == name) return &p;
  return nullptr;
}
const ux::ReadTrack* titled(const ux::ReadExport& x, const std::string& title) {
  for (const auto& t : x.tracks)
    if (t.track.title == title) return &t;
  return nullptr;
}

}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("dh_usb_glue_" + std::to_string(std::hash<std::string>{}(__FILE__ __TIME__)));
  fs::remove_all(root);
  fs::create_directories(root / "music");
  Library lib((root / "library.db").string());
  const auto add = [&](const std::string& title, bool on_disk, int rating) {
    NewTrack t;
    t.path = (root / "music" / (title + ".wav")).string();
    if (on_disk) write_wav(t.path);
    t.title = title;
    t.artist = "Artist " + title;
    t.comment = "note " + title;
    t.key = "8A";
    t.bpm = 120;
    t.rating = rating;
    t.duration_ms = 4000;
    return lib.upsert_track(t);
  };
  const auto one = add("One", true, 5), two = add("Two", true, 1), gone = add("Gone", false, 5);
  lib.replace_beatgrid(one, {{500.0, 120.0, 1}});
  lib.set_hot_cue(one, 2, 1000.0);
  lib.set_hot_cue_style(one, 2, "Drop", 0xE62828);
  lib.set_hot_cue(one, 0, 1500.0, 2500.0);
  lib.replace_memory_cues(one, {Cue{CueKind::Memory, -1, 3000.0, -1, "Out", -1}});

  const auto sets = lib.create_playlist(0, "Sets", true);
  const auto friday = lib.create_playlist(sets, "Friday", false);
  const auto other = lib.create_playlist(0, "Other", false);
  lib.add_to_playlist(friday, one);
  lib.add_to_playlist(friday, gone);
  lib.add_to_playlist(other, two);
  SmartRule five;
  five.field = SmartRule::Field::Rating;
  five.lo = 5;
  const auto smart = lib.create_smart_playlist("Five stars", true, {five});

  const auto run = [&](UsbExportRequest q, const std::string& name, UsbExportResult& res, ux::ReadExport& back) {
    q.destination = (root / name).string();
    fs::create_directories(q.destination);
    std::string error;
    const bool ok = export_library_to_usb(lib, q, res, error);
    if (!ok) std::fprintf(stderr, "export %s failed: %s\n", name.c_str(), error.c_str());
    CHECK(ok);
    CHECK(ux::read_export(q.destination, back, error));
  };

  {  // everything: every playlist and smart playlist; the missing file is reported and left out
    UsbExportResult res;
    ux::ReadExport x;
    run({}, "all", res, x);
    CHECK(res.tracks == 2 && res.playlists == 3 && res.missing_files == 1);
    CHECK(res.missing.size() == 1 && res.missing[0].find("Gone.wav") != std::string::npos);
    CHECK(!res.report_json.empty());
    CHECK(x.tracks.size() == 2 && !titled(x, "Gone"));
    const auto* s = named(x, "Sets");
    const auto* f = named(x, "Friday");
    const auto* m = named(x, "Five stars");
    CHECK(s && s->is_folder && f && !f->is_folder && f->parent_id == s->id && named(x, "Other"));
    CHECK(f->track_ids == std::vector<int>{static_cast<int>(one)});
    CHECK(m && !m->is_folder && m->parent_id == 0);
    CHECK(m->id == static_cast<int>(kSmartExportIdBase + smart));
    CHECK(m->track_ids == std::vector<int>{static_cast<int>(one)});

    const auto* t = titled(x, "One");
    CHECK(t != nullptr);
    const ux::ExportTrack& e = t->track;
    CHECK(e.artist == "Artist One" && e.comment == "note One" && e.key == "8A" && e.rating == 5);
    CHECK(std::fabs(e.bpm - 120) < 0.01 && e.sample_rate == 44100 && e.duration_ms == 4000);
    CHECK(e.beats.size() == 7);  // 0.5 s .. 3.5 s
    for (std::size_t i = 0; i < e.beats.size(); ++i)
      CHECK(e.beats[i].time_ms == static_cast<std::int64_t>(500 * (i + 1)) && e.beats[i].beat_in_bar == static_cast<int>(i % 4) + 1);
    CHECK(e.cues.size() == 3);
    const auto cue = [&](bool hot, int slot) -> const ux::ExportCue* {
      for (const auto& c : e.cues)
        if (c.hot == hot && (!hot || c.slot == slot)) return &c;
      return nullptr;
    };
    const auto* drop = cue(true, 3);  // library slots count from 0, the stick's from 1
    const auto* loop = cue(true, 1);
    const auto* mem = cue(false, 0);
    CHECK(drop && drop->time_ms == 1000 && drop->loop_end_ms < 0 && drop->label == "Drop");
    CHECK(loop && loop->time_ms == 1500 && loop->loop_end_ms == 2500);
    CHECK(mem && mem->time_ms == 3000 && mem->label == "Out");
  }
  {  // a selected playlist brings its folders, nothing else; smart playlists only when chosen
    UsbExportRequest q;
    q.playlist_ids = {friday};
    UsbExportResult res;
    ux::ReadExport x;
    run(q, "one", res, x);
    CHECK(named(x, "Sets") && named(x, "Friday") && !named(x, "Other") && !named(x, "Five stars"));
    CHECK(res.tracks == 1 && res.playlists == 1 && x.tracks.size() == 1);
  }
  {  // a selected folder brings its contents, plus the chosen smart playlist
    UsbExportRequest q;
    q.playlist_ids = {sets};
    q.smart_playlist_ids = {smart};
    UsbExportResult res;
    ux::ReadExport x;
    run(q, "folder", res, x);
    CHECK(named(x, "Sets") && named(x, "Friday") && named(x, "Five stars") && !named(x, "Other"));
    CHECK(res.playlists == 2 && res.tracks == 1);
  }
  {  // cancelling stops before anything is written
    UsbExportRequest q;
    q.destination = (root / "cancel").string();
    fs::create_directories(q.destination);
    UsbExportResult res;
    std::string error;
    int calls = 0;
    CHECK(!export_library_to_usb(lib, q, res, error, [&](int done, int total, const std::string&) {
      ++calls;
      CHECK(total == 3 && done == 0);
      return false;
    }));
    CHECK(calls == 1 && error == "cancelled" && fs::is_empty(q.destination));
  }
  {  // a destination that cannot be written is an error, not a crash
    UsbExportRequest q;
    q.destination = (root / "library.db" / "not-a-folder").string();
    UsbExportResult res;
    std::string error;
    CHECK(!export_library_to_usb(lib, q, res, error));
    CHECK(!error.empty());
  }
  std::error_code ec;
  fs::remove_all(root, ec);
  std::puts("usb export glue: all checks passed");
  return 0;
}
