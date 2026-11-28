// The compatibility report shown before a USB export: per track the problem, what the on-stick conversion will do, and the
// hot cues beyond the eighth; clean selections produce no report; the conversion switch changes the verdicts.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "export_report.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {
void put32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 255); }
void put16(std::string& s, std::uint16_t v) { for (int i = 0; i < 2; ++i) s += static_cast<char>((v >> (8 * i)) & 255); }

// A small WAV: format 1 = integer PCM, 3 = float.
void write_wav(const fs::path& p, int format, int bits, int rate = 44100, int channels = 2, int frames = 2000) {
  const std::uint32_t bytes = static_cast<std::uint32_t>(frames * channels * bits / 8);
  std::string s = "RIFF";
  put32(s, 36 + bytes);
  s += "WAVEfmt ";
  put32(s, 16);
  put16(s, static_cast<std::uint16_t>(format));
  put16(s, static_cast<std::uint16_t>(channels));
  put32(s, static_cast<std::uint32_t>(rate));
  put32(s, static_cast<std::uint32_t>(rate * channels * bits / 8));
  put16(s, static_cast<std::uint16_t>(channels * bits / 8));
  put16(s, static_cast<std::uint16_t>(bits));
  s += "data";
  put32(s, bytes);
  s.append(bytes, '\0');
  std::ofstream(p, std::ios::binary) << s;
}

std::int64_t add(Library& lib, const fs::path& file, const char* title) {
  NewTrack t;
  t.path = file.string();
  t.title = title;
  t.artist = "Artist";
  return lib.upsert_track(t);
}
}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_export_report";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  {
    Library lib((dir / "lib.db").string());
    write_wav(dir / "ok.wav", 1, 16);
    write_wav(dir / "ok2.wav", 1, 16);
    write_wav(dir / "float.wav", 3, 32);
    const auto ok = add(lib, dir / "ok.wav", "Fine");
    const auto fl = add(lib, dir / "float.wav", "Float");
    const auto gone = add(lib, dir / "gone.wav", "Gone");
    const auto cued = add(lib, dir / "ok2.wav", "Fine again");
    (void)ok;
    // ten hot cues on one track: two beyond the eighth
    std::vector<Cue> cues;
    for (int i = 0; i < 10; ++i) {
      Cue c;
      c.kind = CueKind::Hot;
      c.slot = i;
      c.start_ms = 1000.0 * (i + 1);
      cues.push_back(c);
    }
    lib.replace_all_cues(cued, cues);

    // a selection of only fine files: nothing to report
    const auto pl_ok = lib.create_playlist(0, "Fine", false);
    lib.add_to_playlist(pl_ok, ok);
    UsbExportRequest req;
    req.destination = (dir / "stick").string();
    req.playlist_ids.push_back(pl_ok);
    {
      const ExportReport fine = check_export(lib, req);
      for (const ExportReportRow& r : fine.rows)
        for (const std::string& m : r.problems) std::fprintf(stderr, "unexpected: %s: %s\n", r.title.c_str(), m.c_str());
      CHECK(fine.clean());
    }

    const auto pl = lib.create_playlist(0, "Set", false);
    for (const auto id : {ok, fl, gone, cued}) lib.add_to_playlist(pl, id);
    req.playlist_ids = {pl};
    ExportReport off = check_export(lib, req);
    CHECK(!off.clean() && off.tracks == 4);
    CHECK(off.will_not_play == 2);  // the float file and the missing one
    CHECK(off.cue_tracks == 1 && off.cues_total == 2);
    const ExportReportRow *frow = nullptr, *grow = nullptr, *crow = nullptr;
    for (const ExportReportRow& r : off.rows) {
      if (r.title == "Float") frow = &r;
      if (r.title == "Gone") grow = &r;
      if (r.title == "Fine again") crow = &r;
    }
    CHECK(frow && grow && crow && off.rows.size() == 3);
    CHECK(frow->verdict == usbexport::Verdict::WillNotPlay && !frow->problems.empty());
    CHECK(frow->fix == ExportReportRow::Fix::ConvertOff);  // fixable, the conversion is off
    CHECK(grow->fix == ExportReportRow::Fix::CannotFix);
    CHECK(crow->cues_beyond == 2 && crow->problems.empty() && crow->verdict == usbexport::Verdict::Ok);
    CHECK(off.fixable == 1);

    // with the conversion on, the float file says what will be written
    req.convert_incompatible = true;
    const ExportReport on = check_export(lib, req);
    const ExportReportRow* f2 = nullptr;
    for (const ExportReportRow& r : on.rows)
      if (r.title == "Float") f2 = &r;
    CHECK(on.convert && f2 && f2->fix == ExportReportRow::Fix::Convert && f2->fix_detail.find("24-bit") != std::string::npos);

    // every playlist when none is named, smart playlists as playlists, the same tracks once
    UsbExportRequest all;
    all.destination = req.destination;
    CHECK(export_selection_tracks(lib, all).size() == 4);
    const auto smart = lib.create_smart_playlist("Everything", true, {});
    req.playlist_ids.clear();
    req.smart_playlist_ids = {smart};
    CHECK(export_selection_tracks(lib, req).size() == 4);
  }
  fs::remove_all(dir, ec);
  std::puts("test_export_report: ok");
  return 0;
}
