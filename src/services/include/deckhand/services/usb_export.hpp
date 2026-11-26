#pragma once
// Builds a club-player USB export from the DeckHand library (glue between library, decoder and writer).
#include <cstdint>
#include <functional>
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
};

// Smart playlists get this offset on the stick so their ids cannot meet a normal playlist's.
constexpr std::int64_t kSmartExportIdBase = 1000000;

struct UsbExportResult {
  int tracks = 0, playlists = 0, missing_files = 0;
  std::vector<std::string> missing;  // first few missing paths
};

// progress(done, total, title) is called from the calling thread; return false from it to cancel.
bool export_library_to_usb(Library& lib, const UsbExportRequest& request, UsbExportResult& result, std::string& error,
                           const std::function<bool(int, int, const std::string&)>& progress = {});

}  // namespace deckhand
