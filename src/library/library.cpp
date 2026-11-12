#include "deckhand/library/library.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

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

std::vector<TrackRow> Library::tracks(std::int64_t playlist_id, const std::string& search) {
  std::string sql = std::string("SELECT ") + kTrackColumns +
                    " FROM tracks t JOIN library_roots r ON r.id = t.root_id";
  if (playlist_id) sql += " JOIN playlist_tracks p ON p.track_id = t.id AND p.playlist_id = ?1";
  sql += " WHERE (?2 = '' OR t.title LIKE ?3 OR t.artist LIKE ?3 OR t.album LIKE ?3 OR t.rel_path LIKE ?3)";
  sql += playlist_id ? " ORDER BY p.position" : " ORDER BY t.artist COLLATE NOCASE, t.title COLLATE NOCASE";
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

}  // namespace deckhand
