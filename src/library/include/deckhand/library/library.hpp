#pragma once
// DeckHand's own library: tracks, playlists, cues, beatgrids. Thread-safe for
// serialized use (SQLite full-mutex mode); callers keep queries short.
#include <cstdint>
#include <functional>
#include <map>
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

// Where a track starts when it is loaded: the user's own main cue wins, then what analysis found.
struct TrackCues {
  std::optional<double> main_cue_ms;    // set by the user (the main cue point)
  std::optional<double> load_cue_ms;    // analysis: first confident downbeat at or after the first sound
  std::optional<double> first_sound_ms; // analysis: first audible moment
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

// One row of the edit journal: a single field of a track that changed. Values are the column's text form; nullopt = NULL.
struct TrackEditRecord {
  std::int64_t id = 0, track_id = 0;
  std::int64_t batch_id = 0;  // edits made by one operation share it
  std::string field;          // title, artist, album, genre, key, comment, rating, year, color, bpm, grid_locked, and the
                              // whole-set fields "grid" (tempo sections) and "cues" (every cue and loop), see grid_text()
  std::optional<std::string> old_value, new_value;
  std::int64_t edited_at = 0;  // unix seconds
  std::string source;          // 'user' | 'api' | 'import' | 'sync' | 'undo'
};

enum class UndoStatus { Ok, Conflict, NotFound };
struct UndoResult {
  UndoStatus status = UndoStatus::NotFound;
  int applied = 0;           // fields restored
  int conflicts = 0;         // fields that no longer hold the value the edit set (nothing is applied then)
  std::int64_t batch_id = 0;  // the journal batch the undo itself was recorded in
};

// A track removed from the collection, kept for restore_track() until purge_deleted() drops it.
struct DeletedTrack {
  std::int64_t id = 0;
  std::string path, title, artist;
  std::int64_t deleted_at = 0;  // unix seconds
};

// Names the origin of the library edits made on this thread while it lives ('user' when no scope is open), and groups
// them into one journal batch. Scopes nest: an inner scope keeps the outer source and batch unless `force` is set.
struct ResumeBatch {};
class EditScope {
 public:
  explicit EditScope(const char* source, bool force = false);
  // Continues an earlier journal batch (so one undo_batch covers both): later edits on this thread join `batch`.
  EditScope(const char* source, ResumeBatch, std::int64_t batch);
  ~EditScope();
  EditScope(const EditScope&) = delete;
  EditScope& operator=(const EditScope&) = delete;
  std::int64_t batch() const;  // 0 until the first journalled change
 private:
  std::string saved_source_;
  std::int64_t saved_batch_ = 0;
  int saved_depth_ = 0;
  bool forced_ = false;
};

// ---- cues that follow the grid -------------------------------------------------------------------------------------
// Compact text forms of a grid and of a whole cue set, as the edit journal stores them (round-trip exactly).
std::string grid_text(const std::vector<BeatSection>& grid);
std::vector<BeatSection> grid_from_text(const std::string& text);
std::string cues_text(const std::vector<Cue>& cues);
std::vector<Cue> cues_from_text(const std::string& text);

enum class CueFollow { Ask, Always, Never };  // setting "cues_follow_grid"
CueFollow cue_follow_from(const std::string& text, CueFollow fallback = CueFollow::Ask);
const char* cue_follow_name(CueFollow f);
constexpr double kCueGridToleranceMs = 15;

struct CueMove {
  Cue before, after;
  double beats = 0;  // position in beats from the first beat of its tempo section (old grid)
};
struct CueFollowProposal {
  std::vector<CueMove> moves;     // on the old grid, and the new grid puts that beat somewhere else
  std::vector<Cue> off_grid;      // not within the tolerance of a beat, half or quarter beat of the old grid: stay put
  std::vector<Cue> unchanged;     // on the grid and the new grid puts them in the same place (halve / double, bar shift)
  bool no_grid = false;           // the old or the new grid is empty: nothing can be mapped
  double tolerance_ms = kCueGridToleranceMs;
};
// Pure: which cues of `cues` move when the grid changes from `old_grid` to `new_grid`. A loop keeps its length in beats.
CueFollowProposal propose_cue_follow(const std::vector<Cue>& cues, const std::vector<BeatSection>& old_grid,
                                     const std::vector<BeatSection>& new_grid, double tolerance_ms = kCueGridToleranceMs);

struct GridCommit {
  std::int64_t batch_id = 0;  // journal batch of the whole edit; undo_batch() restores grid, tempo, lock and cues together
  bool grid_changed = false;
  CueFollow mode = CueFollow::Never;
  bool applied = false;       // the cue moves were made (mode Always)
  CueFollowProposal proposal; // what moves (applied, or offered under Ask)
};
enum class FollowStatus { Ok, NoGridEdit, AlreadyApplied, Conflict };
struct FollowResult {
  FollowStatus status = FollowStatus::NoGridEdit;
  CueFollowProposal proposal;
  std::int64_t batch_id = 0;
};

// ---- analysis removal -------------------------------------------------------------------------------------------------
struct StripKinds {
  bool grid = false, key = false, loudness = false, waveform = false, first_sound = false, phrases = false, vocals = false, stems = false;
};
struct StripResult {
  bool dry_run = false;
  std::int64_t batch_id = 0;
  int tracks = 0;           // looked at
  int tracks_changed = 0;   // at least one thing cleared
  std::map<std::string, int> cleared;  // kind -> tracks it was cleared on (or would be)
  int skipped_grid_locked = 0;         // grid left alone: locked and not forced
  int skipped_user_key = 0;            // key left alone: the user set it by hand and not forced
  std::vector<std::string> notes;      // kinds not available / nothing to do, and which kinds undo does not restore
};

// ---- duplicates ------------------------------------------------------------------------------------------------------
struct DuplicateMember {
  TrackRow row;
  std::int64_t file_size = 0;  // 0 unknown
  int sample_rate = 0;
  bool lossless = false;
  int cue_count = 0;
  bool has_grid = false;
};
struct DuplicateGroup {
  std::string kind;                    // "exact" (same bytes) | "likely" (same artist, title and length)
  double confidence = 0;               // 1 for exact
  std::vector<DuplicateMember> members;
  std::vector<std::string> differences;  // which of format, bitrate, sample_rate, duration, size, bpm, key differ
  std::int64_t suggested_keep = 0;     // a suggestion only: nothing is merged without merge_duplicates()
  std::string suggestion_reason;
};
struct DuplicateOptions {
  bool exact = true, likely = true;
  double duration_tolerance_ms = 2000;
  bool same_bpm = false, same_key = false;  // likely: also require equal tempo (within 0.5) / key
  std::size_t limit = 0;                    // groups returned, 0 = all
};
struct HashProgress {
  int sized = 0, hashed = 0, missing = 0;  // done by this call
  int remaining = 0;                        // tracks still without a size, plus same-size candidates still without a hash
};
struct MergeOptions {
  enum class Cues { Missing, All, None };
  Cues cues = Cues::Missing;  // Missing: copy cues / loops / grid only where the kept track has none; All: also add what it lacks
  bool fill_metadata = true;  // tags, rating, colour, year, tempo, key where the kept track has none
  bool combine_history = true;
  bool dry_run = false;
};
struct MergeResult {
  bool ok = false;
  std::string error;
  bool dry_run = false;
  std::int64_t batch_id = 0;   // journal batch (tags, cues, grid). Playlists, history and trash are not part of it.
  std::int64_t kept = 0;
  std::vector<std::int64_t> removed;
  int playlists_repointed = 0, playlists_already_had = 0;
  int hot_cues_copied = 0, memory_cues_copied = 0, loops_copied = 0;
  bool grid_copied = false;
  int history_moved = 0;
  std::vector<std::string> fields_filled;
  std::vector<std::string> warnings;
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

