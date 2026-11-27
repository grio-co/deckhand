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
#include "deckhand/library/suggest.hpp"

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
  std::optional<double> last_sound_ms;  // analysis: end of the last audible sound (schema 7); automix uses it for the default mix-out
};

// ---- automix (schema 7) --------------------------------------------------------------------------------------------
// Phrase analysis as stored per track (schema 8). Plain data so the library needs no analysis module.
constexpr int kPhraseAnalysisVersion = 1;  // bump when the phrase algorithm changes: older rows are re-analysed lazily
struct StoredPhraseSection {
  double start_ms = 0, end_ms = 0;
  int start_bar = -1, end_bar = -1, bars = 0, energy = 5;
  std::string label, kind;
  double label_confidence = 0, boundary_confidence = 0, kick = 0;
  bool phrase_aligned = false;
};
struct StoredPhrases {
  int version = kPhraseAnalysisVersion;
  bool gridded = false, time_based = false;
  double origin_ms = 0, pickup_ms = 0, bar_ms = 0;
  int total_bars = 0;
  std::vector<double> bar_starts_ms;
  std::vector<float> bar_energy;
  std::vector<StoredPhraseSection> sections;
};

// Per-track mixing points and the same fields as per-queue-entry overrides. Every field is optional: unset = automatic
// (mix-in: main cue, else load cue; mix-out: N bars before the last audible sound; length and type: the global setting).
// Mean energy of the whole track and of its first and last 16 bars (0..1) from stored phrase data: the per-bar energy
// when there is some, else the section energies (1..10). Empty when the analysis found nothing to measure.
std::optional<EnergySummary> energy_of_phrases(const StoredPhrases& p);

struct AutomixPoints {
  std::optional<double> mix_in_ms;           // where the track starts playing when it is mixed in
  std::optional<double> mix_out_ms;          // where the track has been mixed out completely (the transition ends here)
  std::optional<double> transition_beats;    // preferred transition length
  std::optional<std::string> transition_type;  // "crossfade" | "cut" | "fade" | "echo_out"
  std::optional<double> max_play_ms;         // longest time the track plays, counted from its mix-in
  bool empty() const { return !mix_in_ms && !mix_out_ms && !transition_beats && !transition_type && !max_play_ms; }
};

// ---- linked mix points (schema 10) ----------------------------------------------------------------------------------
// "When track A reaches from_ms, track B starts from to_ms." The positions follow grid edits of their own track like cues do.
struct MixLink {
  std::int64_t id = 0;                         // 0 when adding
  std::int64_t from_track = 0, to_track = 0;
  double from_ms = 0, to_ms = 0;
  std::optional<double> transition_beats;      // unset = the automix default
  std::optional<std::string> transition_type;  // unset or "crossfade" | "cut" | "fade" | "echo_out"
  std::string label;
  std::int64_t created_at = 0;                 // unix seconds
};

// ---- vocal regions (schema 10) ----------------------------------------------------------------------------------------
constexpr int kVocalAnalysisVersion = 1;  // bump when the detector changes: older rows are re-analysed lazily
struct StoredVocalRegion {
  double start_ms = 0, end_ms = 0;
};
struct StoredVocals {
  int version = kVocalAnalysisVersion;
  std::vector<StoredVocalRegion> regions;  // sorted, non-overlapping; may be empty (analysed, no vocals found)
  std::string source;                      // free text: what it was computed from (e.g. the stem cache key)
};

