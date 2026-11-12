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
  std::vector<std::int64_t> playlist_ids;  // empty = every playlist
  bool copy_audio = true;
};

struct UsbExportResult {
  int tracks = 0, playlists = 0, missing_files = 0;
  std::vector<std::string> missing;  // first few missing paths
};

// progress(done, total, title) is called from the calling thread; return false from it to cancel.
bool export_library_to_usb(Library& lib, const UsbExportRequest& request, UsbExportResult& result, std::string& error,
                           const std::function<bool(int, int, const std::string&)>& progress = {});

}  // namespace deckhand
