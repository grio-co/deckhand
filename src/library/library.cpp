#include "deckhand/library/library.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include <sqlite3.h>

#include "deckhand/library/backup.hpp"
#include "deckhand/library/keys.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand {
namespace {

const char* const kSchema =
#include "deckhand_schema.inc"
    ;

// Portable layout: every track lives under a root; root 1 is the filesystem root.
constexpr std::int64_t kSystemRoot = 1;

// A Windows path ("C:\x", "C:/x", "\\server\share") stored under the system root '/' stands for itself.
bool is_windows_absolute(const std::string& p) {
  return (p.size() >= 3 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) ||
         (p.size() >= 3 && p[0] == '\\' && p[1] == '\\');
}

std::string join_path(const std::string& root, const std::string& rel) {
  if (root == "/" && is_windows_absolute(rel)) return rel;
  if (root.empty() || root.back() == '/') return root + rel;
  return root + "/" + rel;
}

std::string rel_of(const std::string& abs) { return abs.size() > 1 && abs[0] == '/' ? abs.substr(1) : abs; }

std::string extension_of(const std::string& path) {
  const auto dot = path.find_last_of('.');
  const auto slash = path.find_last_of("/\\");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  std::string ext = path.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext;
}

const char* const kTrackColumns =
    "t.id, r.last_seen_path, t.rel_path, t.title, t.artist, t.album, t.genre, t.key, t.format, t.bpm,"
    " t.duration_ms, t.rating, t.comment, t.color, t.date_added, t.bitrate, t.grid_locked,"
    " (SELECT count(*) FROM history h WHERE h.track_id = t.id),"
    " (SELECT coalesce(max(h.started_at), 0) FROM history h WHERE h.track_id = t.id)";

TrackRow read_track(const Stmt& s) {
  TrackRow t;
  t.id = s.i64(0);
  t.path = join_path(s.text(1), s.text(2));
  t.title = s.text(3);
  t.artist = s.text(4);
  t.album = s.text(5);
  t.genre = s.text(6);
  t.key = s.text(7);
  t.format = s.text(8);
  t.bpm = s.f64(9);
  t.duration_ms = s.i64(10);
  t.rating = static_cast<int>(s.i64(11));
  t.comment = s.text(12);
  t.color = s.is_null(13) ? 0 : static_cast<int>(s.i64(13));
  t.date_added = s.i64(14);
  t.bitrate = static_cast<int>(s.i64(15));
  t.grid_locked = s.i64(16) != 0;
  t.play_count = static_cast<int>(s.i64(17));
  t.last_played_ms = s.i64(18);
  return t;
}

// Whether migrate() would change this library's schema. (The optional search index is derived data: it is built or
// rebuilt without a copy.)
bool upgrade_pending(Db& db) {
  std::int64_t version = 1;
  {
    Stmt s(db, "SELECT max(version) FROM schema_version");
    if (s.step()) version = s.i64(0);
  }
  return version < kLibrarySchemaVersion;
}

bool has_column(Db& db, const char* table, const char* column) {
  Stmt s(db, (std::string("SELECT 1 FROM pragma_table_info('") + table + "') WHERE name = '" + column + "'").c_str());
  return s.step();
}

// A transaction that joins one already open (callers such as the sync engine batch many edits in their own).
struct OptionalTx {
  explicit OptionalTx(Db& d) : db(d), own(sqlite3_get_autocommit(d.handle()) != 0) {
    if (own) db.exec("BEGIN IMMEDIATE");
  }
  void commit() {
    if (!own) return;
    db.exec("COMMIT");
    own = false;
  }
  ~OptionalTx() {
    if (!own) return;
    try {
      db.exec("ROLLBACK");
    } catch (...) {
    }
  }
  Db& db;
  bool own;
};

}  // namespace

Library::Library(const std::string& db_path) {
  db_.open(db_path);
  bool has_schema = false;
  {
    Stmt s(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='schema_version'");
    has_schema = s.step();
  }
  // Copy an existing library before anything changes it. No copy, no upgrade.
  if (has_schema && upgrade_pending(db_)) {
    try {
      const std::string copy = backup_library(db_path, "upgrade");
      DH_LOG(Info, "library saved to %s before upgrading", copy.c_str());
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string("not upgrading the library, it could not be backed up first: ") + e.what());
    }
  }
  db_.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
  if (!has_schema) {
    Transaction tx(db_);
    db_.exec(kSchema);
    tx.commit();
    DH_LOG(Info, "created library at %s", db_path.c_str());
  }
  db_.exec("INSERT OR IGNORE INTO library_roots(id, label, last_seen_path) VALUES (1, 'System', '/')");
  migrate();
}