// One row of the persisted play queue. `pending` is the entry whose transition has been scheduled but has not started
// (it is put back at the front when the application restarts).
struct QueueRow {
  std::int64_t entry_id = 0, track_id = 0;
  bool play_next = false;
  bool pending = false;
  AutomixPoints overrides;
};
struct QueueHistoryRow {
  std::int64_t id = 0, entry_id = 0, track_id = 0, played_at_ms = 0;
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

// A playlist, smart playlist or folder (with everything in it) removed from the collection, kept for
// restore_playlist() until purge_deleted_playlists() drops it.
struct DeletedPlaylist {
  std::int64_t id = 0;         // trash entry id (not the playlist's old id)
  std::string kind;            // "playlist" | "folder" | "smart"
  std::string name;
  std::int64_t old_id = 0;     // the playlist's id when it was deleted
  std::int64_t parent_id = 0;  // the folder it was in (0 = top level)
  std::int64_t deleted_at = 0; // unix seconds
  int playlists = 0;           // playlists in the deleted subtree (a smart playlist counts 1)
  int folders = 0;             // folders in the subtree, the deleted folder included
  int tracks = 0;              // playlist entries kept (a track in two playlists counts twice)
  bool shared = false;         // a shared-playlist link is kept and comes back with it
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
  int mix_links_moved = 0;    // linked mix point ends of this track that follow the grid (made when applied, else only offered)
};
enum class FollowStatus { Ok, NoGridEdit, AlreadyApplied, Conflict };
struct FollowResult {
  FollowStatus status = FollowStatus::NoGridEdit;
  CueFollowProposal proposal;
  std::int64_t batch_id = 0;
  int mix_links_moved = 0;
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
  int mix_links_moved = 0;     // linked mix points of the removed tracks that now belong to the kept track
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

// How often (and in which order) the user has played two tracks back to back; fed from the play history.
struct TransitionRow {
  std::int64_t from_id = 0, to_id = 0;
  int count = 0;
  std::int64_t last_ms = 0;  // 0 = never played that way (only saved)
  std::string note;          // set when the pair was saved from the preparation panel
  bool saved = false;
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

// ---- relocating moved files (relocate.inc) ----
struct RelocateOptions {
  std::vector<std::string> folders;  // where to look (searched recursively)
  double duration_tolerance_ms = 1500;
  // Length of an audio file in ms (0 / nullopt = unknown). The library cannot decode; callers that can pass this so a
  // match by name can be confirmed by duration.
  std::function<std::int64_t(const std::string&)> probe_duration;
  bool allow_name_only = false;  // accept a lone name match nothing else confirms (reported as weak)
  std::function<bool()> cancelled;
};
struct RelocateMatch {
  std::int64_t track_id = 0;
  std::string old_path, new_path;
  std::string how;   // "hash" | "name+size" | "name+duration" | "name+size+duration" | "name" | "sibling" | "manual"
  bool weak = false;  // only the file name agrees
};
struct RelocateAmbiguous {
  std::int64_t track_id = 0;
  std::string old_path;
  std::vector<std::string> candidates;
};
struct RelocateReport {
  int missing = 0;  // tracks whose file is gone
  int scanned = 0;  // files looked at in the search folders
  std::vector<RelocateMatch> found;
  std::vector<RelocateAmbiguous> ambiguous;  // several candidates tie: never guessed
  std::vector<std::int64_t> not_found;
};
struct RelocateResult {
  int applied = 0, skipped = 0;  // skipped: the file vanished, the track is no longer missing, or the new path is taken
  std::int64_t batch_id = 0;      // one journal batch: undo_batch(batch_id) puts the old paths back
};

// ---- browsing by directory (relocate.inc) ----
struct DiskFolder {
  std::string path;  // directory, forward slashes, no trailing slash
  std::string name;  // last component
  int tracks = 0;    // tracks directly in it
  int total = 0;     // tracks in it and below
  bool has_children = false;
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
  void set_last_sound_ms(std::int64_t track_id, std::optional<double> ms);  // not journalled (derived data)
  // Phrase sections and compact per-bar data (derived data, not journalled). nullopt when never analysed.
  std::optional<StoredPhrases> phrases(std::int64_t track_id);
  void set_phrases(std::int64_t track_id, const StoredPhrases& p);
  void clear_phrases(std::int64_t track_id);
  std::vector<std::int64_t> tracks_with_phrases();  // ids, for the cue-habit summary
  // Automix points of a track (empty when none are set); set with an empty value removes them. Not journalled.
  AutomixPoints automix_points(std::int64_t track_id);
  void set_automix_points(std::int64_t track_id, const AutomixPoints& points);
  // The play queue: the rows in play order (the pending one first), replaced as a whole. Rows of tracks that no longer
  // exist are kept (the player skips them).
  std::vector<QueueRow> play_queue();
  void save_play_queue(const std::vector<QueueRow>& rows);
  // What was played, oldest first in storage; queue_history returns the newest `limit` entries newest first.
  std::int64_t add_queue_history(std::int64_t entry_id, std::int64_t track_id, std::int64_t played_at_ms);
  std::vector<QueueHistoryRow> queue_history(int limit = 100);
  std::optional<QueueHistoryRow> pop_queue_history();  // removes and returns the newest entry
  void clear_queue_history();
  std::int64_t max_queue_entry_id();  // highest entry id in the queue and its history (0 when empty)
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
  // Deleting a playlist or folder (with everything inside) moves it to the playlist trash; the return value is the trash
  // entry id (0 when there is no such playlist). Journalled (field "playlist_trash", track id 0) so undo_batch /
  // undo_edit bring it back. The entry is kept like trashed tracks (purge_deleted_playlists).
  std::int64_t delete_playlist(std::int64_t id);
  std::vector<DeletedPlaylist> deleted_playlists();  // newest first
  // Puts a trashed playlist / folder subtree / smart playlist back: contents, order, place in the tree, rules and
  // shared-playlist link. The old ids are reused when free. When the old folder is gone it goes to the top level; a name
  // already used next to it becomes "Name (restored)". False when there is no such entry. `restored` gets the id of the
  // restored playlist (or smart playlist when kind is "smart").
  bool restore_playlist(std::int64_t trash_id, std::int64_t* restored = nullptr);
  // Drops entries deleted more than the given number of days ago (also runs from purge_deleted()).
  int purge_deleted_playlists(int older_than_days = 30);
  bool purge_deleted_playlist(std::int64_t trash_id);  // one entry now
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
  // To the playlist trash like delete_playlist (kind "smart"); returns the trash entry id, 0 when there is none.
  std::int64_t delete_smart_playlist(std::int64_t id);
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
  // ---- linked mix points (mix_links.inc) ----
  // Creates a link; throws std::invalid_argument for a missing track, the same track on both ends, a negative position or
  // an unknown transition type. A link between two positions that already exist (within 1 ms) updates that link instead.
  // Journalled (field "mix_links" of the from-track: the whole set of its links; undo restores it) and undoable.
  std::int64_t add_mix_link(const MixLink& link);
  std::optional<MixLink> mix_link(std::int64_t id);
  // Replaces positions, length, type and label of link `link.id` (the two tracks stay). False when it does not exist.
  bool update_mix_link(const MixLink& link);
  bool remove_mix_link(std::int64_t id);
  int remove_mix_links(std::int64_t track_id);  // every link out of or into the track; returns how many
  // Links out of / into / between tracks (tracks in the trash are left out), ordered by position in the from-track.
  std::vector<MixLink> mix_links_from(std::int64_t track_id);
  std::vector<MixLink> mix_links_to(std::int64_t track_id);
  std::vector<MixLink> mix_links_between(std::int64_t from_track, std::int64_t to_track);
  // The link automix follows when `from_track` plays at `position_ms` and `to_track` is next: the first link whose from
  // position is still ahead (within `slack_ms` behind), else the last one. nullopt when none.
  std::optional<MixLink> mix_link_for_transition(std::int64_t from_track, std::int64_t to_track, double position_ms = 0, double slack_ms = 1000);
  // Track -> number of links from `from_track` (used by the suggestions).
  std::map<std::int64_t, int> mix_link_targets(std::int64_t from_track);

