#pragma once
// Read-only import from other DJ libraries. The source is never opened in place:
// it is copied to a scratch directory first and only the copy is read.
#include <functional>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"

namespace deckhand {

struct ImportOptions {
  std::string source_db;      // path to the source library database (v6 format)
  std::string key;            // database key, supplied by the user's local configuration
  std::string analysis_root;  // folder that analysis file paths are relative to
  std::string scratch_dir;    // where the working copy is placed
};

struct ImportStats {
  int tracks_added = 0;
  int tracks_updated = 0;
  int playlists = 0;
  int cues = 0;
  int beatgrids = 0;
};

// Throws std::runtime_error on failure. Safe to run repeatedly: existing tracks are
// updated, cues and grids edited in DeckHand are kept.
ImportStats import_library_v6(Library& lib, const ImportOptions& options);

// True when this build can open encrypted source libraries.
bool import_v6_available();

// Parse the beat grid out of an analysis file. Empty on any error.
std::vector<BeatSection> read_analysis_beatgrid(const std::string& analysis_file);

}  // namespace deckhand