// schema.sql is version 1; later versions are applied here, one step at a time.
void Library::migrate() {
  std::int64_t version = 1;
  {
    Stmt s(db_, "SELECT max(version) FROM schema_version");
    if (s.step()) version = s.i64(0);
  }
  if (version < 2) {
    Transaction tx(db_);
    db_.exec("ALTER TABLE tracks ADD COLUMN gain_db REAL;"
             "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
             "CREATE TABLE history_sessions (id INTEGER PRIMARY KEY, started_at INTEGER NOT NULL);"
             "UPDATE schema_version SET version = 2;");
    tx.commit();
    DH_LOG(Info, "library migrated to schema %d", 2);
  }
  if (version < 4) {
    // Track preparation: grid lock, smart playlists, a history index for play counts.
    Transaction tx(db_);
    if (!has_column(db_, "tracks", "grid_locked")) db_.exec("ALTER TABLE tracks ADD COLUMN grid_locked INTEGER NOT NULL DEFAULT 0;");
    db_.exec(
        "CREATE TABLE IF NOT EXISTS smart_playlists ("
        " id INTEGER PRIMARY KEY, name TEXT NOT NULL, match_all INTEGER NOT NULL DEFAULT 1,"
        " rules TEXT NOT NULL DEFAULT '[]', position INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_history_track ON history(track_id);"
        "UPDATE schema_version SET version = 4;");
    tx.commit();
    DH_LOG(Info, "library migrated to schema %d (grid lock, smart playlists)", 4);
  }
  {
    // Substring search index (FTS5 trigram, case-insensitive) kept in step with `tracks` by triggers. Optional: an
    // SQLite build without FTS5 keeps working with the LIKE scan. Tried on every start until it exists.
    Stmt probe(db_, "SELECT 1 FROM sqlite_master WHERE name='tracks_fts'");
    if (!probe.step()) {
      try {
        Transaction tx(db_);
        db_.exec(
            "CREATE VIRTUAL TABLE tracks_fts USING fts5(title, artist, album, rel_path, content='tracks', content_rowid='id',"
            " tokenize='trigram');"
            "CREATE TRIGGER tracks_fts_ai AFTER INSERT ON tracks BEGIN"
            " INSERT INTO tracks_fts(rowid, title, artist, album, rel_path) VALUES (new.id, new.title, new.artist, new.album, new.rel_path);"
            " END;"
            "CREATE TRIGGER tracks_fts_ad AFTER DELETE ON tracks BEGIN"
            " INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, rel_path) VALUES ('delete', old.id, old.title, old.artist, old.album, old.rel_path);"
            " END;"
            "CREATE TRIGGER tracks_fts_au AFTER UPDATE OF title, artist, album, rel_path ON tracks BEGIN"
            " INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, rel_path) VALUES ('delete', old.id, old.title, old.artist, old.album, old.rel_path);"
            " INSERT INTO tracks_fts(rowid, title, artist, album, rel_path) VALUES (new.id, new.title, new.artist, new.album, new.rel_path);"
            " END;"
            "INSERT INTO tracks_fts(tracks_fts) VALUES ('rebuild');");
        tx.commit();
        DH_LOG(Info, "library search index built");
      } catch (const std::exception& e) {
        DH_LOG(Warn, "search index unavailable (%s); searching without it", e.what());
      }
    }
  }
  {
    Stmt s(db_, "SELECT 1 FROM sqlite_master WHERE name='tracks_fts'");
    fts_ = s.step();
  }
}

std::int64_t Library::track_count() {
  Stmt s(db_, "SELECT count(*) FROM tracks");
  return s.step() ? s.i64(0) : 0;
}

std::vector<PlaylistNode> Library::playlists() {
  std::vector<PlaylistNode> out;
  Stmt s(db_,
         "SELECT p.id, coalesce(p.parent_id, 0), p.name, p.is_folder,"
         " (SELECT count(*) FROM playlist_tracks pt WHERE pt.playlist_id = p.id)"
         " FROM playlists p ORDER BY coalesce(p.parent_id, 0), p.position, p.name");
  while (s.step()) {
    PlaylistNode n;
    n.id = s.i64(0);
    n.parent = s.i64(1);
    n.name = s.text(2);
    n.is_folder = s.i64(3) != 0;
    n.track_count = static_cast<int>(s.i64(4));
    out.push_back(std::move(n));
  }
  return out;
}

namespace {
// SELECT body shared by tracks() and track_ids(); `columns` is what to read. Roots are joined only when rows are read.
std::string track_query(const char* columns, bool join_roots, bool fts, std::int64_t playlist_id, SortKey sort, bool descending) {
  std::string sql = std::string("SELECT ") + columns + " FROM tracks t";
  if (join_roots) sql += " JOIN library_roots r ON r.id = t.root_id";
  if (playlist_id) sql += " JOIN playlist_tracks p ON p.track_id = t.id AND p.playlist_id = ?1";
  if (fts) sql += " WHERE (?2 = '' OR t.id IN (SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH ?3))";
  else sql += " WHERE (?2 = '' OR t.title LIKE ?3 OR t.artist LIKE ?3 OR t.album LIKE ?3 OR t.rel_path LIKE ?3)";
  const char* column = nullptr;
  switch (sort) {
    case SortKey::Default: break;
    case SortKey::Title: column = "t.title COLLATE NOCASE"; break;
    case SortKey::Artist: column = "t.artist COLLATE NOCASE"; break;
    case SortKey::Album: column = "t.album COLLATE NOCASE"; break;
    case SortKey::Genre: column = "t.genre COLLATE NOCASE"; break;
    case SortKey::Bpm: column = "t.bpm"; break;
    case SortKey::Key: column = "t.key COLLATE NOCASE"; break;
    case SortKey::Duration: column = "t.duration_ms"; break;
    case SortKey::Rating: column = "t.rating"; break;
    case SortKey::DateAdded: column = "t.date_added"; break;
    case SortKey::Comment: column = "t.comment COLLATE NOCASE"; break;
    case SortKey::Color: column = "coalesce(t.color, 0)"; break;
    case SortKey::Format: column = "t.format COLLATE NOCASE"; break;
    case SortKey::Bitrate: column = "t.bitrate"; break;
    case SortKey::PlayCount: column = "(SELECT count(*) FROM history h WHERE h.track_id = t.id)"; break;
    case SortKey::LastPlayed: column = "(SELECT coalesce(max(h.started_at), 0) FROM history h WHERE h.track_id = t.id)"; break;
  }
  if (column) {
    sql += std::string(" ORDER BY ") + column + (descending ? " DESC" : " ASC") + ", t.title COLLATE NOCASE, t.id";
  } else {
    sql += playlist_id ? " ORDER BY p.position" : " ORDER BY t.artist COLLATE NOCASE, t.title COLLATE NOCASE";
  }
  return sql;
}
}  // namespace

bool Library::use_fts(const std::string& search) const {
  if (!fts_) return false;
  std::size_t chars = 0;
  for (unsigned char c : search) chars += (c & 0xC0) != 0x80;  // code points, not bytes
  return chars >= 3;  // the trigram index cannot answer shorter terms
}

std::string Library::search_pattern(const std::string& search) const {
  if (!use_fts(search)) return "%" + search + "%";
  std::string q = "\"";  // one quoted phrase: every character literal, matched as a substring
  for (char c : search) q += c == '"' ? std::string("\"\"") : std::string(1, c);
  return q + "\"";
}

