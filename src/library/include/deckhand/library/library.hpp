#pragma once
// DeckHand's own library: tracks, playlists, cues, beatgrids. Thread-safe for
// serialized use (SQLite full-mutex mode); callers keep queries short.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "deckhand/library/db.hpp"

namespace deckhand {

struct TrackRow {
  std::int64_t id = 0;
  std::string path;  // absolute, resolved from root + relative path
  std::string title, artist, album, genre, key, format;
  double bpm = 0;
  std::int64_t duration_ms = 0;
  int rating = 0;
};

struct PlaylistNode {
  std::int64_t id = 0;
  std::int64_t parent = 0;  // 0 = top level
  std::string name;
  bool is_folder = false;
  int track_count = 0;
};

enum class CueKind { Memory, Hot, Loop };

struct Cue {
  CueKind kind = CueKind::Memory;
  int slot = -1;  // hot cue slot (0-based), -1 for memory cues
  double start_ms = 0;
  double end_ms = -1;  // loops only
  std::string label;
};

struct BeatSection {
  double first_beat_ms = 0;
  double bpm = 0;
  int bar_beat = 1;  // 1..4
};

struct NewTrack {
  std::string path;  // absolute
  std::string title, artist, album, genre, key, comment;
  double bpm = 0;
  std::int64_t duration_ms = 0;
  int rating = 0, year = 0, bitrate = 0, sample_rate = 0;
};

class Library {
 public:
  explicit Library(const std::string& db_path);

  Db& db() { return db_; }

  std::int64_t track_count();
  std::vector<PlaylistNode> playlists();
  // playlist_id 0 = whole collection. search matches title / artist / album.
  std::vector<TrackRow> tracks(std::int64_t playlist_id, const std::string& search);
  std::optional<TrackRow> track(std::int64_t id);
  std::vector<Cue> cues(std::int64_t track_id);
  std::vector<BeatSection> beatgrid(std::int64_t track_id);

  // Insert, or update the metadata of the track at this path. Returns its id.
  std::int64_t upsert_track(const NewTrack& t);
  std::optional<std::int64_t> track_id_for_path(const std::string& abs_path);

  void set_hot_cue(std::int64_t track_id, int slot, double start_ms, double end_ms = -1);
  void clear_hot_cue(std::int64_t track_id, int slot);
  void set_bpm(std::int64_t track_id, double bpm);
  void replace_beatgrid(std::int64_t track_id, const std::vector<BeatSection>& grid);

 private:
  Db db_;
};

}  // namespace deckhand
