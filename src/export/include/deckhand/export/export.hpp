#pragma once
// USB export for club players: writes PIONEER/rekordbox/export.pdb, PIONEER/USBANLZ/... analysis files and
// (optionally) the audio, and reads the same back for verification. See docs/EXPORT.md for the spec
// sections followed and the list of guessed/undocumented fields. Depends on the C++ standard library only.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace deckhand::usbexport {

struct ExportBeat {
  std::int64_t time_ms = 0;
  double bpm = 0;
  int beat_in_bar = 1;  // 1..4
};

struct ExportCue {
  bool hot = false;                // false = memory point (or memory loop)
  int slot = 0;                    // hot cues: 1 (A) .. 8 (H); memory points: 0
  std::int64_t time_ms = 0;
  std::int64_t loop_end_ms = -1;   // >= 0 makes this a loop
  std::string label;
  std::uint8_t color_id = 0;       // colour table row id 0..8 (memory points / loops)
  bool has_rgb = false;            // hot cue colour for the player LEDs
  std::uint8_t r = 0, g = 0, b = 0;
};

// One waveform sample = 1/150 s; bytes are band levels 0..255.
struct WaveSample {
  std::uint8_t low = 0, mid = 0, high = 0;
};

// ---- stems written as ordinary tracks (see export_stems.hpp, docs/EXPORT.md "Stems on the stick") ----------------

enum StemMix : unsigned {
  kMixAcapella = 1,      // vocals
  kMixInstrumental = 2,  // drums + bass + other
  kMixDrums = 4,
  kMixBass = 8,
  kMixOther = 16,
};
constexpr unsigned kMixesAcapellaInstrumental = kMixAcapella | kMixInstrumental;
constexpr unsigned kMixesFourStems = kMixAcapella | kMixDrums | kMixBass | kMixOther;  // "all four"

// How a stem track's audio is produced: summed from a stem cache file (".dhstems", parsed directly, no dependency on
// the stems module). Stem order in the file: vocals 0, drums 1, bass 2, other 3; `stem_mask` has bit i for stem i.
struct StemRender {
  std::string stems_file;
  unsigned stem_mask = 0;
  int bits = 24;  // 16 or 24
  std::uint64_t frames = 0;
  int sample_rate = 0;
  int parent_id = 0;
  unsigned mix = 0;  // one StemMix value
};

struct ExportTrack {
  int id = 0;  // unique, > 0
  std::string source_path;
  std::string title, artist, album, genre, key, comment;
  double bpm = 0;
  std::int64_t duration_ms = 0;
  int sample_rate = 44100;
  int bitrate = 0;  // kbps
  int year = 0;
  int rating = 0;  // 0..5
  int track_number = 0;
  int color_id = 0;  // 0..8
  std::vector<ExportBeat> beats;
  std::vector<ExportCue> cues;
  std::vector<WaveSample> waveform;  // 150 per second
  std::vector<std::string> tags;     // "My Tag" names, written to exportExt.pdb when ExportOptions::write_ext_db
  // Set on the tracks export_usb derives from stems; callers leave it empty. Such a track has no source file: its
  // audio is rendered onto the stick.
  std::shared_ptr<const StemRender> stem;
};

struct ExportPlaylist {
  int id = 0;         // unique, > 0
  std::string name;
  int parent_id = 0;  // 0 = root; otherwise the id of a folder
  bool is_folder = false;
  std::vector<int> track_ids;  // ordered (ignored for folders)
};

enum class StemPlacement {
  MirrorFolder,  // default: stem tracks live in a "Stems" playlist folder mirroring the playlist tree
  Inline,        // stem tracks are placed right after their parent in every playlist containing it
};