std::vector<TrackRow> Library::tracks(std::int64_t playlist_id, const std::string& search, SortKey sort,
                                      bool descending) {
  const std::string sql = track_query(kTrackColumns, true, use_fts(search), playlist_id, sort, descending);
  Stmt s(db_, sql.c_str());
  if (playlist_id) s.bind(1, playlist_id);
  s.bind(2, search);
  s.bind(3, search_pattern(search));
  std::vector<TrackRow> out;
  while (s.step()) out.push_back(read_track(s));
  return out;
}

std::vector<std::int64_t> Library::track_ids(std::int64_t playlist_id, const std::string& search, SortKey sort,
                                             bool descending) {
  const std::string sql = track_query("t.id", false, use_fts(search), playlist_id, sort, descending);
  Stmt s(db_, sql.c_str());
  if (playlist_id) s.bind(1, playlist_id);
  s.bind(2, search);
  s.bind(3, search_pattern(search));
  std::vector<std::int64_t> out;
  if (playlist_id == 0 && search.empty()) out.reserve(static_cast<std::size_t>(track_count()));
  while (s.step()) out.push_back(s.i64(0));
  return out;
}

std::optional<TrackRow> Library::track(std::int64_t id) {
  const std::string sql = std::string("SELECT ") + kTrackColumns +
                          " FROM tracks t JOIN library_roots r ON r.id = t.root_id WHERE t.id = ?1";
  Stmt s(db_, sql.c_str());
  s.bind(1, id);
  if (!s.step()) return std::nullopt;
  return read_track(s);
}

