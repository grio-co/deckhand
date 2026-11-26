#include "deckhand/services/usb_export.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <set>

#include "deckhand/audio/track.hpp"
#include "deckhand/library/palette.hpp"
#include "deckhand/export/export.hpp"
#include "deckhand/export/export_preflight.hpp"
#include "deckhand/export/export_stems.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand {

std::function<std::string(std::int64_t, const std::string&)> stem_lookup_from_data_dir(const std::string& data_dir) {
  namespace fs = std::filesystem;
  std::map<std::int64_t, std::string> keys;
  std::ifstream in(fs::path(data_dir) / "stems" / "tracks.tsv");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::string id, key;
    if (std::getline(ss, id, '\t') && std::getline(ss, key, '\t') && key.size() == 64) {
      try {
        keys[std::stoll(id)] = key;
      } catch (...) {
      }
    }
  }
  return [keys, data_dir](std::int64_t id, const std::string&) -> std::string {
    const auto it = keys.find(id);
    if (it == keys.end()) return {};
    const fs::path p = fs::path(data_dir) / "stems" / it->second.substr(0, 2) / (it->second + ".dhstems");
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? p.string() : std::string();
  };
}

bool export_library_to_usb(Library& lib, const UsbExportRequest& request, UsbExportResult& result, std::string& error,
                           const std::function<bool(int, int, const std::string&)>& progress) {
  using namespace usbexport;
  result = {};
  try {
    const std::vector<PlaylistNode> nodes = lib.playlists();
    std::map<std::int64_t, const PlaylistNode*> by_id;
    for (const auto& n : nodes) by_id[n.id] = &n;

    // Selected playlists plus the folders that contain them.
    std::set<std::int64_t> wanted;
    if (request.playlist_ids.empty()) {
      for (const auto& n : nodes) wanted.insert(n.id);
    } else {
      for (const auto id : request.playlist_ids) {
        for (std::int64_t p = id; p != 0 && by_id.count(p) && !wanted.count(p); p = by_id[p]->parent) wanted.insert(p);
      }
      // A selected folder brings its contents.
      bool grew = true;
      while (grew) {
        grew = false;
        for (const auto& n : nodes)
          if (!wanted.count(n.id) && wanted.count(n.parent) && by_id[n.parent]->is_folder &&
              std::find(request.playlist_ids.begin(), request.playlist_ids.end(), n.parent) != request.playlist_ids.end())
            grew = wanted.insert(n.id).second || grew;
      }
    }

    std::vector<ExportPlaylist> playlists;
    std::vector<std::int64_t> track_order;
    std::set<std::int64_t> track_seen;
    for (const auto& n : nodes) {
      if (!wanted.count(n.id)) continue;
      ExportPlaylist p;
      p.id = static_cast<int>(n.id);
      p.name = n.name;
      p.parent_id = static_cast<int>(n.parent);
      p.is_folder = n.is_folder;
      if (!n.is_folder) {
        for (const auto id : lib.playlist_track_ids(n.id)) {
          p.track_ids.push_back(static_cast<int>(id));
          if (track_seen.insert(id).second) track_order.push_back(id);
        }
      }
      playlists.push_back(std::move(p));
    }
    // Smart playlists are written as ordinary playlists holding the tracks they match now.
    {
      const bool everything = request.playlist_ids.empty() && request.smart_playlist_ids.empty();
      for (const SmartPlaylist& sp : lib.smart_playlists()) {
        if (!everything && std::find(request.smart_playlist_ids.begin(), request.smart_playlist_ids.end(), sp.id) == request.smart_playlist_ids.end()) continue;
        ExportPlaylist p;
        p.id = static_cast<int>(kSmartExportIdBase + sp.id);
        p.name = sp.name;
        p.parent_id = 0;
        for (const auto id : lib.query_ids(sp.rules, sp.match_all)) {
          p.track_ids.push_back(static_cast<int>(id));
          if (track_seen.insert(id).second) track_order.push_back(id);
        }
        playlists.push_back(std::move(p));
      }
    }
    if (playlists.empty() && request.playlist_ids.empty() && request.smart_playlist_ids.empty()) {  // no playlists at all: export the collection
      for (const auto& row : lib.tracks(0, "")) track_order.push_back(row.id);
    }

    std::vector<ExportTrack> tracks;
    std::set<std::int64_t> dropped;
    const int total = static_cast<int>(track_order.size());
    int done = 0;
    for (const auto id : track_order) {
      const auto row = lib.track(id);
      if (progress && !progress(done++, total, row ? row->title : std::string())) {
        error = "cancelled";
        return false;
      }
      if (!row) {
        dropped.insert(id);
        continue;
      }
      std::string decode_error;
      const std::shared_ptr<TrackAudio> audio = decode_file(row->path, decode_error);
      if (!audio) {
        dropped.insert(id);
        ++result.missing_files;
        if (result.missing.size() < 10) result.missing.push_back(row->path);
        DH_LOG(Warn, "export: skipping %s (%s)", row->path.c_str(), decode_error.c_str());
        continue;
      }
      ExportTrack t;
      t.id = static_cast<int>(id);
      t.source_path = row->path;
      t.title = row->title;
      t.artist = row->artist;
      t.album = row->album;
      t.genre = row->genre;
      t.key = row->key;
      t.bpm = row->bpm;
      t.rating = row->rating;
      t.comment = row->comment;
      if (row->color > 0) t.color_id = palette_nearest(row->color, 0, 8) + 1;
      t.sample_rate = static_cast<int>(audio->sample_rate);
      const double length_ms = static_cast<double>(audio->frames) / audio->sample_rate * 1000.0;
      t.duration_ms = static_cast<std::int64_t>(length_ms);
      std::error_code ec;
      const auto bytes = std::filesystem::file_size(row->path, ec);
      if (!ec && length_ms > 0) t.bitrate = static_cast<int>(static_cast<double>(bytes) * 8.0 / length_ms);

      const std::vector<BeatSection> grid = lib.beatgrid(id);
      for (std::size_t s = 0; s < grid.size(); ++s) {
        if (grid[s].bpm <= 0) continue;
        const double interval = 60000.0 / grid[s].bpm;
        const double end = s + 1 < grid.size() ? grid[s + 1].first_beat_ms : length_ms;
        int bar = grid[s].bar_beat;
        for (double ms = grid[s].first_beat_ms; ms < end - interval * 0.5; ms += interval) {
          t.beats.push_back({static_cast<std::int64_t>(std::llround(ms)), grid[s].bpm, bar});
          bar = bar % 4 + 1;
        }
      }
      for (const Cue& c : lib.cues(id)) {
        ExportCue e;
        e.hot = c.kind != CueKind::Memory && c.slot >= 0;
        e.slot = e.hot ? c.slot + 1 : 0;
        e.time_ms = static_cast<std::int64_t>(std::llround(c.start_ms));
        e.loop_end_ms = c.end_ms >= 0 ? static_cast<std::int64_t>(std::llround(c.end_ms)) : -1;
        e.label = c.label;
        if (c.color >= 0) {
          if (e.hot) {  // hot cues: the colour the player's pads light up in
            e.has_rgb = true;
            e.r = static_cast<std::uint8_t>((c.color >> 16) & 255);
            e.g = static_cast<std::uint8_t>((c.color >> 8) & 255);
            e.b = static_cast<std::uint8_t>(c.color & 255);
          } else {  // memory cues and loops: the nearest of the player's eight colours (palette rows 1..8)
            e.color_id = static_cast<std::uint8_t>(palette_nearest(c.color, 0, 8) + 1);
          }
        }
        t.cues.push_back(std::move(e));
      }
      t.waveform.reserve(audio->wave.size());
      for (const WavePoint& w : audio->wave) t.waveform.push_back({w.low, w.mid, w.high});
      tracks.push_back(std::move(t));
    }
    for (auto& p : playlists)
      std::erase_if(p.track_ids, [&](int id) { return dropped.count(id) != 0; });

    if (progress) progress(total, total, "");
    ExportOptions options;
    options.destination = request.destination;
    options.copy_audio = request.copy_audio;
    options.convert_incompatible = request.convert_incompatible;
    if (!request.player_profile.empty()) options.player_profile = request.player_profile;
    if (request.stem_mixes != 0 || !request.stem_mixes_by_playlist.empty()) {
      options.stems.mixes = request.stem_mixes;
      for (const auto& [id, m] : request.stem_mixes_by_playlist) options.stems.mixes_by_playlist[static_cast<int>(id)] = m;
      options.stems.bits = request.stem_bits;
      options.stems.placement = request.stems_inline ? StemPlacement::Inline : StemPlacement::MirrorFolder;
      if (!request.stems_folder.empty()) options.stems.folder_name = request.stems_folder;
      if (request.stem_lookup) {
        options.stems.lookup = [&request](const ExportTrack& t) { return request.stem_lookup(t.id, t.source_path); };
      }
    }
    ExportResult report;
    const bool exported = export_usb(tracks, playlists, options, error, report);
    result.report_json = report.report.to_json();
    result.converted = static_cast<int>(report.converted);
    result.remaining_will_not_play = static_cast<int>(report.remaining_will_not_play());
    result.stem_tracks = static_cast<int>(report.stems.tracks);
    for (const auto& n : report.stems.no_stems) result.no_stems.push_back(n.title + ": " + n.reason);
    result.cues_not_exported = static_cast<int>(report.cues_not_exported);
    result.required_bytes = report.required_bytes;
    result.available_bytes = report.available_bytes;
    if (!exported) return false;
    result.tracks = static_cast<int>(tracks.size());
    for (const auto& p : playlists) result.playlists += p.is_folder ? 0 : 1;
    DH_LOG(Info, "exported %d tracks and %d playlists to %s (%d files missing)", result.tracks, result.playlists,
           request.destination.c_str(), result.missing_files);
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

}  // namespace deckhand
