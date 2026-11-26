#pragma once
// Builds a club-player USB export from the DeckHand library (glue between library, decoder and writer).
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"

namespace deckhand {

struct UsbExportRequest {
  std::string destination;                 // root of the stick (or any folder)
  std::vector<std::int64_t> playlist_ids;  // empty = every playlist (and every smart playlist)
  // Smart playlists to export as ordinary playlists (their tracks as they are now). Used when playlist_ids is not empty;
  // with playlist_ids empty every smart playlist goes.
  std::vector<std::int64_t> smart_playlist_ids;
  bool copy_audio = true;
  bool convert_incompatible = false;  // write 32-bit / float WAV and AIFF as 24-bit WAV on the stick (see EXPORT.md)
  std::string player_profile;         // rule table of the compatibility check; empty = "club"
  // Stems as ordinary tracks on the stick (see EXPORT.md, "Stems on the stick"). stem_mixes is a bit set of
  // usbexport::StemMix (1 acapella, 2 instrumental, 4 drums, 8 bass, 16 other; 0 = off).
  unsigned stem_mixes = 0;
  std::map<std::int64_t, unsigned> stem_mixes_by_playlist;  // playlist id -> mixes (overrides stem_mixes inside it)
  int stem_bits = 24;                                        // 16 or 24
  bool stems_inline = false;                                 // false: "Stems" folder mirroring the playlists
  std::string stems_folder = "Stems";
  // Track id + file path -> path of its .dhstems file ("" = no stems cached). stem_lookup_from_data_dir() builds one.
  std::function<std::string(std::int64_t, const std::string&)> stem_lookup;
};

// Lookup over the application's stem cache index (<data dir>/stems/tracks.tsv: "<track id>\t<key>\t<model>" per line;
// the file is read once per call). Only files that exist are returned.
std::function<std::string(std::int64_t, const std::string&)> stem_lookup_from_data_dir(const std::string& data_dir);

// Smart playlists get this offset on the stick so their ids cannot meet a normal playlist's.
constexpr std::int64_t kSmartExportIdBase = 1000000;

struct UsbExportResult {
  int tracks = 0, playlists = 0, missing_files = 0;
  std::vector<std::string> missing;  // first few missing paths
  // Compatibility check of the export (PreflightReport::to_json()), empty when the export failed before it ran.
  std::string report_json;
  int converted = 0;               // tracks written as 24-bit WAV
  int remaining_will_not_play = 0; // tracks a club player still will not play
  int stem_tracks = 0;             // stem tracks written (or reused) on the stick
  std::vector<std::string> no_stems;  // "<title>: no stems cached" (and other reasons), one per track without usable stems
  int cues_not_exported = 0;       // hot cues in slots 9-16: the stick format carries 8, the rest is left out (never an error)
  std::uint64_t required_bytes = 0, available_bytes = 0;  // free-space check of the export (set even when it fails)
};

// progress(done, total, title) is called from the calling thread; return false from it to cancel.
bool export_library_to_usb(Library& lib, const UsbExportRequest& request, UsbExportResult& result, std::string& error,
                           const std::function<bool(int, int, const std::string&)>& progress = {});

}  // namespace deckhand
