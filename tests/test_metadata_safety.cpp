// Edit journal, undo, re-import protection and the trash.
#include <cstdio>
#include <filesystem>
#include <string>

#include "check.hpp"
#include "deckhand/library/backup.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;

namespace {
std::string fresh(const char* name) {
  const std::string path = (std::filesystem::temp_directory_path() / name).string();
  for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
  return path;
}

std::int64_t add(Library& lib, const char* path, const char* title, double bpm = 120) {
  NewTrack t;
  t.path = path;
  t.title = title;
  t.artist = "Zed";
  t.bpm = bpm;
  return lib.upsert_track(t);
}

std::int64_t count(Library& lib, const char* sql) {
  Stmt s(lib.db(), sql);
  return s.step() ? s.i64(0) : -1;
}
}  // namespace

static void test_journal_and_undo() {
  Library lib(fresh("deckhand_test_edits.db"));
  const auto a = add(lib, "/m/a.mp3", "Alpha", 126);
  CHECK(lib.track_history(a).empty());  // creating a track is not an edit

  TrackEdit e;
  e.title = "Alpha";  // same value: nothing to record
  e.genre = "House";
  lib.edit_track(a, e);
  auto h = lib.track_history(a);
  CHECK(h.size() == 1 && h[0].field == "genre" && h[0].source == "user" && h[0].new_value == "House");
  CHECK(h[0].old_value.value_or("") == "");  // was empty (or NULL); either way journalled once

  // Several fields of one call share a batch; an API scope names its source.
  std::int64_t batch = 0;
  {
    EditScope scope("api");
    TrackEdit m;
    m.title = "Alpha (Remix)";
    m.rating = 4;
    lib.edit_track(a, m);
    lib.set_track_color(a, 0xff0000);
    batch = scope.batch();
  }
  h = lib.track_history(a);
  CHECK(h.size() == 4 && batch != 0);
  int in_batch = 0;
  for (const auto& r : h) {
    if (r.batch_id == batch) {
      ++in_batch;
      CHECK(r.source == "api");
    }
  }
  CHECK(in_batch == 3);
  lib.set_bpm(a, 126);  // unchanged
  lib.set_bpm(a, 127.5);
  CHECK(lib.track_history(a).size() == 5 && lib.track_history(a)[0].field == "bpm");
  CHECK(lib.recent_edits(2).size() == 2 && lib.recent_edits(2)[0].track_id == a);

  // Undo one edit.
  const auto bpm_edit = lib.track_history(a)[0].id;
  auto u = lib.undo_edit(bpm_edit);
  CHECK(u.status == UndoStatus::Ok && u.applied == 1 && lib.track(a)->bpm == 126);
  CHECK(lib.track_history(a)[0].source == "undo" && lib.track_history(a)[0].new_value == "126");
  // The field no longer holds the edit's new value: conflict, nothing changes.
  u = lib.undo_edit(bpm_edit);
  CHECK(u.status == UndoStatus::Conflict && lib.track(a)->bpm == 126);
  CHECK(lib.undo_edit(999999).status == UndoStatus::NotFound);

  // Conflict inside a batch: the whole undo is refused.
  TrackEdit later;
  later.rating = 2;
  lib.edit_track(a, later);
  u = lib.undo_batch(batch);
  CHECK(u.status == UndoStatus::Conflict && u.conflicts == 1 && lib.track(a)->title == "Alpha (Remix)" && lib.track(a)->color == 0xff0000);
  later.rating = 4;
  lib.edit_track(a, later);
  const auto before = lib.track_history(a).size();
  u = lib.undo_batch(batch);
  const auto row = lib.track(a);
  CHECK(u.status == UndoStatus::Ok && u.applied == 3 && row->title == "Alpha" && row->rating == 0 && row->color == 0);
  CHECK(lib.track_history(a).size() == before + 3 && lib.track_history(a)[0].source == "undo");
  CHECK(count(lib, "SELECT count(DISTINCT batch_id) FROM track_edits WHERE source = 'undo' AND id > 0") >= 1);
  // Undoing an undo is a redo.
  CHECK(lib.undo_batch(u.batch_id).status == UndoStatus::Ok && lib.track(a)->title == "Alpha (Remix)");

  // Grid lock is journalled; unchanged is not.
  lib.set_grid_locked(a, false);
  const auto n = lib.track_history(a).size();
  lib.set_grid_locked(a, true);
  CHECK(lib.track_history(a).size() == n + 1 && lib.track_history(a)[0].field == "grid_locked");
}

