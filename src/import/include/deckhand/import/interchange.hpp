#pragma once
// Library interchange through open, widely supported files: the DJ library XML ("DJ_PLAYLISTS" document: collection,
// tempo grids, cue/loop marks, playlist tree) and M3U8 playlists. See docs/INTERCHANGE.md for what is carried.
//
// None of these functions throws: every failure comes back in `ok` / `error`. Imports run in one database transaction
// (all or nothing); a dry run does the same work and rolls it back, so its statistics are exact.
#include <cstdint>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"

namespace deckhand {

// ---- XML export -------------------------------------------------------------------------------------------------
struct InterchangeExportOptions {
  std::int64_t playlist_id = 0;  // 0 = whole library; else only this playlist / folder subtree and the tracks it lists
  bool include_playlists = true;
  bool include_cues = true;      // hot cues, memory cues, loops
  bool include_grid = true;      // tempo sections
};

struct InterchangeExportStats {
  bool ok = false;
  std::string error;  // set when !ok
  int tracks = 0;
  int playlists = 0;  // playlists and folders written
  int cues = 0;       // marks written
  int files = 0;      // M3U8 export: files written
};

InterchangeExportStats export_library_xml(Library& lib, const std::string& path,
                                          const InterchangeExportOptions& options = {});

// ---- XML import -------------------------------------------------------------------------------------------------
struct InterchangeImportOptions {
  bool merge = true;                 // false: a track already in the library is left completely alone
  bool overwrite_cues = false;       // false: only fill what is empty (hot cue slots, memory cues)
  bool overwrite_grid = false;       // false: only tracks without a beat grid get one
  bool overwrite_metadata = false;   // false: only empty tags / BPM / key / colour / rating are filled
  bool match_by_name = true;         // no path match: same file name and a duration within one second, if unique
  bool import_playlists = true;
  bool import_play_counts = false;   // adds history entries (one session) up to the file's play count
  bool dry_run = false;              // nothing is written; statistics are what a real run would do
};

struct InterchangeStats {
  bool ok = false;
  std::string error;                 // set when !ok; nothing was written
  bool dry_run = false;
  int tracks_in_file = 0;
  int tracks_added = 0;
  int tracks_matched_by_path = 0;
  int tracks_matched_by_name = 0;
  int tracks_skipped = 0;            // matched, and merge was off
  int tracks_missing_file = 0;       // entries whose file is not on disk (imported anyway)
  int tracks_updated = 0;            // matched tracks that received something
  int cues = 0;                      // hot cues, memory cues and loops written
  int grids = 0;                     // tracks that received a beat grid
  int marks_ignored = 0;             // fade / load marks and malformed marks
  int playlists_created = 0;         // playlists and folders
  int playlists_merged = 0;          // already present
  int playlist_entries_added = 0;
  int playlist_entries_unresolved = 0;  // keys that name no track
  int history_entries = 0;
  std::vector<std::string> missing_paths;  // first 200 of the missing files
  std::vector<std::string> warnings;       // first 50
};

InterchangeStats import_library_xml(Library& lib, const std::string& path, const InterchangeImportOptions& options = {});

// ---- M3U8 -------------------------------------------------------------------------------------------------------
// A folder id exports every playlist below it, flattened, in tree order.
InterchangeExportStats export_playlist_m3u8(Library& lib, std::int64_t playlist_id, const std::string& path,
                                            bool relative_paths = false);
// One file per playlist, folders as directories, names made safe for every file system.
InterchangeExportStats export_all_playlists_m3u8(Library& lib, const std::string& dir, bool relative_paths = true);

struct M3uImportOptions {
  std::string playlist_name;   // empty: the #PLAYLIST line, else the file name without extension
  std::int64_t parent = 0;     // folder to create the playlist in (0 = top level)
  bool merge = true;           // an existing playlist of that name gets the missing tracks appended; false: left alone
  bool add_missing_tracks = true;  // list entries not in the library are added (flagged missing if the file is absent)
  bool dry_run = false;
};

InterchangeStats import_playlist_m3u8(Library& lib, const std::string& path, const M3uImportOptions& options = {});

// ---- helpers (also used by tests) -------------------------------------------------------------------------------
// "/Users/me/a b#1.mp3" -> "file://localhost/Users/me/a%20b%231.mp3"; "C:\Music\x.mp3" -> "file://localhost/C:/Music/x.mp3".
std::string interchange_path_to_url(const std::string& path);
// Accepts file://localhost/..., file:///..., file://host/... (host dropped), and plain paths. Percent-decodes.
std::string interchange_url_to_path(const std::string& url);

}  // namespace deckhand
