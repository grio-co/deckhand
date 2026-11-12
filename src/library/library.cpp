#include "deckhand/library/library.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <utility>

#include "deckhand/log/log.hpp"

namespace deckhand {
namespace {

const char* const kSchema =
#include "deckhand_schema.inc"
    ;

// Portable layout: every track lives under a root; root 1 is the filesystem root.
constexpr std::int64_t kSystemRoot = 1;

std::string join_path(const std::string& root, const std::string& rel) {
  if (root.empty() || root.back() == '/') return root + rel;
  return root + "/" + rel;
}

std::string rel_of(const std::string& abs) { return abs.size() > 1 && abs[0] == '/' ? abs.substr(1) : abs; }

std::string extension_of(const std::string& path) {
  const auto dot = path.find_last_of('.');
  const auto slash = path.find_last_of('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  std::string ext = path.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext;
}

const char* const kTrackColumns =
    "t.id, r.last_seen_path, t.rel_path, t.title, t.artist, t.album, t.genre, t.key, t.format, t.bpm,"
    " t.duration_ms, t.rating";

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
  return t;
}

}  // namespace

Library::Library(const std::string& db_path) {
  db_.open(db_path);
  db_.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
  bool has_schema = false;
  {
    Stmt s(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='schema_version'");
    has_schema = s.step();
  }
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

std::vector<TrackRow> Library::tracks(std::int64_t playlist_id, const std::string& search, SortKey sort,
                                      bool descending) {
  std::string sql = std::string("SELECT ") + kTrackColumns +
                    " FROM tracks t JOIN library_roots r ON r.id = t.root_id";
  if (playlist_id) sql += " JOIN playlist_tracks p ON p.track_id = t.id AND p.playlist_id = ?1";
  sql += " WHERE (?2 = '' OR t.title LIKE ?3 OR t.artist LIKE ?3 OR t.album LIKE ?3 OR t.rel_path LIKE ?3)";
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
  }
  if (column) {
    sql += std::string(" ORDER BY ") + column + (descending ? " DESC" : " ASC") + ", t.title COLLATE NOCASE, t.id";
  } else {
    sql += playlist_id ? " ORDER BY p.position" : " ORDER BY t.artist COLLATE NOCASE, t.title COLLATE NOCASE";
  }
  Stmt s(db_, sql.c_str());
  if (playlist_id) s.bind(1, playlist_id);
  s.bind(2, search);
  s.bind(3, "%" + search + "%");
  std::vector<TrackRow> out;
  while (s.step()) out.push_back(read_track(s));
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
  Stmt s(db_, "SELECT kind, slot, start_ms, end_ms, label FROM cues WHERE track_id = ?1 ORDER BY start_ms");
  s.bind(1, track_id);
  while (s.step()) {
    Cue c;
    const std::string kind = s.text(0);
    c.kind = kind == "hot" ? CueKind::Hot : kind == "loop" ? CueKind::Loop : CueKind::Memory;
    c.slot = s.is_null(1) ? -1 : static_cast<int>(s.i64(1));
    c.start_ms = s.f64(2);
    c.end_ms = s.is_null(3) ? -1 : s.f64(3);
    c.label = s.text(4);
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
  clear_hot_cue(track_id, slot);
  Stmt i(db_, "INSERT INTO cues(track_id, kind, slot, start_ms, end_ms) VALUES (?1, ?2, ?3, ?4, ?5)");
  i.bind(1, track_id).bind(2, end_ms >= 0 ? "loop" : "hot").bind(3, slot).bind(4, start_ms);
  if (end_ms >= 0) {
    i.bind(5, end_ms);
  } else {
    i.bind_null(5);
  }
  i.run();
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