static void test_analysis_source_and_cues() {
  Library lib(fresh("deckhand_test_analysis_src.db"));
  const auto a = add(lib, "/m/a.mp3", "Alpha", 0);
  {
    EditScope scope("analysis");
    lib.set_bpm(a, 128);
    lib.set_key(a, "8A");
  }
  CHECK(lib.track_history(a).empty() && lib.recent_edits(10).empty());  // not offered as user edits
  CHECK(lib.track_history(a, 50, true).size() == 2 && lib.track_history(a, 50, true)[0].source == "analysis");
  // An import may replace analysis results (they are not user edits).
  NewTrack t;
  t.path = "/m/a.mp3";
  t.title = "Alpha";
  t.bpm = 130;
  t.key = "9A";
  lib.upsert_track(t);
  CHECK(lib.track(a)->bpm == 130 && lib.track(a)->key == "9A");

  // Load cue columns.
  CHECK(!lib.track_cues(a).main_cue_ms && !lib.track_cues(a).load_cue_ms);
  lib.set_analysis_cues(a, 1500.5, 1200);
  lib.set_main_cue_ms(a, 3000);
  const auto c = lib.track_cues(a);
  CHECK(c.load_cue_ms == 1500.5 && c.first_sound_ms == 1200 && c.main_cue_ms == 3000);
  lib.set_main_cue_ms(a, std::nullopt);
  CHECK(!lib.track_cues(a).main_cue_ms);
  lib.remove_track(a);
  CHECK(lib.restore_track(a) && lib.track_cues(a).load_cue_ms == 1500.5);
}

static void test_cap() {
  Library lib(fresh("deckhand_test_edits_cap.db"));
  const auto a = add(lib, "/m/a.mp3", "Alpha");
  const auto b = add(lib, "/m/b.mp3", "Bravo");
  for (int i = 0; i < 260; ++i) {
    TrackEdit e;
    e.comment = "c" + std::to_string(i);
    lib.edit_track(a, e);
  }
  TrackEdit eb;
  eb.comment = "kept";
  lib.edit_track(b, eb);
  const auto h = lib.track_history(a, 1000);
  CHECK(h.size() == 200 && h[0].new_value == "c259" && h[199].new_value == "c60");
  CHECK(lib.track_history(b).size() == 1);
}

static void test_import_protection() {
  Library lib(fresh("deckhand_test_edits_import.db"));
  const auto a = add(lib, "/m/a.mp3", "Imported title", 120);
  const auto b = add(lib, "/m/b.mp3", "Other", 100);
  TrackEdit e;
  e.title = "My title";
  lib.edit_track(a, e);
  {
    EditScope scope("api");
    TrackEdit g;
    g.genre = "Techno";
    lib.edit_track(a, g);
  }

  NewTrack t;
  t.path = "/m/a.mp3";
  t.title = "Imported title v2";
  t.artist = "New artist";
  t.genre = "Pop";
  t.bpm = 130;
  t.duration_ms = 1000;
  lib.upsert_track(t);  // default: protect
  auto row = lib.track(a);
  CHECK(row->title == "My title" && row->genre == "Techno");  // edited by the user / the API: kept
  CHECK(row->artist == "New artist" && row->bpm == 130 && row->duration_ms == 1000);  // untouched fields follow the import
  auto h = lib.track_history(a);
  CHECK(h[0].source == "import" || h[1].source == "import");

  // Once the latest entry of a field is an import, the next import may replace it.
  t.artist = "Newer artist";
  lib.upsert_track(t);
  CHECK(lib.track(a)->artist == "Newer artist");

  t.title = "Forced title";
  t.genre = "Forced";
  lib.upsert_track(t, true);  // explicit override
  row = lib.track(a);
  CHECK(row->title == "Forced title" && row->genre == "Forced");
  CHECK(lib.track_history(a)[0].source == "import");
  CHECK(lib.track(b)->title == "Other");
}