std::vector<Cue> Library::cues(std::int64_t track_id) {
  std::vector<Cue> out;
  Stmt s(db_, "SELECT kind, slot, start_ms, end_ms, label, color FROM cues WHERE track_id = ?1 ORDER BY start_ms, id");
  s.bind(1, track_id);
  while (s.step()) {
    Cue c;
    const std::string kind = s.text(0);
    c.kind = kind == "hot" ? CueKind::Hot : kind == "loop" ? CueKind::Loop : CueKind::Memory;
    c.slot = s.is_null(1) ? -1 : static_cast<int>(s.i64(1));
    c.start_ms = s.f64(2);
    c.end_ms = s.is_null(3) ? -1 : s.f64(3);
    c.label = s.text(4);
    c.color = s.is_null(5) ? -1 : static_cast<int>(s.i64(5));
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<BeatSection> Library::beatgrid(std::int64_t track_id) {
  std::vector<BeatSection> out;
  Stmt s(db_, "SELECT first_beat_ms, bpm, bar_beat FROM beatgrids WHERE track_id = ?1 ORDER BY position");
  s.bind(1, track_id);
  while (s.step()) out.push_back({s.f64(0), s.f64(1), static_cast<int>(s.i64(2))});
  return out;
}

std::optional<std::int64_t> Library::track_id_for_path(const std::string& abs_path) {
  Stmt s(db_, "SELECT id FROM tracks WHERE root_id = ?1 AND rel_path = ?2");
  s.bind(1, kSystemRoot).bind(2, rel_of(abs_path));
  if (!s.step()) return std::nullopt;
  return s.i64(0);
}

std::int64_t Library::upsert_track(const NewTrack& t) {
  const auto existing = track_id_for_path(t.path);
  if (existing) {
    Stmt u(db_,
           "UPDATE tracks SET title=?1, artist=?2, album=?3, genre=?4, key=?5, comment=?6, bpm=?7,"
           " duration_ms=?8, rating=?9, year=?10, bitrate=?11, sample_rate=?12 WHERE id=?13");
    u.bind(1, t.title).bind(2, t.artist).bind(3, t.album).bind(4, t.genre).bind(5, t.key).bind(6, t.comment);
    u.bind(7, t.bpm).bind(8, t.duration_ms).bind(9, t.rating).bind(10, t.year).bind(11, t.bitrate);
    u.bind(12, t.sample_rate).bind(13, *existing);
    u.run();
    return *existing;
  }
  Stmt i(db_,
         "INSERT INTO tracks(root_id, rel_path, title, artist, album, genre, key, comment, bpm, duration_ms,"
         " rating, year, bitrate, sample_rate, format, date_added)"
         " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)");
  i.bind(1, kSystemRoot).bind(2, rel_of(t.path)).bind(3, t.title).bind(4, t.artist).bind(5, t.album);
  i.bind(6, t.genre).bind(7, t.key).bind(8, t.comment).bind(9, t.bpm).bind(10, t.duration_ms);
  i.bind(11, t.rating).bind(12, t.year).bind(13, t.bitrate).bind(14, t.sample_rate);
  i.bind(15, extension_of(t.path)).bind(16, static_cast<std::int64_t>(std::time(nullptr)));
  i.run();
  return db_.last_insert_id();
}

void Library::set_hot_cue(std::int64_t track_id, int slot, double start_ms, double end_ms) {
  OptionalTx tx(db_);
  std::string label;
  bool has_label = false, has_color = false;
  std::int64_t color = -1;
  {
    Stmt old(db_, "SELECT label, color FROM cues WHERE track_id = ?1 AND slot = ?2 AND kind IN ('hot', 'loop') LIMIT 1");
    old.bind(1, track_id).bind(2, slot);
    if (old.step()) {
      has_label = !old.is_null(0);
      label = old.text(0);
      has_color = !old.is_null(1);
      color = has_color ? old.i64(1) : -1;
    }
  }
  clear_hot_cue(track_id, slot);
  Stmt i(db_, "INSERT INTO cues(track_id, kind, slot, start_ms, end_ms, label, color) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
  i.bind(1, track_id).bind(2, end_ms >= 0 ? "loop" : "hot").bind(3, slot).bind(4, start_ms);
  if (end_ms >= 0) i.bind(5, end_ms);
  else i.bind_null(5);
  if (has_label) i.bind(6, label);
  else i.bind_null(6);
  if (has_color) i.bind(7, color);
  else i.bind_null(7);
  i.run();
  tx.commit();
}

void Library::set_hot_cue_style(std::int64_t track_id, int slot, const std::string& label, int color) {
  Stmt u(db_, "UPDATE cues SET label = ?1, color = ?2 WHERE track_id = ?3 AND slot = ?4 AND kind IN ('hot', 'loop')");
  if (label.empty()) u.bind_null(1);
  else u.bind(1, label);
  if (color < 0) u.bind_null(2);
  else u.bind(2, color);
  u.bind(3, track_id).bind(4, slot);
  u.run();
}

std::vector<Cue> Library::memory_cues(std::int64_t track_id) {
  std::vector<Cue> out;
  for (Cue& c : cues(track_id))
    if (c.slot < 0) out.push_back(std::move(c));
  return out;
}

void Library::replace_memory_cues(std::int64_t track_id, const std::vector<Cue>& list) {
  OptionalTx tx(db_);
  Stmt d(db_, "DELETE FROM cues WHERE track_id = ?1 AND slot IS NULL");
  d.bind(1, track_id);
  d.run();
  Stmt i(db_, "INSERT INTO cues(track_id, kind, slot, start_ms, end_ms, label, color) VALUES (?1, ?2, NULL, ?3, ?4, ?5, ?6)");
  for (const Cue& c : list) {
    const bool loop = c.kind == CueKind::Loop && c.end_ms > c.start_ms;
    i.bind(1, track_id).bind(2, loop ? "loop" : "memory").bind(3, c.start_ms);
    if (loop) i.bind(4, c.end_ms);
    else i.bind_null(4);
    if (c.label.empty()) i.bind_null(5);
    else i.bind(5, c.label);
    if (c.color < 0) i.bind_null(6);
    else i.bind(6, c.color);
    i.run();
  }
  tx.commit();
}

bool Library::grid_locked(std::int64_t track_id) {
  Stmt s(db_, "SELECT grid_locked FROM tracks WHERE id = ?1");
  s.bind(1, track_id);
  return s.step() && s.i64(0) != 0;
}

void Library::set_grid_locked(std::int64_t track_id, bool locked) {
  Stmt u(db_, "UPDATE tracks SET grid_locked = ?1 WHERE id = ?2");
  u.bind(1, locked ? 1 : 0).bind(2, track_id);
  u.run();
}

void Library::set_track_color(std::int64_t track_id, int color) {
  Stmt u(db_, "UPDATE tracks SET color = ?1 WHERE id = ?2");
  if (color <= 0) u.bind_null(1);
  else u.bind(1, color);
  u.bind(2, track_id);
  u.run();
}

void Library::clear_hot_cue(std::int64_t track_id, int slot) {
  Stmt d(db_, "DELETE FROM cues WHERE track_id = ?1 AND slot = ?2 AND kind IN ('hot', 'loop')");
  d.bind(1, track_id).bind(2, slot);
  d.run();
}

void Library::set_bpm(std::int64_t track_id, double bpm) {
  Stmt u(db_, "UPDATE tracks SET bpm = ?1 WHERE id = ?2");
  u.bind(1, bpm).bind(2, track_id);
  u.run();
}

void Library::replace_beatgrid(std::int64_t track_id, const std::vector<BeatSection>& grid) {
  Stmt d(db_, "DELETE FROM beatgrids WHERE track_id = ?1");
  d.bind(1, track_id);
  d.run();
  Stmt i(db_, "INSERT INTO beatgrids(track_id, position, first_beat_ms, bpm, bar_beat) VALUES (?1,?2,?3,?4,?5)");
  int pos = 0;
  for (const auto& g : grid) {
    i.bind(1, track_id).bind(2, pos++).bind(3, g.first_beat_ms).bind(4, g.bpm).bind(5, g.bar_beat);
    i.run();
  }
}

void Library::edit_track(std::int64_t track_id, const TrackEdit& e) {
  Transaction tx(db_);
  const auto text = [&](const char* column, const std::optional<std::string>& v) {
    if (!v) return;
    const std::string sql = std::string("UPDATE tracks SET ") + column + " = ?1 WHERE id = ?2";
    Stmt u(db_, sql.c_str());
    u.bind(1, *v).bind(2, track_id);
    u.run();
  };
  const auto number = [&](const char* column, const std::optional<int>& v) {
    if (!v) return;
    const std::string sql = std::string("UPDATE tracks SET ") + column + " = ?1 WHERE id = ?2";
    Stmt u(db_, sql.c_str());
    u.bind(1, *v).bind(2, track_id);
    u.run();
  };
  text("title", e.title);
  text("artist", e.artist);
  text("album", e.album);
  text("genre", e.genre);
  text("key", e.key);
  text("comment", e.comment);
  number("rating", e.rating);
  number("year", e.year);
  tx.commit();
}

void Library::set_key(std::int64_t track_id, const std::string& key) {
  Stmt u(db_, "UPDATE tracks SET key = ?1 WHERE id = ?2");
  u.bind(1, key).bind(2, track_id);
  u.run();
}

void Library::set_gain_db(std::int64_t track_id, double gain_db) {
  Stmt u(db_, "UPDATE tracks SET gain_db = ?1 WHERE id = ?2");
  u.bind(1, gain_db).bind(2, track_id);
  u.run();
}

std::optional<double> Library::gain_db(std::int64_t track_id) {
  Stmt s(db_, "SELECT gain_db FROM tracks WHERE id = ?1");
  s.bind(1, track_id);
  if (!s.step() || s.is_null(0)) return std::nullopt;
  return s.f64(0);
}

void Library::set_track_path(std::int64_t track_id, const std::string& abs_path) {
  Stmt u(db_, "UPDATE OR IGNORE tracks SET root_id = ?1, rel_path = ?2, format = ?3 WHERE id = ?4");
  u.bind(1, kSystemRoot).bind(2, rel_of(abs_path)).bind(3, extension_of(abs_path)).bind(4, track_id);
  u.run();
}

int Library::relink_prefix(const std::string& old_prefix, const std::string& new_prefix) {
  const std::string from = rel_of(old_prefix), to = rel_of(new_prefix);
  if (from.empty()) return 0;
  std::vector<std::pair<std::int64_t, std::string>> rows;
  {
    Stmt s(db_, "SELECT id, rel_path FROM tracks WHERE root_id = ?1 AND substr(rel_path, 1, ?2) = ?3");
    s.bind(1, kSystemRoot).bind(2, static_cast<std::int64_t>(from.size())).bind(3, from);
    while (s.step()) rows.emplace_back(s.i64(0), s.text(1));
  }
  Transaction tx(db_);
  int changed = 0;
  Stmt u(db_, "UPDATE OR IGNORE tracks SET rel_path = ?1 WHERE id = ?2");
  for (const auto& [id, rel] : rows) {
    u.bind(1, to + rel.substr(from.size())).bind(2, id);
    u.run();
    ++changed;
  }
  tx.commit();
  return changed;
}

void Library::remove_track(std::int64_t track_id) {
  Transaction tx(db_);
  std::vector<std::int64_t> lists;
  {
    Stmt s(db_, "SELECT DISTINCT playlist_id FROM playlist_tracks WHERE track_id = ?1");
    s.bind(1, track_id);
    while (s.step()) lists.push_back(s.i64(0));
  }
  for (const auto list : lists) {
    auto ids = playlist_track_ids(list);
    std::erase(ids, track_id);
    set_playlist_tracks(list, ids);
  }
  Stmt d(db_, "DELETE FROM tracks WHERE id = ?1");
  d.bind(1, track_id);
  d.run();
  Stmt m(db_, "DELETE FROM import_map WHERE entity = 'track' AND local_id = ?1");
  m.bind(1, track_id);
  m.run();
  tx.commit();
}

std::int64_t Library::create_playlist(std::int64_t parent, const std::string& name, bool is_folder) {
  Stmt i(db_,
         "INSERT INTO playlists(parent_id, name, is_folder, position) VALUES (?1, ?2, ?3,"
         " (SELECT coalesce(max(position), -1) + 1 FROM playlists WHERE coalesce(parent_id, 0) = ?4))");
  if (parent) {
    i.bind(1, parent);
  } else {
    i.bind_null(1);
  }
  i.bind(2, name).bind(3, is_folder ? 1 : 0).bind(4, parent);
  i.run();
  return db_.last_insert_id();
}

void Library::rename_playlist(std::int64_t id, const std::string& name) {
  Stmt u(db_, "UPDATE playlists SET name = ?1 WHERE id = ?2");
  u.bind(1, name).bind(2, id);
  u.run();
}

void Library::delete_playlist(std::int64_t id) {
  Transaction tx(db_);
  Stmt m(db_,
         "WITH RECURSIVE sub(id) AS (SELECT ?1 UNION ALL SELECT p.id FROM playlists p JOIN sub ON p.parent_id = sub.id)"
         " DELETE FROM import_map WHERE entity = 'playlist' AND local_id IN (SELECT id FROM sub)");
  m.bind(1, id);
  m.run();
  Stmt d(db_, "DELETE FROM playlists WHERE id = ?1");  // children and entries cascade
  d.bind(1, id);
  d.run();
  tx.commit();
}

bool Library::move_playlist(std::int64_t id, std::int64_t new_parent, int position) {
  for (std::int64_t p = new_parent; p != 0;) {  // refuse to move a folder into its own subtree
    if (p == id) return false;
    Stmt s(db_, "SELECT coalesce(parent_id, 0) FROM playlists WHERE id = ?1");
    s.bind(1, p);
    if (!s.step()) return false;
    p = s.i64(0);
  }
  Transaction tx(db_);
  std::vector<std::int64_t> siblings;
  {
    Stmt s(db_, "SELECT id FROM playlists WHERE coalesce(parent_id, 0) = ?1 AND id != ?2 ORDER BY position, name");
    s.bind(1, new_parent).bind(2, id);
    while (s.step()) siblings.push_back(s.i64(0));
  }
  const auto at = position < 0 || position > static_cast<int>(siblings.size()) ? siblings.size()
                                                                               : static_cast<std::size_t>(position);
  siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(at), id);
  {
    Stmt u(db_, "UPDATE playlists SET parent_id = ?1 WHERE id = ?2");
    if (new_parent) {
      u.bind(1, new_parent);
    } else {
      u.bind_null(1);
    }
    u.bind(2, id);
    u.run();
  }
  Stmt u(db_, "UPDATE playlists SET position = ?1 WHERE id = ?2");
  int pos = 0;
  for (const auto sibling : siblings) {
    u.bind(1, pos++).bind(2, sibling);
    u.run();
  }
  tx.commit();
  return true;
}

std::vector<std::int64_t> Library::playlist_track_ids(std::int64_t playlist_id) {
  std::vector<std::int64_t> out;
  Stmt s(db_, "SELECT track_id FROM playlist_tracks WHERE playlist_id = ?1 ORDER BY position");
  s.bind(1, playlist_id);
  while (s.step()) out.push_back(s.i64(0));
  return out;
}

void Library::set_playlist_tracks(std::int64_t playlist_id, const std::vector<std::int64_t>& track_ids) {
  Stmt d(db_, "DELETE FROM playlist_tracks WHERE playlist_id = ?1");
  d.bind(1, playlist_id);
  d.run();
  Stmt i(db_, "INSERT INTO playlist_tracks(playlist_id, position, track_id) VALUES (?1, ?2, ?3)");
  int pos = 0;
  for (const auto id : track_ids) {
    i.bind(1, playlist_id).bind(2, pos++).bind(3, id);
    i.run();
  }
}

void Library::add_to_playlist(std::int64_t playlist_id, std::int64_t track_id, int position) {
  Transaction tx(db_);
  auto ids = playlist_track_ids(playlist_id);
  const auto at = position < 0 || position > static_cast<int>(ids.size()) ? ids.size()
                                                                         : static_cast<std::size_t>(position);
  ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(at), track_id);
  set_playlist_tracks(playlist_id, ids);
  tx.commit();
}

void Library::remove_from_playlist(std::int64_t playlist_id, int position) {
  Transaction tx(db_);
  auto ids = playlist_track_ids(playlist_id);
  if (position < 0 || position >= static_cast<int>(ids.size())) return;
  ids.erase(ids.begin() + position);
  set_playlist_tracks(playlist_id, ids);
  tx.commit();
}

void Library::move_in_playlist(std::int64_t playlist_id, int from, int to) {
  Transaction tx(db_);
  auto ids = playlist_track_ids(playlist_id);
  const int n = static_cast<int>(ids.size());
  if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
  const auto id = ids[static_cast<std::size_t>(from)];
  ids.erase(ids.begin() + from);
  ids.insert(ids.begin() + to, id);
  set_playlist_tracks(playlist_id, ids);
  tx.commit();
}

namespace {
std::int64_t now_ms() { return static_cast<std::int64_t>(std::time(nullptr)) * 1000; }
}  // namespace

std::int64_t Library::begin_history_session() {
  Stmt i(db_, "INSERT INTO history_sessions(started_at) VALUES (?1)");
  i.bind(1, now_ms());
  i.run();
  return db_.last_insert_id();
}

std::int64_t Library::add_history(std::int64_t session_id, std::int64_t track_id, const std::string& deck,
                                  std::int64_t started_at_ms) {
  Stmt i(db_, "INSERT INTO history(session_id, track_id, deck, started_at, played_ms) VALUES (?1, ?2, ?3, ?4, 0)");
  i.bind(1, session_id).bind(2, track_id).bind(3, deck).bind(4, started_at_ms);
  i.run();
  return db_.last_insert_id();
}

void Library::set_history_played(std::int64_t history_id, std::int64_t played_ms) {
  Stmt u(db_, "UPDATE history SET played_ms = ?1 WHERE id = ?2");
  u.bind(1, played_ms).bind(2, history_id);
  u.run();
}

std::vector<HistorySession> Library::history_sessions() {
  std::vector<HistorySession> out;
  Stmt s(db_,
         "SELECT s.id, s.started_at, (SELECT count(*) FROM history h WHERE h.session_id = s.id) AS n"
         " FROM history_sessions s WHERE n > 0 ORDER BY s.started_at DESC, s.id DESC");
  while (s.step()) out.push_back({s.i64(0), s.i64(1), static_cast<int>(s.i64(2))});
  return out;
}

std::vector<HistoryEntry> Library::history(std::int64_t session_id) {
  std::vector<HistoryEntry> out;
  Stmt s(db_,
         "SELECT h.id, h.session_id, coalesce(h.track_id, 0), h.deck, coalesce(t.title, ''), coalesce(t.artist, ''),"
         " h.started_at, h.played_ms FROM history h LEFT JOIN tracks t ON t.id = h.track_id"
         " WHERE h.session_id = ?1 ORDER BY h.started_at, h.id");
  s.bind(1, session_id);
  while (s.step()) {
    HistoryEntry e;
    e.id = s.i64(0);
    e.session_id = s.i64(1);
    e.track_id = s.i64(2);
    e.deck = s.text(3);
    e.title = s.text(4);
    e.artist = s.text(5);
    e.started_at_ms = s.i64(6);
    e.played_ms = s.i64(7);
    out.push_back(std::move(e));
  }
  return out;
}


// ---- smart playlists ---------------------------------------------------------------------------------------------
namespace {

const char* const kFieldNames[] = {"text", "bpm", "key", "genre", "rating", "color", "added", "plays", "playlist", "missing", "bpmnear"};

std::string json_escape(const std::string& s) {
  std::string out;
  for (const unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      out += b;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::string num_text(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.10g", v);
  return b;
}

// Minimal reader for the one shape smart_rules_json writes: an array of flat objects of strings, numbers, booleans.
struct JsonIn {
  const std::string& s;
  std::size_t i = 0;
  void ws() {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  }
  bool eat(char c) {
    ws();
    if (i < s.size() && s[i] == c) {
      ++i;
      return true;
    }
    return false;
  }
  bool str(std::string& out) {
    out.clear();
    if (!eat('"')) return false;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\') {
        if (++i >= s.size()) return false;
        if (s[i] == 'u') {
          if (i + 4 >= s.size()) return false;
          const unsigned long cp = std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
          if (cp < 0x80) out += static_cast<char>(cp);
          else if (cp < 0x800) out += {static_cast<char>(0xC0 | (cp >> 6)), static_cast<char>(0x80 | (cp & 0x3F))};
          else out += {static_cast<char>(0xE0 | (cp >> 12)), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), static_cast<char>(0x80 | (cp & 0x3F))};
          i += 4;
        } else {
          out += s[i];
        }
      } else {
        out += s[i];
      }
      ++i;
    }
    return i < s.size() && s[i++] == '"';
  }
  bool num(double& v) {
    ws();
    char* end = nullptr;
    v = std::strtod(s.c_str() + i, &end);
    if (end == s.c_str() + i || !std::isfinite(v)) return false;
    i = static_cast<std::size_t>(end - s.c_str());
    return true;
  }
};

// Bound values of a generated query, numbered from ?4 (?1 playlist, ?2 / ?3 search).
struct Bound {
  std::string text;
  double num = 0;
  bool is_text = true;
};

std::string like_escape(const std::string& s) {
  std::string out = "%";
  for (const char c : s) {
    if (c == '%' || c == '_' || c == '\\') out += '\\';
    out += c;
  }
  return out + "%";
}

}  // namespace