  // Insert, or update the metadata of the track at this path. Returns its id. An update leaves alone every field whose
  // latest journal entry is a user or API edit, unless overwrite_edited is set.
  std::int64_t upsert_track(const NewTrack& t, bool overwrite_edited = false);
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
  // Load cue. Not journalled (a cue point is not metadata anyone re-imports).
  TrackCues track_cues(std::int64_t track_id);
  void set_main_cue_ms(std::int64_t track_id, std::optional<double> ms);
  void set_analysis_cues(std::int64_t track_id, std::optional<double> load_cue_ms, std::optional<double> first_sound_ms);
  void replace_beatgrid(std::int64_t track_id, const std::vector<BeatSection>& grid);
  // A hand-edited grid, as one journalled batch: sections, tempo (`bpm`, when given), lock (when given) and, by `mode`,
  // the cues that follow it (Always: moved now; Ask: only proposed, apply_cue_follow() moves them; Never: left).
  GridCommit commit_grid(std::int64_t track_id, const std::vector<BeatSection>& grid, std::optional<double> bpm,
                         std::optional<bool> lock, CueFollow mode, double tolerance_ms = kCueGridToleranceMs,
                         bool dry_run = false);
  // Second call of the Ask flow: moves the cues for the grid edit of journal batch `batch_id`, inside that same batch.
  FollowResult apply_cue_follow(std::int64_t track_id, std::int64_t batch_id, double tolerance_ms = kCueGridToleranceMs);
  // Replaces every cue and loop of the track (hot, memory, loops); journalled as one "cues" row.
  void replace_all_cues(std::int64_t track_id, const std::vector<Cue>& cues);

