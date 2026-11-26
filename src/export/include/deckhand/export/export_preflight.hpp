#pragma once
// Export compatibility check: tells the user, before they get to the club, which files a player class cannot
// read (classic case: 32-bit float WAV), and converts the incompatible ones on the stick when asked.
// The rule tables are data (player_profiles()); each rule says whether it is `documented` or `unverified`.
// None of it has been checked against hardware: see docs/EXPORT.md, "Compatibility check".
#include <cstdint>
#include <string>
#include <vector>

#include "deckhand/export/export.hpp"
#include "deckhand/export/format_probe.hpp"

namespace deckhand::usbexport {

enum class Verdict { Ok, Warning, WillNotPlay };
enum class RuleSource { Documented, Unverified };

const char* verdict_name(Verdict v);      // "ok" | "warning" | "will_not_play"
const char* rule_source_name(RuleSource s);  // "documented" | "unverified"

// ---- player profiles (data) ---------------------------------------------------------------------------------

// A rule matches a probed format when every set field matches; the first matching rule of a profile wins.
struct FormatMatch {
  std::vector<Container> containers;  // empty = any
  std::vector<Codec> codecs;          // empty = any
  int is_float = -1;                  // -1 any, 0 integer, 1 float
  int bits_min = 0, bits_max = 0;     // 0 = unbounded
  int rate_min = 0, rate_max = 0;     // 0 = unbounded
  std::vector<int> rates;             // empty = any; otherwise the rate must be in (or, if rates_exclude, not in) it
  bool rates_exclude = false;
  int layer_min = 0, layer_max = 0;   // MP3 layer, 0 = unbounded
  int big_endian = -1;                // -1 any
};

struct FormatRule {
  std::string id;       // stable reason id, e.g. "pcm.float"
  FormatMatch match;
  Verdict verdict = Verdict::Ok;
  RuleSource source = RuleSource::Unverified;
  std::string message;  // plain language, shown to the user
};

struct PlayerProfile {
  std::string id;    // "club", "club_legacy"
  std::string name;  // user-facing
  std::vector<FormatRule> rules;
  std::vector<int> pcm_rates;           // sample rates the 24-bit WAV fix may keep (no resampling is done)
  std::uint64_t max_file_bytes = 0xFFFFFFFFull;  // FAT32 limit; 0 = no limit
  std::size_t max_path_bytes = 500;     // database path string limit (the path with its leading '/')
};

const std::vector<PlayerProfile>& player_profiles();
const PlayerProfile* find_player_profile(const std::string& id);  // nullptr if unknown
const PlayerProfile& default_player_profile();                     // "club"

// Hot cues the stick format cannot hold: it carries 8 (A-H), DeckHand decks have 16. Hot cues in slots 9-16 are left
// out of the export (never an error); the preflight reports them as "cues.not_exported" and the result counts them.
constexpr int kExportHotCues = 8;
inline std::size_t hot_cues_beyond_format(const ExportTrack& t) {
  std::size_t n = 0;
  for (const ExportCue& c : t.cues)
    if (c.hot && c.slot > kExportHotCues) ++n;
  return n;
}

// ---- report -------------------------------------------------------------------------------------------------

struct PreflightIssue {
  Verdict verdict = Verdict::Warning;
  std::string id;       // stable reason id: "pcm.float", "source.missing", "path.too_long", ...
  std::string message;  // plain language
  RuleSource source = RuleSource::Documented;  // is the rule behind this documented or unverified
  bool fixable = false;  // convert_incompatible can repair it (24-bit WAV copy on the stick)
};

struct TrackPreflight {
  int track_id = 0;
  std::string path;       // source file
  Verdict verdict = Verdict::Ok;  // worst of issues
  FormatInfo format;
  std::string format_text;        // describe_format(format)
  std::vector<PreflightIssue> issues;
  bool fixable = false;   // the blocking issue can be fixed by conversion
  bool converted = false; // filled in by export_usb: this track was written as 24-bit WAV
};

struct PreflightReport {
  std::string profile_id;
  std::vector<TrackPreflight> tracks;
  std::size_t ok = 0, warnings = 0, will_not_play = 0, fixable = 0;  // per track, worst verdict
  bool all_ok() const { return warnings == 0 && will_not_play == 0; }
  std::string text() const;  // one line per track with issues, plus a summary
  std::string to_json() const;
};

struct PreflightOptions {
  bool stick_is_fat32 = true;  // apply the FAT32 4 GB file limit
};

// Cheap, bounded-read check of every track against `profile`. Never throws; never modifies anything.
PreflightReport preflight_export(const std::vector<ExportTrack>& tracks, const PlayerProfile& profile,
                                 const PreflightOptions& options = {});

// ---- conversion (the fix path) ------------------------------------------------------------------------------

// What a conversion to 24-bit integer PCM WAV of a WAV/AIFF source would produce. Only 32-bit integer and
// 32/64-bit float PCM, 1 or 2 channels, at one of the profile's PCM rates (no resampling), can be converted.
// There is no decoder in this module (it must not depend on the audio module), so other formats (FLAC, ALAC, Ogg,
// ...) are NOT convertible here.
struct ConversionPlan {
  int sample_rate = 0, channels = 0, bytes_per_sample = 0;
  bool is_float = false, big_endian = false;
  std::uint64_t frames = 0;
  std::uint64_t out_bytes = 0;  // size of the written WAV file
  int bitrate_kbps = 0;         // of the written file
};

// Returns true and fills `plan` when `f` can be converted; otherwise false with `why` set (optional).
bool plan_pcm24_conversion(const FormatInfo& f, const PlayerProfile& profile, ConversionPlan& plan,
                           std::string* why = nullptr);

// Writes the converted file to `dest` (the caller supplies a temporary path). The source is only read.
bool convert_to_pcm24_wav(const std::string& source, const FormatInfo& f, const ConversionPlan& plan,
                          const std::string& dest, std::string& error);

// ---- export with a result -----------------------------------------------------------------------------------

struct ExportResult {
  PreflightReport report;  // computed before anything was written; tracks[i].converted is set afterwards
  std::size_t converted = 0;
  StemReport stems;                 // filled when ExportOptions::stems is enabled
  // Free-space check (always done): bytes the export needs (estimate, upper bound) and what the destination has.
  // Filled before anything is written, also when the export then fails for lack of space.
  std::uint64_t required_bytes = 0, available_bytes = 0;
  // Hot cues in slots 9-16 that were left out (the stick format carries 8), and the tracks they belong to.
  std::size_t cues_not_exported = 0, tracks_with_cues_not_exported = 0;
  // Tracks still marked will-not-play after the export (not converted: option off or not convertible).
  std::size_t remaining_will_not_play() const;
};

// Same as export_usb, plus the compatibility report. With options.convert_incompatible the fixable files are
// written to the stick as 24-bit WAV (the sources are never modified) and the database describes the converted
// files. The export proceeds even when the report contains will-not-play tracks.
bool export_usb(const std::vector<ExportTrack>& tracks, const std::vector<ExportPlaylist>& playlists,
                const ExportOptions& options, std::string& error, ExportResult& result);

}  // namespace deckhand::usbexport