  // ---- vocal regions (mix_links.inc) ----
  // Derived data, not journalled. nullopt when never analysed.
  std::optional<StoredVocals> vocals(std::int64_t track_id);
  void set_vocals(std::int64_t track_id, const StoredVocals& v);
  void clear_vocals(std::int64_t track_id);
  std::vector<std::int64_t> tracks_with_vocals();

  // ---- transition history and suggestions (suggest.cpp) ----
  // Called by add_history(): counts previous-track -> this-track of the same session. 0 when nothing was counted.
  int record_transition(std::int64_t session_id, std::int64_t track_id, std::int64_t at_ms);
  // Recounts every pair from the play history (the one-time backfill of schema 9; also a repair).
  void rebuild_transitions();
  void add_transition(std::int64_t from_id, std::int64_t to_id, int count = 1, std::int64_t at_ms = 0);
  std::optional<TransitionRow> transition(std::int64_t from_id, std::int64_t to_id);
  std::vector<TransitionRow> transitions_from(std::int64_t from_id, std::size_t limit = 50);
  // Saves a pair from the preparation panel (kept even when never played that way); the note replaces an older one.
  void save_transition(std::int64_t from_id, std::int64_t to_id, const std::string& note);
  // Energy of the track (0..1) from its stored phrase analysis; computed and stored when the row has none yet.
  std::optional<EnergySummary> energy_summary(std::int64_t track_id);
  // Ranked suggestions for what to play after `seed_id`. Deterministic: ties go to the lower track id.
  std::vector<NextTrack> suggest(std::int64_t seed_id, const NextTrackOptions& options = {});
  NextTrackWeights suggest_weights();  // the defaults with the settings "suggest.w.*" applied