static void test_trash() {
  const std::string path = fresh("deckhand_test_trash.db");
  std::int64_t a = 0, b = 0, c = 0, pl = 0;
  {
    Library lib(path);
    a = add(lib, "/m/a.mp3", "Alpha", 126);
    b = add(lib, "/m/b.mp3", "Bravo");
    c = add(lib, "/m/c.mp3", "Charlie");
    lib.replace_beatgrid(b, {{250, 126, 3}});
    lib.set_hot_cue(b, 1, 1500);
    lib.replace_memory_cues(b, {{CueKind::Memory, -1, 800, -1, "drop", -1}});
    pl = lib.create_playlist(0, "Set", false);
    lib.add_to_playlist(pl, a);
    lib.add_to_playlist(pl, b);
    lib.add_to_playlist(pl, c);
    TrackEdit e;
    e.genre = "Dub";
    lib.edit_track(b, e);

    lib.remove_track(b);
    CHECK(lib.track_count() == 2 && !lib.track(b));
    CHECK(lib.tracks(0, "Bravo").empty() && lib.playlist_track_ids(pl).size() == 2);
    CHECK(lib.query_ids({}, true, "Bravo").empty());
    const auto trash = lib.deleted_tracks();
    CHECK(trash.size() == 1 && trash[0].id == b && trash[0].title == "Bravo" && trash[0].path == "/m/b.mp3");

    std::int64_t back = 0;
    CHECK(lib.restore_track(b, &back) && back == b);
    CHECK(lib.deleted_tracks().empty() && lib.track_count() == 3);
    CHECK(lib.track(b)->title == "Bravo" && lib.track(b)->genre == "Dub");
    CHECK(lib.beatgrid(b).size() == 1 && lib.beatgrid(b)[0].bar_beat == 3);
    CHECK(lib.cues(b).size() == 2 && lib.memory_cues(b)[0].label == "drop");
    const auto ids = lib.playlist_track_ids(pl);
    CHECK(ids.size() == 3 && ids[1] == b);  // same place in the playlist
    CHECK(lib.tracks(0, "Bravo").size() == 1);
    CHECK(lib.track_history(b).size() == 1);  // history survives the round trip
    CHECK(!lib.restore_track(b));

    // A file added again under another entry blocks the restore.
    lib.remove_track(a);
    add(lib, "/m/a.mp3", "Alpha again");
    CHECK(!lib.restore_track(a));

    lib.remove_track(c);
    CHECK(lib.deleted_tracks().size() == 2);
    // Age the trash: only entries older than the limit go.
    lib.db().exec("UPDATE trash_tracks SET deleted_at = deleted_at - 40 * 86400 WHERE id = " + std::to_string(c));
    CHECK(lib.purge_deleted(30) == 1 && lib.deleted_tracks().size() == 1);
    CHECK(!lib.restore_track(c));
    lib.db().exec("UPDATE trash_tracks SET deleted_at = deleted_at - 40 * 86400");
  }
  {
    Library lib(path);  // purge runs when the library opens
    CHECK(lib.deleted_tracks().empty());
    CHECK(count(lib, "SELECT count(*) FROM trash_cues") == 0 && count(lib, "SELECT count(*) FROM trash_beatgrids") == 0);
    CHECK(count(lib, "SELECT count(*) FROM trash_tracks") == 0);
  }
}

