// Importer for the "v6" DJ library database: an encrypted SQLite file.
// Works only on a private copy of the source; the original is never opened.
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

#include "deckhand/import/import.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand {

#if !defined(DECKHAND_WITH_SQLCIPHER)

bool import_v6_available() { return false; }
ImportStats import_library_v6(Library&, const ImportOptions&) {
  throw std::runtime_error("this build cannot read encrypted source libraries");
}

#else

namespace {

namespace fs = std::filesystem;
const char* const kSource = "djlib-v6";

std::string snapshot(const ImportOptions& o) {
  fs::create_directories(o.scratch_dir);
  const fs::path dst = fs::path(o.scratch_dir) / "source-copy.db";
  std::error_code ec;
  for (const char* suffix : {"", "-wal", "-shm"}) fs::remove(dst.string() + suffix, ec);
  fs::copy_file(o.source_db, dst);
  for (const char* suffix : {"-wal", "-shm"}) {
    if (fs::exists(o.source_db + suffix)) fs::copy_file(o.source_db + suffix, dst.string() + suffix, ec);
  }
  return dst.string();
}

std::optional<std::int64_t> mapped(Stmt& find, const char* entity, const std::string& source_id) {
  find.bind(1, kSource).bind(2, entity).bind(3, source_id);
  std::optional<std::int64_t> id;
  if (find.step()) id = find.i64(0);
  find.reset();
  return id;
}

// Source cue "kind": 0 is a memory cue; hot cues are 1,2,3,5,6,... (4 is unused).
int hot_slot(std::int64_t kind) { return static_cast<int>(kind <= 3 ? kind - 1 : kind - 2); }

}  // namespace

bool import_v6_available() { return true; }

