#pragma once
// DeckHand's own library: tracks, playlists, cues, beatgrids. Thread-safe for
// serialized use (SQLite full-mutex mode); callers keep queries short.
#include <cstdint>
#include <optional>
#include <set>
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
  // appended (schema 4 reads): what the browser columns show
  std::string comment;
  int color = 0;               // 0 = none, else 0xRRGGBB
  std::int64_t date_added = 0; // unix seconds
  int bitrate = 0;
  bool grid_locked = false;    // analysis never replaces a locked grid
  int play_count = 0;          // history entries of this track
  std::int64_t last_played_ms = 0;  // 0 = never
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
  int color = -1;      // 0xRRGGBB, -1 = none (a hot cue then takes its slot colour)
};

struct BeatSection {
  double first_beat_ms = 0;
  double bpm = 0;
  int bar_beat = 1;  // 1..4
};

enum class SortKey { Default, Title, Artist, Album, Genre, Bpm, Key, Duration, Rating, DateAdded,
                     Comment, Color, Format, Bitrate, PlayCount, LastPlayed };

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

// One condition of a smart playlist or a quick filter. Every field is evaluated by SQL, except Missing, which asks the
// file system about the rows that matched the rest.
struct SmartRule {
  enum class Field { Text, Bpm, Key, Genre, Rating, Color, AddedDays, Plays, InPlaylist, Missing, BpmNear };
  Field field = Field::Text;
  bool negate = false;      // NOT: text does not contain, not in the playlist, file present, key not in the set ...
  std::string text;         // Text: substring of title / artist / album / comment / path; Genre: substring; Key: list "8A,9A"
  double lo = 0, hi = 0;    // Bpm: range (hi 0 = open end); Rating: minimum in lo; AddedDays: lo = days; Plays: lo = at least,
                            //   hi = at most (hi < 0 = no upper limit); Color: lo = colour (0 = no colour tag)
  std::int64_t id = 0;      // InPlaylist: playlist id
                            // BpmNear (quick filter, not shown in the editor): lo = a tempo, hi = tolerance as a fraction (0.06);
                            //   matches that tempo and its half and double
};
struct SmartPlaylist {
  std::int64_t id = 0;
  std::string name;
  bool match_all = true;    // all rules (AND) or any rule (OR)
  std::vector<SmartRule> rules;
};
std::string smart_rules_json(const std::vector<SmartRule>& rules);
// False (rules cleared) when the text is not what smart_rules_json writes.
bool smart_rules_from_json(const std::string& json, std::vector<SmartRule>& rules);

class Library {
 public:
  explicit Library(const std::string& db_path);

  Db& db() { return db_; }

  std::int64_t track_count();
  std::vector<PlaylistNode> playlists();
  // playlist_id 0 = whole collection. search matches title / artist / album.
  std::vector<TrackRow> tracks(std::int64_t playlist_id, const std::string& search,
                               SortKey sort = SortKey::Default, bool descending = false);
  // Same ordering and filtering as tracks(), but ids only (no row building): for large libraries the track table
  // keeps the ids and fetches just the rows on screen with track().
  std::vector<std::int64_t> track_ids(std::int64_t playlist_id, const std::string& search, SortKey sort = SortKey::Default,
                                      bool descending = false);
  std::optional<TrackRow> track(std::int64_t id);
  std::vector<Cue> cues(std::int64_t track_id);
  std::vector<BeatSection> beatgrid(std::int64_t track_id);

  // Insert, or update the metadata of the track at this path. Returns its id.
  std::int64_t upsert_track(const NewTrack& t);
  std::optional<std::int64_t> track_id_for_path(const std::string& abs_path);

  // A hot cue keeps its name and colour when it is set again (moved); clear_hot_cue forgets them.
  void set_hot_cue(std::int64_t track_id, int slot, double start_ms, double end_ms = -1);
  void clear_hot_cue(std::int64_t track_id, int slot);
  // Name and colour of a hot cue (the cue must exist). color -1 = slot colour.
  void set_hot_cue_style(std::int64_t track_id, int slot, const std::string& label, int color);
  // Memory cues and saved loops (kind Memory / Loop without a slot), replaced as a set. Hot cues are not touched.
  std::vector<Cue> memory_cues(std::int64_t track_id);
  void replace_memory_cues(std::int64_t track_id, const std::vector<Cue>& cues);
  // Grid lock: analysis (and re-analysis) leaves a locked grid alone; hand edits lock it.
  bool grid_locked(std::int64_t track_id);
  void set_grid_locked(std::int64_t track_id, bool locked);
  // Track colour tag: 0 = none, else 0xRRGGBB.
  void set_track_color(std::int64_t track_id, int color);
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

  // Smart playlists: saved rule sets, evaluated when asked (they always reflect the library as it is now).
  std::vector<SmartPlaylist> smart_playlists();
  std::optional<SmartPlaylist> smart_playlist(std::int64_t id);
  std::int64_t create_smart_playlist(const std::string& name, bool match_all, const std::vector<SmartRule>& rules);
  void update_smart_playlist(const SmartPlaylist& p);  // name, match mode and rules of p.id
  void delete_smart_playlist(std::int64_t id);
  // Tracks matching rules (all of them or any), then the text search, in the order tracks() would give.
  std::vector<std::int64_t> query_ids(const std::vector<SmartRule>& rules, bool match_all, const std::string& search = {},
                                      SortKey sort = SortKey::Default, bool descending = false);
  std::vector<std::int64_t> smart_track_ids(std::int64_t smart_id, const std::string& search = {},
                                            SortKey sort = SortKey::Default, bool descending = false);

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

  // True when the substring search index exists. SQLite builds without FTS5 (some distribution
  // packages) run without it: search falls back to a scan and the schema stays at version 2.
  bool search_indexed() const { return fts_; }

 private:
  void migrate();
  bool use_fts(const std::string& search) const;
  std::string search_pattern(const std::string& search) const;
  const std::set<std::int64_t>& missing_ids();  // tracks whose file is gone (rescanned at most every 5 s)
  std::set<std::int64_t> missing_ids_;
  std::int64_t missing_at_ = 0;
  bool fts_ = false;  // tracks_fts (FTS5 trigram) exists: substring search of 3+ characters uses it
  Db db_;
};

}  // namespace deckhand
