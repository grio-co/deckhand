// Cues that follow the grid, selective analysis removal, duplicate finder and merge.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "check.hpp"
#include "deckhand/library/backup.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {
std::string fresh(const char* name) {
  const std::string path = (fs::temp_directory_path() / name).string();
  for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
  return path;
}

std::int64_t add(Library& lib, const std::string& path, const char* title, const char* artist = "Zed", double bpm = 120, std::int64_t dur = 200000) {
  NewTrack t;
  t.path = path;
  t.title = title;
  t.artist = artist;
  t.bpm = bpm;
  t.duration_ms = dur;
  return lib.upsert_track(t);
}

std::int64_t count(Library& lib, const std::string& sql) {
  Stmt s(lib.db(), sql.c_str());
  return s.step() ? s.i64(0) : -1;
}

bool near(double a, double b) { return std::fabs(a - b) < 0.01; }

const Cue* find_slot(const std::vector<Cue>& v, int slot) {
  for (const Cue& c : v)
    if (c.slot == slot) return &c;
  return nullptr;
}

void setup_cues(Library& lib, std::int64_t id) {
  lib.replace_beatgrid(id, {{100, 120, 1}});
  lib.set_hot_cue(id, 0, 2100);          // beat 4
  lib.set_hot_cue(id, 1, 2300);          // off grid (vocal)
  Cue m;
  m.kind = CueKind::Memory;
  m.start_ms = 2605;                     // 5 ms late of beat 5: still on the grid
  m.label = "drop\tone";
  Cue loop;
  loop.kind = CueKind::Loop;
  loop.start_ms = 2100;
  loop.end_ms = 4100;                    // 4 beats
  lib.replace_memory_cues(id, {m, loop});
}
}  // namespace

static void test_follow_always_and_undo() {
  Library lib(fresh("deckhand_test_tools_follow.db"));
  const auto id = add(lib, "/m/a.mp3", "A");
  setup_cues(lib, id);
  const auto old_cues = lib.cues(id);
  const GridCommit c = lib.commit_grid(id, {{140, 120, 1}}, 120.0, true, CueFollow::Always);
  CHECK(c.grid_changed && c.applied && c.batch_id > 0);
  CHECK(c.proposal.moves.size() == 3 && c.proposal.off_grid.size() == 1 && c.proposal.off_grid[0].slot == 1);
  const auto now = lib.cues(id);
  CHECK(near(find_slot(now, 0)->start_ms, 2140));
  CHECK(near(find_slot(now, 1)->start_ms, 2300));  // off-grid stays
  bool saw_mem = false, saw_loop = false;
  for (const Cue& q : now) {
    if (q.kind == CueKind::Memory) saw_mem = near(q.start_ms, 2645) && q.label == "drop\tone";
    if (q.kind == CueKind::Loop) saw_loop = near(q.start_ms, 2140) && near(q.end_ms - q.start_ms, 2000);
  }
  CHECK(saw_mem && saw_loop);
  CHECK(lib.grid_locked(id) && lib.beatgrid(id)[0].first_beat_ms == 140);

  const UndoResult u = lib.undo_batch(c.batch_id);
  CHECK(u.status == UndoStatus::Ok);
  CHECK(lib.beatgrid(id)[0].first_beat_ms == 100 && !lib.grid_locked(id));
  const auto back = lib.cues(id);
  CHECK(back.size() == old_cues.size());
  CHECK(near(find_slot(back, 0)->start_ms, 2100));
  bool label_ok = false;
  for (const Cue& q : back) label_ok = label_ok || (q.kind == CueKind::Memory && q.label == "drop\tone" && near(q.start_ms, 2605));
  CHECK(label_ok);
}