  // ---- relocating moved files ----
  // Dry run: looks for the files of tracks that are missing. Changes nothing.
  RelocateReport relocate_scan(const RelocateOptions& options);
  // Applies the given matches (typically report.found) in one journal batch, keeping size and content hash.
  RelocateResult relocate_apply(const std::vector<RelocateMatch>& matches);
  // Manual relink: the track's file is now at new_path. Journalled ("path"); undoable.
  RelocateResult relink_track(std::int64_t track_id, const std::string& new_path);
  // Before relinking: other missing tracks that sat in the same old folder (or below it) whose file exists at the same
  // place relative to the new folder. Feed report.found to relocate_apply().
  RelocateReport relink_siblings(std::int64_t track_id, const std::string& new_path);

  // ---- folder views ----
  // A playlist folder shows the tracks of every playlist beneath it, each once. Default order: playlist by playlist (tree
  // order), then position; other sorts order the whole set. `sources` (optional) gets track -> name of the first playlist.
  std::vector<std::int64_t> folder_track_ids(std::int64_t folder_id, const std::string& search, SortKey sort = SortKey::Default,
                                             bool descending = false, std::map<std::int64_t, std::string>* sources = nullptr);
  // The library by the directories of its files. Children of `dir` ("" = top: the roots of the paths).
  std::vector<DiskFolder> disk_folders(const std::string& dir = {});
  std::vector<std::int64_t> disk_folder_track_ids(const std::string& dir, bool recursive, const std::string& search,
                                                  SortKey sort = SortKey::Default, bool descending = false);

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
  int undo_mix_links(const TrackEditRecord& e);
  int undo_playlist_trash(const TrackEditRecord& e);
  std::int64_t trash_playlist_node(std::int64_t id, bool smart, std::string* kind);  // 0 when absent; not journalled
  std::int64_t restore_playlist_entry(std::int64_t entry, std::string* kind);        // 0 when absent; not journalled
  int undo_whole(const TrackEditRecord& e);  // the "grid" and "cues" rows
  int follow_mix_links(std::int64_t track_id, const std::vector<BeatSection>& old_grid, const std::vector<BeatSection>& new_grid,
                       double tolerance_ms, bool apply);  // moved (apply) or movable (!apply) link ends
  void write_link_set(std::int64_t from_track, const std::vector<MixLink>& links);  // replaces the set, ids kept
  std::vector<MixLink> read_links(const char* where, std::int64_t a, std::int64_t b);
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