std::string smart_rules_json(const std::vector<SmartRule>& rules) {
  std::string out = "[";
  for (std::size_t k = 0; k < rules.size(); ++k) {
    const SmartRule& r = rules[k];
    out += (k ? "," : "");
    out += std::string("{\"f\":\"") + kFieldNames[static_cast<int>(r.field)] + "\",\"n\":" + (r.negate ? "true" : "false") +
           ",\"t\":\"" + json_escape(r.text) + "\",\"lo\":" + num_text(r.lo) + ",\"hi\":" + num_text(r.hi) +
           ",\"id\":" + std::to_string(r.id) + "}";
  }
  return out + "]";
}

bool smart_rules_from_json(const std::string& json, std::vector<SmartRule>& rules) {
  rules.clear();
  JsonIn in{json};
  const auto fail = [&] {
    rules.clear();
    return false;
  };
  if (!in.eat('[')) return fail();
  if (in.eat(']')) return true;
  for (;;) {
    SmartRule r;
    if (!in.eat('{')) return fail();
    bool have_field = false;
    for (;;) {
      std::string key;
      if (!in.str(key) || !in.eat(':')) return fail();
      in.ws();
      if (key == "f" || key == "t") {
        std::string v;
        if (!in.str(v)) return fail();
        if (key == "t") {
          r.text = v;
        } else {
          for (int f = 0; f < 11; ++f)
            if (v == kFieldNames[f]) {
              r.field = static_cast<SmartRule::Field>(f);
              have_field = true;
            }
        }
      } else if (key == "n") {
        if (json.compare(in.i, 4, "true") == 0) {
          r.negate = true;
          in.i += 4;
        } else if (json.compare(in.i, 5, "false") == 0) {
          r.negate = false;
          in.i += 5;
        } else {
          return fail();
        }
      } else if (key == "lo" || key == "hi" || key == "id") {
        double v = 0;
        if (!in.num(v)) return fail();
        if (key == "lo") r.lo = v;
        else if (key == "hi") r.hi = v;
        else r.id = static_cast<std::int64_t>(v);
      } else {
        return fail();
      }
      if (in.eat(',')) continue;
      if (in.eat('}')) break;
      return fail();
    }
    if (!have_field) return fail();
    rules.push_back(std::move(r));
    if (in.eat(',')) continue;
    if (in.eat(']')) break;
    return fail();
  }
  in.ws();
  return in.i == json.size() ? true : fail();
}

