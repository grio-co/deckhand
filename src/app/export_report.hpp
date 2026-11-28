#pragma once
// The compatibility report shown before a USB export starts: which tracks a club player may refuse, what the on-stick
// conversion will do about each, and the hot cues beyond the eighth that the stick format cannot carry. Built from the
// library's selection without decoding any audio (the check reads file headers only), so it comes back in a moment.
// The panel (Panel::ExportReport) shows ExportReport; proceed / cancel decide whether the export then runs.
#include <cstdint>
#include <string>
#include <vector>

#include "deckhand/export/export_preflight.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/services/usb_export.hpp"

namespace deckhand {

struct ExportReportRow {
  std::int64_t track_id = 0;
  std::string title, artist;
  usbexport::Verdict verdict = usbexport::Verdict::Ok;  // worst of the problems (Ok when the row is only about cues)
  std::vector<std::string> problems;   // the rule table's plain-language messages (cue notes left out: see cues_beyond)
  // What the conversion will do about it.
  enum class Fix { None, Convert, ConvertOff, CannotFix } fix = Fix::None;
  std::string fix_detail;              // Convert: "48 kHz, 2 ch, 24-bit, 54.3 MB" (the written file)
  int cues_beyond = 0;                 // hot cues in slots 9-16: not exported
};

struct ExportReport {
  std::string profile;                 // rule table id
  std::vector<ExportReportRow> rows;   // only tracks with something to say
  std::size_t tracks = 0;              // tracks checked
  std::size_t will_not_play = 0, warnings = 0;  // tracks, by worst verdict
  std::size_t fixable = 0;             // will-not-play tracks the conversion can repair
  std::size_t cue_tracks = 0, cues_total = 0;   // tracks with cues beyond the eighth, and how many cues
  bool convert = false;                // the conversion is switched on
  bool clean() const { return rows.empty(); }  // nothing to tell: the export just starts
};

// Pure: the report for tracks already resolved. `tracks` supplies titles and cues (the preflight carries only ids).
ExportReport build_export_report(const usbexport::PreflightReport& rep, const std::vector<usbexport::ExportTrack>& tracks,
                                 const usbexport::PlayerProfile& profile, bool convert);

// The tracks an export of `request` would write (playlists with their folders, smart playlists, or the whole collection),
// with the fields the check needs. No audio is decoded.
std::vector<usbexport::ExportTrack> export_selection_tracks(Library& lib, const UsbExportRequest& request);

// Selection, check and report in one go (what the loader thread runs before a panel export).
ExportReport check_export(Library& lib, const UsbExportRequest& request);

}  // namespace deckhand
