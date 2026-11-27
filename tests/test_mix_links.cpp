// Linked mix points: storage, lookups, grid follow, journal and undo, trash, suggestions, schema 9 -> 10.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "check.hpp"
#include "deckhand/library/backup.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {

struct Dir {
  fs::path p;
  explicit Dir(const char* name) {
    p = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
  }
  ~Dir() {
    std::error_code ec;
    fs::remove_all(p, ec);
  }
  std::string db() const { return (p / "library.db").string(); }
};

std::int64_t add(Library& lib, const std::string& name, double bpm = 120, const char* key = "8A") {
  NewTrack t;
  t.path = "/music/" + name + ".mp3";
  t.title = name;
  t.artist = "A";
  t.bpm = bpm;
  t.key = key;
  t.duration_ms = 240000;
  return lib.upsert_track(t);
}

bool near(double a, double b) { return std::fabs(a - b) < 0.01; }

MixLink link(std::int64_t a, double from, std::int64_t b, double to) {
  MixLink l;
  l.from_track = a;
  l.from_ms = from;
  l.to_track = b;
  l.to_ms = to;
  return l;
}

void test_crud_and_lookups() {
  Dir d("deckhand_test_mix_links_crud");
  {
    Library lib(d.db());
    const auto a = add(lib, "a"), b = add(lib, "b"), c = add(lib, "c");
    MixLink l = link(a, 60000, b, 8000);
    l.transition_beats = 16;
    l.transition_type = "echo_out";
    l.label = "drop\tto drop";
    const auto id = lib.add_mix_link(l);
    CHECK(id > 0);
    const auto got = lib.mix_link(id);
    CHECK(got && got->from_track == a && got->to_track == b && near(got->from_ms, 60000) && near(got->to_ms, 8000));
    CHECK(got->transition_beats && *got->transition_beats == 16 && got->transition_type == "echo_out" && got->label == "drop\tto drop" && got->created_at > 0);
    const auto id2 = lib.add_mix_link(link(a, 30000, c, 0));
    const auto id3 = lib.add_mix_link(link(b, 100000, a, 1000));
    // Same ends again: the same link, updated.
    MixLink again = link(a, 60000.4, b, 8000.2);
    again.label = "x";
    CHECK(lib.add_mix_link(again) == id && lib.mix_link(id)->label == "x");

    const auto out = lib.mix_links_from(a);
    CHECK(out.size() == 2 && out[0].id == id2 && out[1].id == id);  // by position
    CHECK(lib.mix_links_to(a).size() == 1 && lib.mix_links_to(a)[0].id == id3);
    CHECK(lib.mix_links_between(a, b).size() == 1 && lib.mix_links_between(b, a).size() == 1 && lib.mix_links_between(b, c).empty());
    CHECK(lib.mix_link_targets(a).size() == 2);

    // Update keeps the tracks.
    MixLink up = *lib.mix_link(id);
    up.from_ms = 61000;
    up.transition_beats.reset();
    up.transition_type = "cut";
    CHECK(lib.update_mix_link(up));
    const auto u = lib.mix_link(id);
    CHECK(near(u->from_ms, 61000) && !u->transition_beats && u->transition_type == "cut" && u->from_track == a && u->to_track == b);
    MixLink none = up;
    none.id = 9999;
    CHECK(!lib.update_mix_link(none));

    // Which link automix follows: the first one still ahead of the playhead.
    lib.add_mix_link(link(a, 20000, b, 500));
    CHECK(near(lib.mix_link_for_transition(a, b, 0)->from_ms, 20000));
    CHECK(near(lib.mix_link_for_transition(a, b, 30000)->from_ms, 61000));
    CHECK(near(lib.mix_link_for_transition(a, b, 90000)->from_ms, 61000));  // none ahead: the last
    CHECK(!lib.mix_link_for_transition(c, a, 0));

    // Validation.
    bool threw = false;
    try { lib.add_mix_link(link(a, 1, a, 2)); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { lib.add_mix_link(link(a, -1, b, 2)); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { lib.add_mix_link(link(a, 1, 99999, 2)); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    MixLink bad = link(a, 1, b, 2);
    bad.transition_type = "wobble";
    try { lib.add_mix_link(bad); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    CHECK(lib.remove_mix_link(id) && !lib.mix_link(id) && !lib.remove_mix_link(id));
    CHECK(lib.remove_mix_links(a) >= 2 && lib.mix_links_from(a).empty() && lib.mix_links_to(a).empty());
  }
}

void test_journal_undo_and_trash() {
  Dir d("deckhand_test_mix_links_undo");
  {
    Library lib(d.db());
    const auto a = add(lib, "a"), b = add(lib, "b");
    const auto id = lib.add_mix_link(link(a, 5000, b, 1000));
    auto hist = lib.track_history(a);
    CHECK(!hist.empty() && hist[0].field == "mix_links");
    CHECK(lib.undo_edit(hist[0].id).status == UndoStatus::Ok);
    CHECK(lib.mix_links_from(a).empty());
    // Undo of the undo: the link comes back with its id.
    hist = lib.track_history(a);
    CHECK(lib.undo_edit(hist[0].id).status == UndoStatus::Ok);
    CHECK(lib.mix_link(id) && near(lib.mix_link(id)->from_ms, 5000));
    // Edit, then an older entry conflicts.
    MixLink up = *lib.mix_link(id);
    up.to_ms = 2000;
    lib.update_mix_link(up);
    hist = lib.track_history(a);
    CHECK(hist.size() >= 2);
    CHECK(lib.undo_edit(hist[1].id).status == UndoStatus::Conflict);
    CHECK(lib.undo_edit(hist[0].id).status == UndoStatus::Ok && near(lib.mix_link(id)->to_ms, 1000));
    // Removal is journalled too.
    lib.remove_mix_link(id);
    hist = lib.track_history(a);
    CHECK(lib.undo_edit(hist[0].id).status == UndoStatus::Ok && lib.mix_link(id));

    // A track in the trash takes its links out of every listing; restoring brings them back.
    lib.remove_track(b);
    CHECK(lib.mix_links_from(a).empty() && lib.mix_link_targets(a).empty());
    std::int64_t back = 0;
    CHECK(lib.restore_track(b, &back));
    CHECK(lib.mix_links_from(a).size() == 1 && lib.mix_links_from(a)[0].to_track == back);
  }
}

void test_grid_follow() {
  Dir d("deckhand_test_mix_links_follow");
  {
    Library lib(d.db());
    const auto a = add(lib, "a"), b = add(lib, "b");
    lib.replace_beatgrid(a, {{100, 120, 1}});
    lib.replace_beatgrid(b, {{100, 120, 1}});
    const auto id = lib.add_mix_link(link(a, 2100, b, 600));  // beat 4 of a, beat 1 of b

    // Never: nothing moves.
    GridCommit never = lib.commit_grid(a, {{140, 120, 1}}, std::nullopt, std::nullopt, CueFollow::Never);
    CHECK(never.mix_links_moved == 0 && near(lib.mix_link(id)->from_ms, 2100));
    lib.commit_grid(a, {{100, 120, 1}}, std::nullopt, std::nullopt, CueFollow::Never);

    // Always: the from end follows the grid of a, the to end the grid of b.
    GridCommit c = lib.commit_grid(a, {{150, 120, 1}}, std::nullopt, std::nullopt, CueFollow::Always);
    CHECK(c.mix_links_moved == 1 && near(lib.mix_link(id)->from_ms, 2150) && near(lib.mix_link(id)->to_ms, 600));
    GridCommit cb = lib.commit_grid(b, {{250, 120, 1}}, std::nullopt, std::nullopt, CueFollow::Always);
    CHECK(cb.mix_links_moved == 1 && near(lib.mix_link(id)->to_ms, 750) && near(lib.mix_link(id)->from_ms, 2150));
    // One undo of the batch puts the grid and the link end back.
    CHECK(lib.undo_batch(cb.batch_id).status == UndoStatus::Ok);
    CHECK(near(lib.mix_link(id)->to_ms, 600) && near(lib.beatgrid(b)[0].first_beat_ms, 100));

    // Off the grid: stays.
    MixLink off = *lib.mix_link(id);
    off.to_ms = 760;
    lib.update_mix_link(off);
    GridCommit co = lib.commit_grid(b, {{300, 120, 1}}, std::nullopt, std::nullopt, CueFollow::Always);
    CHECK(co.mix_links_moved == 0 && near(lib.mix_link(id)->to_ms, 760));
    lib.update_mix_link([&] { MixLink m = *lib.mix_link(id); m.to_ms = 800; return m; }());  // on the new grid of b

    // Ask: offered, then applied once.
    GridCommit ask = lib.commit_grid(b, {{350, 120, 1}}, std::nullopt, std::nullopt, CueFollow::Ask);
    CHECK(ask.mix_links_moved == 1 && !ask.applied && near(lib.mix_link(id)->to_ms, 800));
    FollowResult f = lib.apply_cue_follow(b, ask.batch_id);
    CHECK(f.status == FollowStatus::Ok && f.mix_links_moved == 1 && near(lib.mix_link(id)->to_ms, 850));
    CHECK(lib.apply_cue_follow(b, ask.batch_id).status == FollowStatus::AlreadyApplied && near(lib.mix_link(id)->to_ms, 850));
    CHECK(lib.undo_batch(ask.batch_id).status == UndoStatus::Ok && near(lib.mix_link(id)->to_ms, 800));
  }
}

void test_suggestions() {
  Dir d("deckhand_test_mix_links_suggest");
  {
    Library lib(d.db());
    const auto seed = add(lib, "seed", 120, "8A");
    const auto plain = add(lib, "plain", 120, "8A");
    const auto tied = add(lib, "tied", 120, "8A");
    const auto far = add(lib, "far", 90, "3B");  // out of tempo range and not harmonic
    NextTrackOptions o;
    auto top = lib.suggest(seed, o);
    CHECK(top.size() == 2);  // `far` is filtered
    lib.add_mix_link(link(seed, 100000, tied, 4000));
    lib.add_mix_link(link(seed, 120000, far, 0));
    top = lib.suggest(seed, o);
    CHECK(top.size() == 3);
    // Tied ranks above the otherwise identical plain one, with the reason.
    std::size_t pos_tied = 99, pos_plain = 99;
    for (std::size_t i = 0; i < top.size(); ++i) {
      if (top[i].track_id == tied) pos_tied = i;
      if (top[i].track_id == plain) pos_plain = i;
    }
    CHECK(pos_tied < pos_plain);
    const NextTrackFactor* lf = nullptr;
    for (const auto& f : top[pos_tied].factors)
      if (f.id == "link") lf = &f;
    CHECK(lf && lf->tag == "mix_link" && lf->points > 0 && lf->detail == 1);
    bool far_found = false;
    for (const auto& s : top)
      if (s.track_id == far) {
        far_found = true;
        bool has = false;
        for (const auto& f : s.factors) has = has || f.id == "link";
        CHECK(has);
      }
    CHECK(far_found);
    // Weight zero: no factor, and the filters apply again.
    o.weights.linked = 0;
    top = lib.suggest(seed, o);
    CHECK(top.size() == 3);  // still a candidate (the link makes it one)
    for (const auto& s : top)
      for (const auto& f : s.factors) CHECK(f.id != "link");
    // Excluded stays out; links the other way do not count.
    o.exclude.insert(tied);
    CHECK(lib.suggest(seed, o).size() == 2);
    CHECK(lib.suggest(tied, NextTrackOptions{}).size() >= 1);
    for (const auto& s : lib.suggest(tied, NextTrackOptions{}))
      for (const auto& f : s.factors) CHECK(f.id != "link");
    CHECK(lib.suggest_weights().linked == 40);
  }
}

void test_migration() {
  Dir d("deckhand_test_mix_links_migrate");
  {
    Library lib(d.db());
    const auto a = add(lib, "a");
    lib.db().exec("DROP TABLE mix_links; DROP TABLE track_vocals; DROP TABLE vocal_regions; UPDATE schema_version SET version = 9;");
    (void)a;
  }
  {
    Library lib(d.db());  // migrates 9 -> 10
    Stmt s(lib.db(), "SELECT max(version) FROM schema_version");
    CHECK(s.step() && s.i64(0) == 10 && kLibrarySchemaVersion == 10);
    const auto ids = lib.track_ids(0, "");
    CHECK(ids.size() == 1);
    const auto b = add(lib, "b");
    CHECK(lib.add_mix_link(link(ids[0], 1000, b, 0)) > 0);
    CHECK(!lib.vocals(b));
    const auto copies = library_backups(d.db());
    CHECK(!copies.empty() && copies[0].schema_version == 9);
  }
}

}  // namespace

void test_merge_repoints() {
  Dir d("deckhand_test_mix_links_merge");
  {
    Library lib(d.db());
    const auto keep = add(lib, "keep"), dup = add(lib, "dup"), x = add(lib, "x"), y = add(lib, "y");
    lib.add_mix_link(link(dup, 60000, x, 8000));   // out of the duplicate
    lib.add_mix_link(link(y, 90000, dup, 16000));  // into the duplicate
    lib.add_mix_link(link(keep, 60000, x, 8000));  // the kept track already has this one: the copy goes
    lib.add_mix_link(link(keep, 100000, dup, 500)); // between the two: would point at itself
    const MergeResult r = lib.merge_duplicates(keep, {dup});
    CHECK(r.ok && r.mix_links_moved >= 3);
    CHECK(lib.mix_links_from(dup).empty() && lib.mix_links_to(dup).empty());
    const auto out = lib.mix_links_from(keep);
    CHECK(out.size() == 1 && out[0].to_track == x && near(out[0].from_ms, 60000));  // the duplicate-of-a-link is gone
    const auto in = lib.mix_links_to(keep);
    CHECK(in.size() == 1 && in[0].from_track == y && near(in[0].to_ms, 16000));
    CHECK(lib.mix_links_between(keep, keep).empty());
  }
}

int main() {
  test_crud_and_lookups();
  test_journal_undo_and_trash();
  test_grid_follow();
  test_suggestions();
  test_migration();
  test_merge_repoints();
  std::puts("test_mix_links ok");
  return 0;
}