const std::set<std::int64_t>& Library::missing_ids() {
  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
  if (missing_at_ == 0 || now - missing_at_ >= 5) {
    missing_ids_.clear();
    Stmt s(db_, "SELECT t.id, r.last_seen_path, t.rel_path FROM tracks t JOIN library_roots r ON r.id = t.root_id");
    while (s.step()) {
      std::error_code ec;
      if (!std::filesystem::exists(join_path(s.text(1), s.text(2)), ec)) missing_ids_.insert(s.i64(0));
    }
    missing_at_ = now;
  }
  return missing_ids_;
}

std::vector<std::int64_t> Library::query_ids(const std::vector<SmartRule>& rules, bool match_all, const std::string& search,
                                             SortKey sort, bool descending) {
  std::vector<Bound> binds;
  const auto param = [&](const std::string& t) {
    binds.push_back({t, 0, true});
    return "?" + std::to_string(binds.size() + 3);
  };
  const auto numparam = [&](double v) {
    binds.push_back({"", v, false});
    return "?" + std::to_string(binds.size() + 3);
  };
  std::vector<std::string> clauses;
  std::vector<std::int64_t> missing_rows;  // Missing rules are answered from the file-system scan
  std::string miss_sql;
  for (const SmartRule& r : rules) {
    std::string c;
    switch (r.field) {
      case SmartRule::Field::Text: {
        if (r.text.empty()) continue;
        const std::string p = param(like_escape(r.text));
        c = "(coalesce(t.title,'') LIKE " + p + " ESCAPE '\\' OR coalesce(t.artist,'') LIKE " + p + " ESCAPE '\\' OR coalesce(t.album,'') LIKE " + p +
            " ESCAPE '\\' OR coalesce(t.comment,'') LIKE " + p + " ESCAPE '\\' OR t.rel_path LIKE " + p + " ESCAPE '\\')";
        break;
      }
      case SmartRule::Field::Genre: {
        if (r.text.empty()) continue;
        c = "coalesce(t.genre,'') LIKE " + param(like_escape(r.text)) + " ESCAPE '\\'";
        break;
      }
      case SmartRule::Field::Bpm:
        c = "(coalesce(t.bpm,0) >= " + numparam(r.lo) + (r.hi > 0 ? " AND coalesce(t.bpm,0) <= " + numparam(r.hi) : std::string()) + ")";
        break;
      case SmartRule::Field::Key: {
        std::string list;
        for (std::size_t a = 0; a <= r.text.size();) {
          const std::size_t b = r.text.find(',', a);
          std::string k = r.text.substr(a, b == std::string::npos ? std::string::npos : b - a);
          while (!k.empty() && std::isspace(static_cast<unsigned char>(k.back()))) k.pop_back();
          while (!k.empty() && std::isspace(static_cast<unsigned char>(k.front()))) k.erase(k.begin());
          if (!k.empty()) {
            const KeyInfo info = parse_key(k);
            const std::string forms[] = {k, info ? key_wheel_text(info) : k, info ? key_musical_text(info) : k};
            for (std::size_t f = 0; f < 3; ++f)
              if (f == 0 || forms[f] != forms[0]) list += (list.empty() ? "" : ",") + param(forms[f]);
          }
          if (b == std::string::npos) break;
          a = b + 1;
        }
        if (list.empty()) continue;
        c = "coalesce(t.key,'') COLLATE NOCASE IN (" + list + ")";
        break;
      }
      case SmartRule::Field::Rating: c = "coalesce(t.rating,0) >= " + numparam(r.lo); break;
      case SmartRule::Field::Color:
        c = r.lo <= 0 ? std::string("coalesce(t.color,0) = 0") : "t.color = " + numparam(r.lo);
        break;
      case SmartRule::Field::AddedDays:
        c = "t.date_added >= " + numparam(static_cast<double>(std::time(nullptr)) - r.lo * 86400.0);
        break;
      case SmartRule::Field::Plays: {
        const std::string count = "(SELECT count(*) FROM history h WHERE h.track_id = t.id)";
        c = "(" + count + " >= " + numparam(r.lo) + (r.hi >= 0 ? " AND " + count + " <= " + numparam(r.hi) : std::string()) + ")";
        break;
      }
      case SmartRule::Field::InPlaylist:
        c = "t.id IN (SELECT track_id FROM playlist_tracks WHERE playlist_id = " + numparam(static_cast<double>(r.id)) + ")";
        break;
      case SmartRule::Field::BpmNear: {
        if (r.lo <= 0) {
          c = "0";
          break;
        }
        const std::string ref = numparam(r.lo), tol = numparam(r.hi > 0 ? r.hi : 0.06);
        c = "(coalesce(t.bpm,0) > 0 AND (abs(t.bpm / " + ref + " - 1) <= " + tol + " OR abs(t.bpm / (" + ref + " * 2) - 1) <= " + tol +
            " OR abs(t.bpm / (" + ref + " / 2) - 1) <= " + tol + "))";
        break;
      }
      case SmartRule::Field::Missing: {
        // negate = the file is present. The scan is cached; ids are integers, so they go into the SQL as a list.
        std::string ids;
        const auto& gone = missing_ids();
        if (r.negate) {
          c = "1";
          if (!gone.empty()) {
            for (const auto id : gone) ids += (ids.empty() ? "" : ",") + std::to_string(id);
            c = "t.id NOT IN (" + ids + ")";
          }
        } else {
          for (const auto id : gone) ids += (ids.empty() ? "" : ",") + std::to_string(id);
          c = ids.empty() ? std::string("0") : "t.id IN (" + ids + ")";
        }
        clauses.push_back("(" + c + ")");
        continue;
      }
    }
    if (r.negate) c = "NOT (" + c + ")";
    clauses.push_back("(" + c + ")");
  }
  std::string sql = "SELECT t.id FROM tracks t WHERE ";
  const bool fts = use_fts(search);
  if (fts) sql += "(?2 = '' OR t.id IN (SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH ?3))";
  else sql += "(?2 = '' OR t.title LIKE ?3 OR t.artist LIKE ?3 OR t.album LIKE ?3 OR t.rel_path LIKE ?3)";
  if (!clauses.empty()) {
    sql += " AND (";
    for (std::size_t k = 0; k < clauses.size(); ++k) sql += (k ? (match_all ? " AND " : " OR ") : "") + clauses[k];
    sql += ")";
  }
  const char* column = nullptr;
  switch (sort) {
    case SortKey::Default: break;
    case SortKey::Title: column = "t.title COLLATE NOCASE"; break;
    case SortKey::Artist: column = "t.artist COLLATE NOCASE"; break;
    case SortKey::Album: column = "t.album COLLATE NOCASE"; break;
    case SortKey::Genre: column = "t.genre COLLATE NOCASE"; break;
    case SortKey::Bpm: column = "t.bpm"; break;
    case SortKey::Key: column = "t.key COLLATE NOCASE"; break;
    case SortKey::Duration: column = "t.duration_ms"; break;
    case SortKey::Rating: column = "t.rating"; break;
    case SortKey::DateAdded: column = "t.date_added"; break;
    case SortKey::Comment: column = "t.comment COLLATE NOCASE"; break;
    case SortKey::Color: column = "coalesce(t.color, 0)"; break;
    case SortKey::Format: column = "t.format COLLATE NOCASE"; break;
    case SortKey::Bitrate: column = "t.bitrate"; break;
    case SortKey::PlayCount: column = "(SELECT count(*) FROM history h WHERE h.track_id = t.id)"; break;
    case SortKey::LastPlayed: column = "(SELECT coalesce(max(h.started_at), 0) FROM history h WHERE h.track_id = t.id)"; break;
  }
  if (column) sql += std::string(" ORDER BY ") + column + (descending ? " DESC" : " ASC") + ", t.title COLLATE NOCASE, t.id";
  else sql += " ORDER BY t.artist COLLATE NOCASE, t.title COLLATE NOCASE, t.id";
  Stmt st(db_, sql.c_str());
  st.bind(1, static_cast<std::int64_t>(0));
  st.bind(2, search);
  st.bind(3, search_pattern(search));
  for (std::size_t k = 0; k < binds.size(); ++k) {
    const int at = static_cast<int>(k) + 4;
    if (binds[k].is_text) st.bind(at, binds[k].text);
    else st.bind(at, binds[k].num);
  }
  std::vector<std::int64_t> out;
  while (st.step()) out.push_back(st.i64(0));
  return out;
}

