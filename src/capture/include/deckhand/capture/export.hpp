#pragma once
// Export: split pieces of a recording, or an edited copy of a decoded track, to WAV or AIFF with tags. The original is
// never written to. FLAC needs an encoder and the tree has none (flac_available() is false): asking for it is an error.
#include <cstdint>
#include <string>
#include <vector>

#include "deckhand/capture/cleanup.hpp"
#include "deckhand/capture/source.hpp"
#include "deckhand/capture/split.hpp"

namespace deckhand::capture {

enum class Container { Wav, Aiff, Flac };
bool flac_available();
const char* container_extension(Container c);  // "wav", "aiff", "flac"

struct ExportSettings {
  Container container = Container::Wav;
  SampleFormat format = SampleFormat::Pcm24;  // AIFF: Pcm16 or Pcm24
  CleanupSettings cleanup;                    // off by default; with everything off a piece is the source samples, untouched
  bool overwrite = false;                     // refuse to replace an existing file
};

struct ExportResult {
  bool ok = false;
  std::string path, error;
  std::uint64_t start = 0, end = 0, frames = 0;
  CleanupReport cleanup;
};

// Makes "NN - Artist - Title" safe for every file system (no separators or reserved characters, no trailing dots).
std::string safe_filename(const std::string& s);
std::string piece_file_name(const TrackTags& tags, int track_number, Container c);

// Writes frames [start, end) of `src` to `path` (written to a temporary name first, then renamed).
ExportResult export_range(SampleSource& src, std::uint64_t start, std::uint64_t end, const std::string& path, const ExportSettings& s,
                          const TrackTags* tags = nullptr);

// Writes every non-empty piece into `dir`, named from its tags. `tags` is matched to the non-empty pieces in order
// (missing entries get "Track NN"; a zero track_number is filled in, and track_total with the number of pieces).
std::vector<ExportResult> export_pieces(SampleSource& src, const std::vector<Piece>& pieces, const std::vector<TrackTags>& tags,
                                        const std::string& dir, const ExportSettings& s);

// "Export an edited track": trims / fades / normalises a decoded track into a new file. Refuses to write over `source_path`
// (the original, compared as the same file) or over any existing file unless s.overwrite.
ExportResult export_edit(SampleSource& src, const std::string& source_path, double start_s, double end_s, const std::string& out_path,
                         const ExportSettings& s, const TrackTags* tags = nullptr);

}  // namespace deckhand::capture