  // Removes derived data only (never tags, cues, playlists, the main cue). One journal batch; key, tempo, grid and lock
  // are undoable, loudness, waveform cache, first-sound / load-cue are not (analysis re-creates them).
  StripResult strip_analysis(const std::vector<std::int64_t>& ids, const StripKinds& kinds, bool force = false, bool dry_run = false);

  // Duplicates. hash_pending() does a bounded amount of work (file sizes first, then content hashes of files that share a
  // size with another track); call it until `remaining` is 0 (limit 0 only counts). Nothing runs when the library opens.
  HashProgress hash_pending(int limit = 50);
  std::vector<DuplicateGroup> find_duplicates(const DuplicateOptions& options = {});
  MergeResult merge_duplicates(std::int64_t keep_id, const std::vector<std::int64_t>& remove_ids, const MergeOptions& options = {});

  // Tags, analysis results, file location.
  void edit_track(std::int64_t track_id, const TrackEdit& edit);
  void set_key(std::int64_t track_id, const std::string& key);
  void set_gain_db(std::int64_t track_id, double gain_db);
  std::optional<double> gain_db(std::int64_t track_id);
  void set_track_path(std::int64_t track_id, const std::string& abs_path);
  // Rewrites every track path starting with old_prefix. Returns the number changed.
  int relink_prefix(const std::string& old_prefix, const std::string& new_prefix);
  // Moves the track (cues, grid, playlist places) to the trash; restore_track() puts it back.
  void remove_track(std::int64_t track_id);
  std::vector<DeletedTrack> deleted_tracks();
  // False when the track is not in the trash or another track now has its file. Returns the id in `restored` (usually
  // the old one).
  bool restore_track(std::int64_t track_id, std::int64_t* restored = nullptr);
  // Drops trash entries (and their edit history) older than the given age. Runs when the library opens.
  int purge_deleted(int older_than_days = 30);

  // Edit journal. Newest first. Entries with source 'analysis' (DeckHand's own analysis results) are left out unless asked
  // for: they are not edits the user made, and undo lists should not offer them.
  std::vector<TrackEditRecord> track_history(std::int64_t track_id, int limit = 50, bool include_analysis = false);
  std::vector<TrackEditRecord> recent_edits(int limit = 50, bool include_analysis = false);
  // For code that writes the tracks table itself (the sync engine): take a snapshot, change the row, then journal what
  // differs under the current EditScope.
  using FieldValues = std::vector<std::optional<std::string>>;
  FieldValues edit_snapshot(std::int64_t track_id);
  void edit_journal(std::int64_t track_id, const FieldValues& before);
  // Fields whose latest journal entry is a user or API edit: imports leave them alone unless told to overwrite.
  std::set<std::string> protected_fields(std::int64_t track_id);
  void set_date_added(std::int64_t track_id, std::int64_t unix_seconds);  // not journalled
  // Restores the old value only if the field still holds the edit's new value; otherwise reports a conflict.
  UndoResult undo_edit(std::int64_t edit_id);
  UndoResult undo_batch(std::int64_t batch_id);  // all or nothing

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
  void journal_record(std::int64_t track_id, const std::string& field, const std::optional<std::string>& old_value,
                      const std::optional<std::string>& new_value);
  void tracked(std::int64_t track_id, const char* source, const std::function<void()>& change);
  UndoResult undo_records(const std::vector<TrackEditRecord>& newest_first);
  int undo_one(const TrackEditRecord& e);  // 0 restored, 1 conflict, 2 track gone
  int undo_whole(const TrackEditRecord& e);  // the "grid" and "cues" rows
  void apply_cue_moves(std::int64_t track_id, const std::vector<Cue>& before, const CueFollowProposal& p);
  std::vector<TrackEditRecord> read_edits(const char* where, std::int64_t arg, int limit, bool include_analysis = true);
  bool use_fts(const std::string& search) const;
  std::string search_pattern(const std::string& search) const;
  const std::set<std::int64_t>& missing_ids();  // tracks whose file is gone (rescanned at most every 5 s)
  std::set<std::int64_t> missing_ids_;
  std::int64_t missing_at_ = 0;
  bool fts_ = false;  // tracks_fts (FTS5 trigram) exists: substring search of 3+ characters uses it
  Db db_;
};

}  // namespace deckhand