struct StemOptions {
  unsigned mixes = 0;  // StemMix bits for every playlist without an entry in mixes_by_playlist; 0 = off
  // Per playlist (or folder: applies to everything inside it unless that has its own entry): the mixes for the
  // tracks in it. A track gets the union over the playlists that hold it. 0 turns stems off for that playlist.
  std::map<int, unsigned> mixes_by_playlist;
  int bits = 24;  // 16 or 24
  StemPlacement placement = StemPlacement::MirrorFolder;
  std::string folder_name = "Stems";  // MirrorFolder: name of the top-level folder
  // Caller-supplied: path of the ".dhstems" file holding this track's cached stems ("" = none cached). The export
  // module never reads the stem cache index itself.
  std::function<std::string(const ExportTrack&)> lookup;
  bool enabled() const { return lookup && (mixes != 0 || !mixes_by_playlist.empty()); }
};

struct StemNote {
  int track_id = 0;
  std::string title;
  std::string reason;  // "no stems cached", "stems file unreadable: ...", ...
};

struct StemReport {
  std::size_t tracks = 0;    // stem tracks in the export
  std::size_t reused = 0;    // of those, files already on the stick with identical content (not rewritten)
  std::uint64_t bytes = 0;   // audio bytes of the stem files to write (excluding reused ones)
  std::vector<StemNote> no_stems;  // parents without usable stems; "no stems cached" is the usual reason
};

struct ExportOptions {
  std::string destination;      // stick root
  bool copy_audio = true;       // copy to Contents/<Artist>/<Album>/<file>
  bool write_color_preview = true;  // PWV4 (entry layout is a guess, see docs/EXPORT.md)
  bool write_3band = true;          // ANLZ0000.2EX with PWV6/PWV7 (CDJ-3000)
  // EXPERIMENTAL: also write PIONEER/rekordbox/exportExt.pdb (tags and track-tag links). The public documentation
  // of that file is thin (see docs/EXPORT.md); default off.
  bool write_ext_db = false;
  std::string tag_category = "DeckHand";  // name of the single tag category used for ExportTrack::tags
  // Compatibility fix (see export_preflight.hpp, docs/EXPORT.md "Compatibility check"). Default off: files a club
  // player cannot read are still copied unchanged and reported. When on, WAV/AIFF files stored as 32-bit
  // integer or float are written to the stick as 24-bit PCM WAV (the source is never modified).
  bool convert_incompatible = false;
  std::string player_profile = "club";  // rule table used for the check; see player_profiles()
  StemOptions stems;                    // stem tracks (default off)
  // Test hooks. available_bytes_override >= 0 replaces the free-space query of the destination volume;
  // fail_after_commits >= 0 makes the commit phase fail after that many files were moved into place.
  std::int64_t available_bytes_override = -1;
  int fail_after_commits = -1;
};

bool export_usb(const std::vector<ExportTrack>& tracks, const std::vector<ExportPlaylist>& playlists,
                const ExportOptions& options, std::string& error);

struct ReadTrack {
  ExportTrack track;  // waveform left empty; duration_ms has whole-second resolution
  std::string label, file_path, analyze_path, ppth_dat, ppth_ext, isrc;
  unsigned file_type = 0;
  std::vector<ExportCue> dat_cues;  // from PCOB (DAT)
  std::vector<std::uint8_t> pwav, pwv2, pwv3;
  std::vector<std::uint16_t> pwv5;
  std::size_t pwv4_columns = 0, pwv6_columns = 0, pwv7_entries = 0, pvbr_entries = 0;
  bool has_ext = false, has_2ex = false;
};

struct ReadTag {
  std::uint32_t id = 0, category = 0, category_pos = 0;
  bool is_category = false;
  std::string name;
};

struct ReadExport {
  std::vector<ReadTrack> tracks;
  std::vector<ExportPlaylist> playlists;  // folders and playlists with ordered track ids
  std::vector<std::string> colors;        // colour table names, by row order
  std::vector<std::size_t> table_rows;    // valid row count per table type id
  std::uint32_t page_size = 0, num_pages = 0;
  // exportExt.pdb (empty unless the file exists)
  bool has_ext_db = false;
  std::vector<ReadTag> tags;                                      // categories and tags
  std::vector<std::pair<std::uint32_t, std::uint32_t>> tag_links;  // (track id, tag id)
  std::vector<std::size_t> ext_table_rows;                        // valid rows per table, in header order
};

bool read_export(const std::string& root, ReadExport& out, std::string& error);

}  // namespace deckhand::usbexport
