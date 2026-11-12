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

enum class SortKey { Default, Title, Artist, Album, Genre, Bpm, Key, Duration, Rating, DateAdded };

// Fields left as nullopt are not changed.
struct TrackEdit {
  std::optional<std::string> title, artist, album, genre, key, comment;
  std::optional<int> rating, year;
};

struct HistoryEntry {
  std::int64_t id = 0, session_id = 0, track_id = 0;  // track_id 0: track since removed
  std::string deck, title, artist;
  std::int64_t started_at_ms = 0, played_ms = 0;
};

struct HistorySession {
  std::int64_t id = 0, started_at_ms = 0;
  int entries = 0;
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
  std::vector<TrackRow> tracks(std::int64_t playlist_id, const std::string& search,
                               SortKey sort = SortKey::Default, bool descending = false);
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

  // Tags, analysis results, file location.
  void edit_track(std::int64_t track_id, const TrackEdit& edit);
  void set_key(std::int64_t track_id, const std::string& key);
  void set_gain_db(std::int64_t track_id, double gain_db);
  std::optional<double> gain_db(std::int64_t track_id);
  void set_track_path(std::int64_t track_id, const std::string& abs_path);
  // Rewrites every track path starting with old_prefix. Returns the number changed.
  int relink_prefix(const std::string& old_prefix, const std::string& new_prefix);
  void remove_track(std::int64_t track_id);

  // Playlists. Positions are 0-based; position -1 appends.
  std::int64_t create_playlist(std::int64_t parent, const std::string& name, bool is_folder);
  void rename_playlist(std::int64_t id, const std::string& name);
  void delete_playlist(std::int64_t id);
  // Returns false when the move would put a folder inside itself.
  bool move_playlist(std::int64_t id, std::int64_t new_parent, int position);
  std::vector<std::int64_t> playlist_track_ids(std::int64_t playlist_id);
  void set_playlist_tracks(std::int64_t playlist_id, const std::vector<std::int64_t>& track_ids);
  void add_to_playlist(std::int64_t playlist_id, std::int64_t track_id, int position = -1);
  void remove_from_playlist(std::int64_t playlist_id, int position);
  void move_in_playlist(std::int64_t playlist_id, int from, int to);

  // Play history. A session is one run of the application.
  std::int64_t begin_history_session();
  std::int64_t add_history(std::int64_t session_id, std::int64_t track_id, const std::string& deck,
                           std::int64_t started_at_ms);
  void set_history_played(std::int64_t history_id, std::int64_t played_ms);
  std::vector<HistorySession> history_sessions();
  std::vector<HistoryEntry> history(std::int64_t session_id);

  // Small key/value store for application settings.
  std::string setting(const std::string& key, const std::string& fallback = {});
  void set_setting(const std::string& key, const std::string& value);

 private:
  void migrate();
  Db db_;
};

}  // namespace deckhand