static void test_follow_ask_never_and_tempo() {
  Library lib(fresh("deckhand_test_tools_ask.db"));
  const auto id = add(lib, "/m/a.mp3", "A");
  setup_cues(lib, id);
  const GridCommit never = lib.commit_grid(id, {{140, 120, 1}}, std::nullopt, std::nullopt, CueFollow::Never);
  CHECK(!never.applied && near(find_slot(lib.cues(id), 0)->start_ms, 2100));
  lib.commit_grid(id, {{100, 120, 1}}, std::nullopt, std::nullopt, CueFollow::Never);  // back

  const GridCommit ask = lib.commit_grid(id, {{140, 120, 1}}, std::nullopt, std::nullopt, CueFollow::Ask);
  CHECK(!ask.applied && ask.proposal.moves.size() == 3 && near(find_slot(lib.cues(id), 0)->start_ms, 2100));
  const FollowResult f = lib.apply_cue_follow(id, ask.batch_id);
  CHECK(f.status == FollowStatus::Ok && f.proposal.moves.size() == 3);
  CHECK(near(find_slot(lib.cues(id), 0)->start_ms, 2140));
  CHECK(lib.apply_cue_follow(id, ask.batch_id).status == FollowStatus::AlreadyApplied);
  CHECK(lib.undo_batch(ask.batch_id).status == UndoStatus::Ok);  // grid and cues in one undo
  CHECK(near(find_slot(lib.cues(id), 0)->start_ms, 2100) && lib.beatgrid(id)[0].first_beat_ms == 100);

  // Tempo change of the section: the cue stays on its beat (beat 4 of 120 -> 100 bpm: 100 + 4 * 600).
  const GridCommit t = lib.commit_grid(id, {{100, 100, 1}}, 100.0, std::nullopt, CueFollow::Always);
  CHECK(t.applied && near(find_slot(lib.cues(id), 0)->start_ms, 100 + 4 * 600.0));

  // Loop length in beats is kept.
  for (const Cue& q : lib.cues(id))
    if (q.kind == CueKind::Loop) CHECK(near(q.end_ms - q.start_ms, 4 * 600.0));
  // The setting parser.
  CHECK(cue_follow_from("always") == CueFollow::Always && cue_follow_from("x") == CueFollow::Ask);
}

static void test_halve_double_leaves_positions() {
  Library lib(fresh("deckhand_test_tools_half.db"));
  const auto id = add(lib, "/m/a.mp3", "A");
  setup_cues(lib, id);
  const auto before = lib.cues(id);
  for (const double bpm : {60.0, 240.0}) {
    const GridCommit c = lib.commit_grid(id, {{100, bpm, 1}}, bpm, std::nullopt, CueFollow::Always);
    CHECK(c.grid_changed && !c.applied && c.proposal.moves.empty() && c.proposal.unchanged.size() == 3);
    const auto now = lib.cues(id);
    CHECK(now.size() == before.size());
    for (std::size_t i = 0; i < now.size(); ++i) CHECK(near(now[i].start_ms, before[i].start_ms));
  }
  // No grid at all: nothing can be mapped.
  CHECK(propose_cue_follow(before, {}, {{0, 120, 1}}).no_grid);
  // Round trip of the journal text.
  CHECK(cues_from_text(cues_text(before)).size() == before.size());
  CHECK(grid_from_text(grid_text({{1.5, 123.25, 2}}))[0].bpm == 123.25);
}