static void test_schema4_migration() {
  const std::string path = fresh("deckhand_test_meta_migrate.db");
  std::int64_t a = 0;
  {
    Library lib(path);
    NewTrack t;
    t.path = "/m/a.mp3";
    t.title = "Alpha";
    t.bpm = 126;
    a = lib.upsert_track(t);
    lib.replace_beatgrid(a, {{250, 126, 3}});
    lib.replace_memory_cues(a, {{CueKind::Memory, -1, 800, -1, "drop", -1}});
    const auto pl = lib.create_playlist(0, "Set", false);
    lib.add_to_playlist(pl, a);
    lib.set_setting("k", "v");
    // As the previous release left it.
    lib.db().exec("DROP TABLE track_edits; DROP TABLE trash_tracks; DROP TABLE trash_cues; DROP TABLE trash_beatgrids;"
                  "DROP TABLE trash_playlist_tracks; DROP TABLE trash_import_map; UPDATE schema_version SET version = 4;");
  }
  {
    Library lib(path);
    CHECK(count(lib, "SELECT max(version) FROM schema_version") == kLibrarySchemaVersion && kLibrarySchemaVersion == 10);
    const auto copies = library_backups(path);
    CHECK(!copies.empty() && copies[0].reason == "upgrade" && copies[0].schema_version == 4 && copies[0].tracks == 1);
    CHECK(lib.track_count() == 1 && lib.track(a)->title == "Alpha" && lib.track(a)->bpm == 126);
    CHECK(lib.beatgrid(a).size() == 1 && lib.memory_cues(a).size() == 1 && lib.playlists().size() == 1 && lib.setting("k") == "v");
    CHECK(lib.track_history(a).empty() && lib.deleted_tracks().empty());
    TrackEdit e;
    e.genre = "House";
    lib.edit_track(a, e);
    CHECK(lib.track_history(a).size() == 1);
    lib.remove_track(a);
    CHECK(lib.restore_track(a) && lib.track(a)->genre == "House");
  }
}

static void test_schema5_migration() {
  const std::string path = fresh("deckhand_test_meta_migrate5.db");
  std::int64_t a = 0;
  {
    Library lib(path);
    NewTrack t;
    t.path = "/m/a.mp3";
    t.title = "Alpha";
    a = lib.upsert_track(t);
    lib.set_hot_cue(a, 1, 900);
    // As schema 5 left it: no size / hash columns.
    lib.db().exec("DROP INDEX idx_tracks_file_size; DROP INDEX idx_tracks_content_hash;"
                  "ALTER TABLE tracks DROP COLUMN file_size; ALTER TABLE tracks DROP COLUMN content_hash;"
                  "ALTER TABLE trash_tracks DROP COLUMN file_size; ALTER TABLE trash_tracks DROP COLUMN content_hash;"
                  "UPDATE schema_version SET version = 5;");
  }
  {
    Library lib(path);
    CHECK(count(lib, "SELECT max(version) FROM schema_version") == kLibrarySchemaVersion);
    const auto copies = library_backups(path);
    CHECK(!copies.empty() && copies[0].reason == "upgrade" && copies[0].schema_version == 5);
    CHECK(count(lib, "SELECT count(*) FROM pragma_table_info('tracks') WHERE name IN ('file_size', 'content_hash')") == 2);
    CHECK(count(lib, "SELECT count(*) FROM pragma_table_info('trash_tracks') WHERE name IN ('file_size', 'content_hash')") == 2);
    CHECK(count(lib, "SELECT file_size IS NULL FROM tracks") == 1);  // unknown until hash_pending runs
    CHECK(lib.track(a)->title == "Alpha" && lib.cues(a).size() == 1);
    CHECK(lib.hash_pending(10).missing == 1);  // /m/a.mp3 does not exist
    lib.remove_track(a);
    CHECK(lib.restore_track(a));
  }
}

