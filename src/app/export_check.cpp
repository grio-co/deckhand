// The compatibility report before a USB export (export_report.hpp).
#include "export_report.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "deckhand/export/export.hpp"

namespace deckhand {

using namespace usbexport;

std::vector<usbexport::ExportTrack> export_selection_tracks(Library& lib, const UsbExportRequest& request) {
  const std::vector<PlaylistNode> nodes = lib.playlists();
  std::map<std::int64_t, const PlaylistNode*> by_id;
  for (const auto& n : nodes) by_id[n.id] = &n;

  // The same selection as export_library_to_usb: chosen playlists with their folders, a chosen folder with its contents.
  std::set<std::int64_t> wanted;
  if (request.playlist_ids.empty()) {
    for (const auto& n : nodes) wanted.insert(n.id);
  } else {
    for (const auto id : request.playlist_ids)
      for (std::int64_t p = id; p != 0 && by_id.count(p) && !wanted.count(p); p = by_id[p]->parent) wanted.insert(p);
    bool grew = true;
    while (grew) {
      grew = false;
      for (const auto& n : nodes)
        if (!wanted.count(n.id) && wanted.count(n.parent) && by_id[n.parent]->is_folder &&
            std::find(request.playlist_ids.begin(), request.playlist_ids.end(), n.parent) != request.playlist_ids.end())
          grew = wanted.insert(n.id).second || grew;
    }
  }
  std::vector<std::int64_t> order;
  std::set<std::int64_t> seen;
  bool any_playlist = false;
  for (const auto& n : nodes) {
    if (!wanted.count(n.id)) continue;
    any_playlist = true;
    if (n.is_folder) continue;
    for (const auto id : lib.playlist_track_ids(n.id))
      if (seen.insert(id).second) order.push_back(id);
  }
  {
    const bool everything = request.playlist_ids.empty() && request.smart_playlist_ids.empty();
    for (const SmartPlaylist& sp : lib.smart_playlists()) {
      if (!everything && std::find(request.smart_playlist_ids.begin(), request.smart_playlist_ids.end(), sp.id) == request.smart_playlist_ids.end()) continue;
      any_playlist = true;
      for (const auto id : lib.query_ids(sp.rules, sp.match_all))
        if (seen.insert(id).second) order.push_back(id);
    }
  }
  if (!any_playlist && request.playlist_ids.empty() && request.smart_playlist_ids.empty())
    for (const auto& row : lib.tracks(0, "")) order.push_back(row.id);

  std::vector<usbexport::ExportTrack> out;
  for (const auto id : order) {
    const auto row = lib.track(id);
    if (!row) continue;
    ExportTrack t;
    t.id = static_cast<int>(id);
    t.source_path = row->path;
    t.title = row->title;
    t.artist = row->artist;
    t.album = row->album;
    for (const Cue& c : lib.cues(id)) {
      ExportCue e;
      e.hot = c.kind != CueKind::Memory && c.slot >= 0;
      e.slot = e.hot ? c.slot + 1 : 0;
      e.time_ms = static_cast<std::int64_t>(c.start_ms);
      t.cues.push_back(std::move(e));
    }
    out.push_back(std::move(t));
  }
  return out;
}

ExportReport build_export_report(const PreflightReport& rep, const std::vector<usbexport::ExportTrack>& tracks, const PlayerProfile& profile, bool convert) {
  ExportReport out;
  out.profile = rep.profile_id;
  out.convert = convert;
  out.tracks = tracks.size();
  std::map<int, const ExportTrack*> by_id;
  for (const ExportTrack& t : tracks) by_id[t.id] = &t;
  std::map<int, const TrackPreflight*> checked;
  for (const TrackPreflight& t : rep.tracks) checked[t.track_id] = &t;
  for (const ExportTrack& et : tracks) {
    ExportReportRow row;
    row.track_id = et.id;
    row.title = et.title;
    row.artist = et.artist;
    row.cues_beyond = static_cast<int>(hot_cues_beyond_format(et));
    const auto it = checked.find(et.id);
    if (it != checked.end()) {
      const TrackPreflight& t = *it->second;
      bool fixable_issue = false;
      for (const PreflightIssue& i : t.issues) {
        if (i.id != "cues.not_exported") row.problems.push_back(i.message);
        if (i.verdict > row.verdict && i.id != "cues.not_exported") row.verdict = i.verdict;
        fixable_issue = fixable_issue || (i.fixable && i.verdict == Verdict::WillNotPlay);
      }
      if (row.verdict == Verdict::WillNotPlay) {
        ConversionPlan plan;
        if (t.fixable || fixable_issue) {
          if (convert && plan_pcm24_conversion(t.format, profile, plan)) {
            row.fix = ExportReportRow::Fix::Convert;
            char b[96];
            std::snprintf(b, sizeof b, "%.1f kHz, %d ch, 24-bit, %.1f MB", plan.sample_rate / 1000.0, plan.channels,
                          static_cast<double>(plan.out_bytes) / 1048576.0);
            row.fix_detail = b;
          } else {
            row.fix = convert ? ExportReportRow::Fix::CannotFix : ExportReportRow::Fix::ConvertOff;
          }
        } else {
          row.fix = ExportReportRow::Fix::CannotFix;
        }
      }
      if (row.verdict == Verdict::WillNotPlay) {
        ++out.will_not_play;
        if (row.fix != ExportReportRow::Fix::CannotFix) ++out.fixable;
      } else if (row.verdict == Verdict::Warning) {
        ++out.warnings;
      }
    }
    if (row.cues_beyond > 0) {
      ++out.cue_tracks;
      out.cues_total += static_cast<std::size_t>(row.cues_beyond);
    }
    if (!row.problems.empty() || row.cues_beyond > 0) out.rows.push_back(std::move(row));
  }
  return out;
}

ExportReport check_export(Library& lib, const UsbExportRequest& request) {
  const std::vector<ExportTrack> tracks = export_selection_tracks(lib, request);
  const PlayerProfile* profile = request.player_profile.empty() ? nullptr : find_player_profile(request.player_profile);
  if (!profile) profile = &default_player_profile();
  const PreflightReport rep = preflight_export(tracks, *profile);
  return build_export_report(rep, tracks, *profile, request.convert_incompatible);
}

}  // namespace deckhand