static void test_strip() {
  Library lib(fresh("deckhand_test_tools_strip.db"));
  const auto a = add(lib, "/m/a.mp3", "A", "Zed", 126);
  const auto b = add(lib, "/m/b.mp3", "B", "Zed", 128);
  for (const auto id : {a, b}) {
    lib.replace_beatgrid(id, {{0, 126, 1}});
    lib.set_gain_db(id, -3);
    lib.set_analysis_cues(id, 500, 400);
    lib.set_hot_cue(id, 0, 1000);
    lib.set_main_cue_ms(id, 700);
    Stmt w(lib.db(), "INSERT INTO waveforms(track_id, kind, data) VALUES (?1, 'x', x'00')");
    w.bind(1, id);
    w.run();
  }
  {
    EditScope scope("analysis");
    lib.set_key(a, "8A");
    lib.set_key(b, "9A");
  }
  lib.set_grid_locked(b, true);

  lib.set_phrases(a, StoredPhrases{});
  StripKinds k;
  k.grid = k.key = k.loudness = k.waveform = k.first_sound = k.phrases = k.vocals = k.stems = true;
  StripResult dry = lib.strip_analysis({a, b}, k, false, true);
  CHECK(dry.dry_run && dry.cleared["grid"] == 1 && dry.skipped_grid_locked == 1 && dry.cleared["key"] == 2);
  CHECK(lib.track(a)->key == "8A" && lib.beatgrid(a).size() == 1 && lib.gain_db(a) && lib.phrases(a));  // untouched

  StripKinds only_key;
  only_key.key = true;
  StripResult r1 = lib.strip_analysis({a}, only_key);
  CHECK(r1.cleared["key"] == 1 && lib.track(a)->key.empty() && lib.beatgrid(a).size() == 1 && lib.gain_db(a));
  CHECK(lib.undo_batch(r1.batch_id).status == UndoStatus::Ok && lib.track(a)->key == "8A");

  StripResult r = lib.strip_analysis({a, b}, k);
  CHECK(r.tracks == 2 && r.cleared["grid"] == 1 && r.skipped_grid_locked == 1 && r.notes.size() >= 2 && r.cleared["phrases"] == 1 && !lib.phrases(a));
  CHECK(lib.beatgrid(a).empty() && lib.track(a)->bpm == 0 && !lib.gain_db(a) && !lib.track_cues(a).load_cue_ms && !lib.track_cues(a).first_sound_ms);
  CHECK(count(lib, "SELECT count(*) FROM waveforms") == 0);
  CHECK(lib.beatgrid(b).size() == 1 && lib.track(b)->bpm == 128);          // locked: kept
  CHECK(lib.cues(a).size() == 1 && lib.track_cues(a).main_cue_ms == 700);   // user data untouched
  CHECK(lib.undo_batch(r.batch_id).status == UndoStatus::Ok);
  CHECK(lib.beatgrid(a).size() == 1 && lib.track(a)->bpm == 126 && lib.track(a)->key == "8A");
  CHECK(!lib.gain_db(a));  // loudness is not undoable

  StripResult forced = lib.strip_analysis({b}, StripKinds{true}, true);
  CHECK(forced.cleared["grid"] == 1 && lib.beatgrid(b).empty() && !lib.grid_locked(b));
}