ImportStats import_library_v6(Library& lib, const ImportOptions& o) {
  if (!fs::exists(o.source_db)) throw std::runtime_error("source library not found: " + o.source_db);
  if (o.key.empty()) throw std::runtime_error("no key configured for the source library");

  Db src;
  src.open(snapshot(o));
  src.exec("PRAGMA key = '" + o.key + "';");
  try {
    Stmt probe(src, "SELECT count(*) FROM djmdContent");
    probe.step();
  } catch (const std::exception&) {
    throw std::runtime_error("could not read the source library (wrong key or unsupported version)");
  }

  ImportStats stats;
  Db& db = lib.db();
  Transaction tx(db);

  Stmt find_map(db, "SELECT local_id FROM import_map WHERE source=?1 AND entity=?2 AND source_id=?3");
  Stmt put_map(db, "INSERT OR REPLACE INTO import_map(source, entity, source_id, local_id) VALUES (?1,?2,?3,?4)");
  Stmt count_cues(db, "SELECT count(*) FROM cues WHERE track_id=?1");
  Stmt count_grid(db, "SELECT count(*) FROM beatgrids WHERE track_id=?1");
  Stmt add_cue(db, "INSERT INTO cues(track_id, kind, slot, start_ms, end_ms, label) VALUES (?1,?2,?3,?4,?5,?6)");

  // ---- tracks ----------------------------------------------------------------
  std::unordered_map<std::string, std::int64_t> track_ids;
  Stmt tracks(src,
              "SELECT c.ID, c.FolderPath, c.Title, a.Name, al.Name, g.Name, k.ScaleName, c.BPM, c.Length,"
              " c.Rating, c.ReleaseYear, c.BitRate, c.SampleRate, c.Commnt, c.AnalysisDataPath"
              " FROM djmdContent c"
              " LEFT JOIN djmdArtist a ON a.ID = c.ArtistID"
              " LEFT JOIN djmdAlbum al ON al.ID = c.AlbumID"
              " LEFT JOIN djmdGenre g ON g.ID = c.GenreID"
              " LEFT JOIN djmdKey k ON k.ID = c.KeyID"
              " WHERE c.rb_local_deleted = 0 AND c.FolderPath IS NOT NULL AND c.FolderPath != ''");
  Stmt cues(src,
            "SELECT Kind, InMsec, OutMsec, Comment FROM djmdCue"
            " WHERE ContentID = ?1 AND rb_local_deleted = 0 ORDER BY InMsec");

  while (tracks.step()) {
    const std::string source_id = tracks.text(0);
    NewTrack t;
    t.path = tracks.text(1);
    if (t.path.empty() || t.path[0] != '/') continue;  // streaming-service entries have no local file
    t.title = tracks.text(2);
    t.artist = tracks.text(3);
    t.album = tracks.text(4);
    t.genre = tracks.text(5);
    t.key = tracks.text(6);
    t.bpm = tracks.i64(7) / 100.0;
    t.duration_ms = tracks.i64(8) * 1000;
    t.rating = static_cast<int>(tracks.i64(9));
    t.year = static_cast<int>(tracks.i64(10));
    t.bitrate = static_cast<int>(tracks.i64(11));
    t.sample_rate = static_cast<int>(tracks.i64(12));
    t.comment = tracks.text(13);
    if (t.title.empty()) t.title = fs::path(t.path).stem().string();

    const bool known = lib.track_id_for_path(t.path).has_value();
    const std::int64_t id = lib.upsert_track(t);
    known ? ++stats.tracks_updated : ++stats.tracks_added;
    track_ids[source_id] = id;
    put_map.bind(1, kSource).bind(2, "track").bind(3, source_id).bind(4, id);
    put_map.run();

    // Cues and grid: only fill what DeckHand does not already have, so edits made here survive.
    count_cues.bind(1, id);
    const bool has_cues = count_cues.step() && count_cues.i64(0) > 0;
    count_cues.reset();
    if (!has_cues) {
      cues.bind(1, source_id);
      while (cues.step()) {
        const std::int64_t kind = cues.i64(0);
        const double in_ms = cues.f64(1);
        const double out_ms = cues.f64(2);
        if (in_ms < 0) continue;
        const bool loop = out_ms > in_ms;
        add_cue.bind(1, id).bind(2, loop ? "loop" : kind == 0 ? "memory" : "hot");
        if (kind == 0) {
          add_cue.bind_null(3);
        } else {
          add_cue.bind(3, hot_slot(kind));
        }
        add_cue.bind(4, in_ms);
        if (loop) {
          add_cue.bind(5, out_ms);
        } else {
          add_cue.bind_null(5);
        }
        add_cue.bind(6, cues.text(3));
        add_cue.run();
        ++stats.cues;
      }
      cues.reset();
    }

    count_grid.bind(1, id);
    const bool has_grid = count_grid.step() && count_grid.i64(0) > 0;
    count_grid.reset();
    const std::string analysis = tracks.text(14);
    if (!has_grid && !analysis.empty() && !o.analysis_root.empty()) {
      const auto grid = read_analysis_beatgrid(o.analysis_root + analysis);
      if (!grid.empty()) {
        lib.replace_beatgrid(id, grid);
        ++stats.beatgrids;
      }
    }
  }

  // ---- playlists (plain lists and folders only) --------------------------------
  struct SrcList {
    std::string id, name, parent;
    bool folder;
  };
  std::vector<SrcList> lists;
  {
    Stmt s(src,
           "SELECT ID, Name, Attribute, ParentID FROM djmdPlaylist"
           " WHERE rb_local_deleted = 0 AND Attribute IN (0, 1) ORDER BY Seq");
    while (s.step()) lists.push_back({s.text(0), s.text(1), s.text(3), s.i64(2) == 1});
  }
  std::unordered_map<std::string, std::int64_t> list_ids;
  int position = 0;
  for (const auto& l : lists) {
    auto id = mapped(find_map, "playlist", l.id);
    if (id) {
      Stmt u(db, "UPDATE playlists SET name=?1, is_folder=?2, position=?3 WHERE id=?4");
      u.bind(1, l.name).bind(2, l.folder ? 1 : 0).bind(3, position).bind(4, *id);
      u.run();
      Stmt check(db, "SELECT 1 FROM playlists WHERE id=?1");
      check.bind(1, *id);
      if (!check.step()) id.reset();
    }
    if (!id) {
      Stmt i(db, "INSERT INTO playlists(parent_id, name, is_folder, position) VALUES (NULL, ?1, ?2, ?3)");
      i.bind(1, l.name).bind(2, l.folder ? 1 : 0).bind(3, position);
      i.run();
      id = db.last_insert_id();
    }
    ++position;
    list_ids[l.id] = *id;
    put_map.bind(1, kSource).bind(2, "playlist").bind(3, l.id).bind(4, *id);
    put_map.run();
    ++stats.playlists;
  }
  for (const auto& l : lists) {
    const auto parent = list_ids.find(l.parent);
    Stmt u(db, "UPDATE playlists SET parent_id=?1 WHERE id=?2");
    if (parent != list_ids.end()) {
      u.bind(1, parent->second);
    } else {
      u.bind_null(1);
    }
    u.bind(2, list_ids[l.id]);
    u.run();
  }
  {
    Stmt entries(src,
                 "SELECT ContentID FROM djmdSongPlaylist WHERE PlaylistID = ?1 AND rb_local_deleted = 0"
                 " ORDER BY TrackNo");
    Stmt clear(db, "DELETE FROM playlist_tracks WHERE playlist_id = ?1");
    Stmt add(db, "INSERT INTO playlist_tracks(playlist_id, position, track_id) VALUES (?1, ?2, ?3)");
    for (const auto& l : lists) {
      if (l.folder) continue;
      const std::int64_t local = list_ids[l.id];
      clear.bind(1, local);
      clear.run();
      entries.bind(1, l.id);
      int pos = 0;
      while (entries.step()) {
        const auto it = track_ids.find(entries.text(0));
        if (it == track_ids.end()) continue;
        add.bind(1, local).bind(2, pos++).bind(3, it->second);
        add.run();
      }
      entries.reset();
    }
  }

  tx.commit();
  src.close();
  std::error_code ec;
  for (const char* suffix : {"", "-wal", "-shm"}) {
    fs::remove((fs::path(o.scratch_dir) / "source-copy.db").string() + suffix, ec);
  }
  DH_LOG(Info, "import: %d added, %d updated, %d playlists, %d cues, %d grids", stats.tracks_added,
         stats.tracks_updated, stats.playlists, stats.cues, stats.beatgrids);
  return stats;
}

#endif

}  // namespace deckhand
