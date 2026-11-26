#pragma once
// Stems on the stick: club players cannot separate stems, so each exported track that has cached stems can bring
// its acapella / instrumental / single stems along as ordinary PCM WAV tracks. See docs/EXPORT.md, "Stems on the
// stick". The stem files are read straight from the stem cache's ".dhstems" layout (header documented in
// src/stems/include/deckhand/stems/cache.hpp); this module does not link the stems module.
#include <string>
#include <vector>

#include "deckhand/export/export.hpp"

namespace deckhand::usbexport {

// "off" | "acapella+instrumental" | "all" / "four" (acapella, drums, bass, other) | "everything" (all five mixes) |
// a list of acapella|vocals|instrumental|drums|bass|other separated by '+', ',' or space. Case-insensitive.
bool parse_stem_mixes(const std::string& text, unsigned& mixes, std::string& error);
std::string stem_mixes_text(unsigned mixes);  // "acapella+instrumental", "off", ...
const char* stem_mix_label(unsigned single_mix);  // "Acapella", "Instrumental", "Drums", "Bass", "Other"

// The extra tracks and the playlist set of an export with stems. `tracks` holds only the stem tracks (the parents
// stay with the caller); `playlists` is the complete replacement playlist list (originals, with the stem entries
// placed inline, or plus the mirrored folder). Nothing is written to disk. Deterministic: the same input gives the
// same ids, so a re-export finds its earlier files.
struct StemExpansion {
  std::vector<ExportTrack> tracks;
  std::vector<ExportPlaylist> playlists;
  StemReport report;
};
bool expand_stems(const std::vector<ExportTrack>& tracks, const std::vector<ExportPlaylist>& playlists,
                  const ExportOptions& options, StemExpansion& out, std::string& error);

// Size of the WAV file a stem track becomes (44-byte header + data).
std::uint64_t stem_wav_bytes(std::uint64_t frames, int bits);

}  // namespace deckhand::usbexport
