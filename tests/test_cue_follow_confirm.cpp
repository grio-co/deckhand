// The question after a grid edit: built from the commit, "move" applies the follow inside the edit's journal batch, "leave
// them" keeps the cues, always / never need no question.
#include <cstdio>
#include <filesystem>

#include "check.hpp"
#include "cue_follow_confirm.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

int main() {
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_cue_follow_confirm";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  i18n::Catalog cat;
  CHECK(cat.load_builtin("resources/lang/en.dhlang"));
  {
    Library lib((dir / "lib.db").string());
    NewTrack n;
    n.path = (dir / "a.mp3").string();
    n.title = "A";
    n.duration_ms = 200000;
    const auto id = lib.upsert_track(n);
    lib.replace_beatgrid(id, {{0.0, 120.0, 1}});
    Cue c;
    c.kind = CueKind::Hot;
    c.slot = 0;
    c.start_ms = 4000;  // beat 8 of the old grid
    lib.replace_all_cues(id, {c});

    const std::vector<BeatSection> moved{{100.0, 120.0, 1}};  // the grid shifts by 100 ms
    const GridCommit commit = lib.commit_grid(id, moved, std::nullopt, std::nullopt, CueFollow::Ask);
    CHECK(commit.batch_id != 0 && commit.proposal.moves.size() == 1 && !commit.applied);
    CHECK(lib.cues(id)[0].start_ms == 4000);  // only proposed
    CHECK(cue_follow_needs_question(CueFollow::Ask, commit));
    CHECK(!cue_follow_needs_question(CueFollow::Never, commit) && !cue_follow_needs_question(CueFollow::Always, commit));

    std::int64_t got_track = 0, got_batch = 0;
    ConfirmRequest q = cue_follow_request(cat, id, commit, [&](std::int64_t t, std::int64_t b) { got_track = t; got_batch = b; });
    CHECK(q.message.find("1") != std::string::npos && !q.title.empty() && q.ok_label == "Move the cues" && q.cancel_label == "Leave them");
    CHECK(!q.details.empty());

    ConfirmState s;
    s.show(q);
    s.cancel();  // leave them
    CHECK(got_track == 0 && lib.cues(id)[0].start_ms == 4000);
    s.show(std::move(q));
    s.accept();  // move them: the app posts a job that does exactly this
    CHECK(got_track == id && got_batch == commit.batch_id);
    const FollowResult f = lib.apply_cue_follow(got_track, got_batch);
    CHECK(f.status == FollowStatus::Ok && std::fabs(lib.cues(id)[0].start_ms - 4100) < 1.0);
    CHECK(lib.apply_cue_follow(got_track, got_batch).status == FollowStatus::AlreadyApplied);
    // one undo takes the grid and the cue back together
    CHECK(lib.undo_batch(got_batch).status == UndoStatus::Ok && std::fabs(lib.cues(id)[0].start_ms - 4000) < 1.0);
  }
  fs::remove_all(dir, ec);
  std::puts("test_cue_follow_confirm: ok");
  return 0;
}