// Schema 6 -> 7: automix points, the play queue and the last-sound column appear, nothing is lost, a copy is taken.
static void test_schema6_migration() {
  const std::string path = fresh("deckhand_test_meta_migrate6.db");
  std::int64_t a = 0;
  {
    Library lib(path);
    NewTrack t;
    t.path = "/m/a.mp3";
    t.title = "Alpha";
    a = lib.upsert_track(t);
    lib.set_analysis_cues(a, 1000, 500);
    lib.set_main_cue_ms(a, 1234);
    // As schema 6 left it: no automix tables, no last_sound_ms column (not in the trash either).
    lib.db().exec("DROP TABLE automix_points; DROP TABLE play_queue; DROP TABLE play_queue_history;"
                  "ALTER TABLE tracks DROP COLUMN last_sound_ms; ALTER TABLE trash_tracks DROP COLUMN last_sound_ms;"
                  "UPDATE schema_version SET version = 6;");
  }
  {
    Library lib(path);
    CHECK(count(lib, "SELECT max(version) FROM schema_version") == 10 && kLibrarySchemaVersion == 10);
    const auto copies = library_backups(path);
    CHECK(!copies.empty() && copies[0].reason == "upgrade" && copies[0].schema_version == 6);
    CHECK(count(lib, "SELECT count(*) FROM pragma_table_info('tracks') WHERE name = 'last_sound_ms'") == 1);
    CHECK(count(lib, "SELECT count(*) FROM pragma_table_info('trash_tracks') WHERE name = 'last_sound_ms'") == 1);
    CHECK(lib.track(a)->title == "Alpha");
    const TrackCues c = lib.track_cues(a);
    CHECK(c.main_cue_ms && *c.main_cue_ms == 1234 && c.load_cue_ms && *c.load_cue_ms == 1000 && !c.last_sound_ms);
    // The new tables work: points round-trip, an empty set removes the row, the queue and its history persist.
    CHECK(lib.automix_points(a).empty());
    AutomixPoints p;
    p.mix_in_ms = 2500;
    p.mix_out_ms = 150000;
    p.transition_beats = 8;
    p.transition_type = "cut";
    p.max_play_ms = 90000;
    lib.set_automix_points(a, p);
    const AutomixPoints q = lib.automix_points(a);
    CHECK(q.mix_in_ms && *q.mix_in_ms == 2500 && q.mix_out_ms && *q.mix_out_ms == 150000 && q.transition_beats && *q.transition_beats == 8);
    CHECK(q.transition_type && *q.transition_type == "cut" && q.max_play_ms && *q.max_play_ms == 90000);
    p.mix_in_ms.reset();
    lib.set_automix_points(a, p);
    CHECK(!lib.automix_points(a).mix_in_ms && lib.automix_points(a).mix_out_ms);
    lib.set_automix_points(a, AutomixPoints{});
    CHECK(count(lib, "SELECT count(*) FROM automix_points") == 0);
    lib.set_last_sound_ms(a, 200000);
    CHECK(lib.track_cues(a).last_sound_ms && *lib.track_cues(a).last_sound_ms == 200000);
    lib.remove_track(a);  // the trash keeps the column, a restore brings it back
    CHECK(lib.restore_track(a) && lib.track_cues(a).last_sound_ms && *lib.track_cues(a).last_sound_ms == 200000);
    std::vector<QueueRow> rows(2);
    rows[0].entry_id = 5;
    rows[0].track_id = a;
    rows[0].pending = true;
    rows[1].entry_id = 6;
    rows[1].track_id = 99;
    rows[1].overrides.max_play_ms = 1000;
    lib.save_play_queue(rows);
    const auto back = lib.play_queue();
    CHECK(back.size() == 2 && back[0].entry_id == 5 && back[0].pending && back[1].overrides.max_play_ms && lib.max_queue_entry_id() == 6);
    lib.add_queue_history(5, a, 100);
    lib.add_queue_history(6, 99, 200);
    CHECK(lib.queue_history(10).size() == 2 && lib.queue_history(10)[0].track_id == 99 && lib.max_queue_entry_id() == 6);
    CHECK(lib.pop_queue_history() && lib.queue_history(10).size() == 1);
  }
}

int main() {
  test_journal_and_undo();
  test_analysis_source_and_cues();
  test_cap();
  test_import_protection();
  test_trash();
  test_schema4_migration();
  test_schema5_migration();
  test_schema6_migration();
  std::puts("test_metadata_safety ok");
  return 0;
}