std::vector<SmartPlaylist> Library::smart_playlists() {
  std::vector<SmartPlaylist> out;
  Stmt s(db_, "SELECT id, name, match_all, rules FROM smart_playlists ORDER BY position, name COLLATE NOCASE, id");
  while (s.step()) {
    SmartPlaylist p;
    p.id = s.i64(0);
    p.name = s.text(1);
    p.match_all = s.i64(2) != 0;
    smart_rules_from_json(s.text(3), p.rules);
    out.push_back(std::move(p));
  }
  return out;
}

std::optional<SmartPlaylist> Library::smart_playlist(std::int64_t id) {
  for (SmartPlaylist& p : smart_playlists())
    if (p.id == id) return std::move(p);
  return std::nullopt;
}

std::int64_t Library::create_smart_playlist(const std::string& name, bool match_all, const std::vector<SmartRule>& rules) {
  Stmt i(db_, "INSERT INTO smart_playlists(name, match_all, rules, position) VALUES (?1, ?2, ?3, (SELECT coalesce(max(position) + 1, 0) FROM smart_playlists))");
  i.bind(1, name).bind(2, match_all ? 1 : 0).bind(3, smart_rules_json(rules));
  i.run();
  return db_.last_insert_id();
}

void Library::update_smart_playlist(const SmartPlaylist& p) {
  Stmt u(db_, "UPDATE smart_playlists SET name = ?1, match_all = ?2, rules = ?3 WHERE id = ?4");
  u.bind(1, p.name).bind(2, p.match_all ? 1 : 0).bind(3, smart_rules_json(p.rules)).bind(4, p.id);
  u.run();
}

void Library::delete_smart_playlist(std::int64_t id) {
  Stmt d(db_, "DELETE FROM smart_playlists WHERE id = ?1");
  d.bind(1, id);
  d.run();
}

std::vector<std::int64_t> Library::smart_track_ids(std::int64_t smart_id, const std::string& search, SortKey sort, bool descending) {
  const auto p = smart_playlist(smart_id);
  if (!p) return {};
  return query_ids(p->rules, p->match_all, search, sort, descending);
}

std::string Library::setting(const std::string& key, const std::string& fallback) {
  Stmt s(db_, "SELECT value FROM settings WHERE key = ?1");
  s.bind(1, key);
  return s.step() ? s.text(0) : fallback;
}

void Library::set_setting(const std::string& key, const std::string& value) {
  Stmt u(db_, "INSERT INTO settings(key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  u.bind(1, key).bind(2, value);
  u.run();
}

}  // namespace deckhand
