#pragma once
// USB export for club players: writes PIONEER/rekordbox/export.pdb, PIONEER/USBANLZ/... analysis files and
// (optionally) the audio, and reads the same back for verification. See docs/EXPORT.md for the spec
// sections followed and the list of guessed/undocumented fields. Depends on the C++ standard library only.
#include <cstdint>
#include <string>
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
};

struct ExportPlaylist {
  int id = 0;         // unique, > 0
  std::string name;
  int parent_id = 0;  // 0 = root; otherwise the id of a folder
  bool is_folder = false;
  std::vector<int> track_ids;  // ordered (ignored for folders)
};

struct ExportOptions {
  std::string destination;      // stick root
  bool copy_audio = true;       // copy to Contents/<Artist>/<Album>/<file>
  bool write_color_preview = true;  // PWV4 (entry layout is a guess, see docs/EXPORT.md)
  bool write_3band = true;          // ANLZ0000.2EX with PWV6/PWV7 (CDJ-3000)
};

bool export_usb(const std::vector<ExportTrack>& tracks, const std::vector<ExportPlaylist>& playlists,
                const ExportOptions& options, std::string& error);

struct ReadTrack {
  ExportTrack track;  // waveform left empty; duration_ms has whole-second resolution
  std::string label, file_path, analyze_path, ppth_dat, ppth_ext;
  unsigned file_type = 0;
  std::vector<ExportCue> dat_cues;  // from PCOB (DAT)
  std::vector<std::uint8_t> pwav, pwv2, pwv3;
  std::vector<std::uint16_t> pwv5;
  std::size_t pwv4_columns = 0, pwv6_columns = 0, pwv7_entries = 0, pvbr_entries = 0;
  bool has_ext = false, has_2ex = false;
};

struct ReadExport {
  std::vector<ReadTrack> tracks;
  std::vector<ExportPlaylist> playlists;  // folders and playlists with ordered track ids
  std::vector<std::string> colors;        // colour table names, by row order
  std::vector<std::size_t> table_rows;    // valid row count per table type id
  std::uint32_t page_size = 0, num_pages = 0;
};

bool read_export(const std::string& root, ReadExport& out, std::string& error);

}  // namespace deckhand::usbexport