static void test_duplicates_and_merge() {
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_tools_dups";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const auto write = [&](const char* name, const std::string& body) {
    std::ofstream(dir / name, std::ios::binary) << body;
    return (dir / name).string();
  };
  const std::string same = std::string(5000, 'x') + "audio";
  const std::string p1 = write("one.flac", same), p2 = write("two.flac", same), p3 = write("three.mp3", "different mp3 bytes");
  const std::string p4 = write("four.mp3", "another mp3, other size!!");

  Library lib(fresh("deckhand_test_tools_dups.db"));
  const auto t1 = add(lib, p1, "Song", "Art", 120, 200000);
  const auto t2 = add(lib, p2, "Song", "Art", 120, 200000);
  const auto t3 = add(lib, p3, "Song (", "Art", 120, 201500);  // normalises to the same title
  const auto t4 = add(lib, p4, "Other", "Art", 120, 90000);
  Stmt fm(lib.db(), "UPDATE tracks SET format='flac', bitrate=900 WHERE id IN (?1, ?2)");
  fm.bind(1, t1).bind(2, t2);
  fm.run();
  Stmt fm3(lib.db(), "UPDATE tracks SET bitrate=192 WHERE id = ?1");
  fm3.bind(1, t3);
  fm3.run();
  CHECK(count(lib, "SELECT max(version) FROM schema_version") == kLibrarySchemaVersion && kLibrarySchemaVersion == 11);

  CHECK(lib.find_duplicates().size() == 1);  // likely only: no hashes yet
  HashProgress hp = lib.hash_pending(50);
  CHECK(hp.sized == 4 && hp.hashed == 2 && hp.remaining == 0);  // only the two same-size files are read
  CHECK(count(lib, "SELECT count(*) FROM tracks WHERE content_hash IS NOT NULL AND content_hash != ''") == 2);
  CHECK(lib.hash_pending(50).sized == 0);

  auto groups = lib.find_duplicates();
  CHECK(groups.size() == 2);
  const DuplicateGroup* exact = nullptr;
  const DuplicateGroup* likely = nullptr;
  for (const auto& g : groups) (g.kind == "exact" ? exact : likely) = &g;
  CHECK(exact && likely && exact->confidence == 1 && exact->members.size() == 2);
  CHECK(likely->members.size() == 3 && likely->confidence < 1 && likely->confidence >= 0.7);
  bool fmt = false;
  for (const auto& d : likely->differences) fmt = fmt || d == "format" || d == "bitrate";
  CHECK(fmt);
  CHECK(likely->suggestion_reason == "lossless over lossy" && (likely->suggested_keep == t1));

  DuplicateOptions strict;
  strict.likely = false;
  CHECK(lib.find_duplicates(strict).size() == 1);
  strict = {};
  strict.duration_tolerance_ms = 500;
  strict.exact = false;
  CHECK(lib.find_duplicates(strict).size() == 1 && lib.find_duplicates(strict)[0].members.size() == 2);

  // Merge t2 and t3 into t1.
  lib.set_hot_cue(t2, 0, 1000);
  lib.set_hot_cue(t2, 3, 3000);
  lib.set_hot_cue(t1, 0, 500);  // slot 0 taken on the kept track
  lib.replace_beatgrid(t3, {{10, 120, 1}});
  Cue m;
  m.start_ms = 4000;
  lib.replace_memory_cues(t3, {m});
  TrackEdit e;
  e.genre = "House";
  e.rating = 4;
  lib.edit_track(t2, e);
  const auto pl = lib.create_playlist(0, "P", false), pl2 = lib.create_playlist(0, "Q", false), pl3 = lib.create_playlist(0, "R", false);
  lib.set_playlist_tracks(pl, {t4, t2, t3});   // no kept track here: it takes the place of the first removed
  lib.set_playlist_tracks(pl2, {t1, t2});      // already has it: no double entry
  lib.set_playlist_tracks(pl3, {t3, t4});
  const auto sess = lib.begin_history_session();
  lib.add_history(sess, t1, "A", 1);
  lib.add_history(sess, t2, "A", 2);
  lib.add_history(sess, t3, "B", 3);
  const auto size_before = fs::file_size(p2);

  MergeOptions dry;
  dry.dry_run = true;
  MergeResult d = lib.merge_duplicates(t1, {t2, t3}, dry);
  CHECK(d.ok && d.playlists_repointed == 2 && d.playlists_already_had == 1 && d.hot_cues_copied == 1);
  CHECK(lib.track(t2) && lib.track(t3) && lib.playlist_track_ids(pl2).size() == 2 && lib.cues(t1).size() == 1);  // nothing changed

  MergeResult r = lib.merge_duplicates(t1, {t2, t3});
  CHECK(r.ok && r.batch_id > 0 && r.history_moved == 2 && r.grid_copied && r.memory_cues_copied == 1);
  CHECK(!lib.track(t2) && !lib.track(t3) && lib.track(t1));
  CHECK(lib.playlist_track_ids(pl) == (std::vector<std::int64_t>{t4, t1}));
  CHECK(lib.playlist_track_ids(pl2) == (std::vector<std::int64_t>{t1}));
  CHECK(lib.playlist_track_ids(pl3) == (std::vector<std::int64_t>{t1, t4}));
  CHECK(lib.track(t1)->play_count == 3 && lib.track(t1)->rating == 4 && lib.track(t1)->genre == "House");
  const auto cues = lib.cues(t1);
  CHECK(near(find_slot(cues, 0)->start_ms, 500) && find_slot(cues, 3) && lib.beatgrid(t1).size() == 1);  // kept slot 0 wins
  CHECK(lib.deleted_tracks().size() == 2);
  CHECK(fs::exists(p2) && fs::exists(p3) && fs::file_size(p2) == size_before);  // files untouched

  CHECK(lib.undo_batch(r.batch_id).status == UndoStatus::Ok);  // tags, cues and grid on the kept track
  CHECK(lib.track(t1)->genre.empty() && lib.beatgrid(t1).empty() && lib.cues(t1).size() == 1);
  CHECK(!lib.merge_duplicates(t1, {t1}).ok && !lib.merge_duplicates(t1, {9999}).ok);

  std::int64_t back = 0;
  CHECK(lib.restore_track(t2, &back) && back == t2 && lib.track(t2));
  CHECK(lib.restore_track(t3) && lib.deleted_tracks().empty());
  CHECK(lib.playlist_track_ids(pl2).size() == 2);  // the restored track is back where it was
  fs::remove_all(dir);
}

int main() {
  test_follow_always_and_undo();
  test_follow_ask_never_and_tempo();
  test_halve_double_leaves_positions();
  test_strip();
  test_duplicates_and_merge();
  std::puts("test_library_tools ok");
  return 0;
}
