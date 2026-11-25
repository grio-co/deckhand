#pragma once
// Structural validator for a written USB export and a comparison harness for the day a vendor-made reference
// export is available. Both read the stick only (no writer bookkeeping). See docs/EXPORT.md.
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace deckhand::usbexport {

enum class Severity { Warning, Error };

struct Issue {
  Severity severity = Severity::Error;
  std::string code;  // stable identifier, e.g. "pdb.chain_loop"
  std::string message;
};

struct ValidationReport {
  std::vector<Issue> issues;
  std::size_t tables_checked = 0, tracks_checked = 0, files_checked = 0;

  std::size_t errors() const;
  std::size_t warnings() const;
  bool ok() const { return errors() == 0; }
  bool has(const std::string& code) const;  // any issue (error or warning) with this code
  void add(Severity s, std::string code, std::string message);
  std::string text() const;                 // one line per issue plus a summary
};

struct ValidateOptions {
  bool check_audio = true;  // audio files exist with the recorded size
  bool check_anlz = true;   // analysis files exist and parse
};

// Walks root/PIONEER/rekordbox/export.pdb (and exportExt.pdb when present), the ANLZ files and the audio files.
// Returns report.ok(). Never throws.
bool validate_export(const std::string& root, ValidationReport& report, const ValidateOptions& options = {});

// ---- reference comparison ----------------------------------------------------------------------------------

struct CompareReport {
  struct FieldDiff {
    std::string where;  // e.g. "track 'song.mp3'" or "table 1 'House'"
    std::string field;
    std::string reference, ours;
  };
  struct Histogram {  // values the reference uses for a field DeckHand currently guesses
    std::string field;
    std::map<std::string, std::size_t> reference;
    std::string ours;  // value DeckHand writes (first matched row)
  };
  std::size_t matched_tracks = 0;
  std::vector<std::string> unmatched_reference, unmatched_ours;  // track keys
  std::vector<std::string> structure;  // page counts, row group layout, header values, file inventory
  std::vector<FieldDiff> fields;       // field-level differences of matched rows
  std::vector<Histogram> histograms;
  std::vector<std::string> anlz;       // per file tag inventory differences
  bool identical() const { return structure.empty() && fields.empty() && anlz.empty() && unmatched_reference.empty() && unmatched_ours.empty(); }
  std::string text() const;
};

// Parses both exports and reports differences. Tracks are matched by audio file name, falling back to title.
// Returns false (with `error`) only when one of the exports cannot be read at all.
bool compare_exports(const std::string& reference_root, const std::string& ours_root, CompareReport& report,
                     std::string& error);

// ---- helpers exposed for tests -----------------------------------------------------------------------------

// Deterministic FAT32/exFAT-safe file or directory name (forbidden characters, control characters, non-BMP
// characters, trailing dots/spaces, reserved DOS device names, length cap in bytes).
std::string sanitize_fs_name(const std::string& name, std::size_t max_bytes, const char* fallback);

// Reasons a path (relative or absolute, '/' separated) is not valid on FAT32/exFAT; empty = valid.
std::vector<std::string> fat_path_problems(const std::string& path);

}  // namespace deckhand::usbexport
