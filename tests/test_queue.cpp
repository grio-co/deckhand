// Play queue: ordering (play next), editing, duplicates policy, history, back, persistence across a restart.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/app/play_queue.hpp"

using namespace deckhand;

namespace {

std::string fresh(const char* name) {
  const std::string path = (std::filesystem::temp_directory_path() / name).string();
  for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
  return path;
}

std::vector<std::int64_t> tracks(const PlayQueue& q) {
  std::vector<std::int64_t> out;
  for (const QueueEntry& e : q.upcoming()) out.push_back(e.track_id);
  return out;
}
using V = std::vector<std::int64_t>;

void test_ordering() {
  PlayQueue q;
  q.add_end({1, 2, 3});
  CHECK(tracks(q) == (V{1, 2, 3}));
  q.play_next({10});
  CHECK(tracks(q) == (V{10, 1, 2, 3}));
  q.play_next({11, 12});  // behind the earlier play-next, in the order given
  CHECK(tracks(q) == (V{10, 11, 12, 1, 2, 3}));
  q.insert({20}, 4);
  CHECK(tracks(q) == (V{10, 11, 12, 1, 20, 2, 3}));
  q.insert({21}, 0);  // an insert at the front is not a play-next: a later play-next goes before it
  q.play_next({13});
  CHECK(tracks(q) == (V{13, 21, 10, 11, 12, 1, 20, 2, 3}));
  // move and remove
  const auto up = q.upcoming();
  CHECK(q.move(up[8].id, 0));
  CHECK(tracks(q) == (V{3, 13, 21, 10, 11, 12, 1, 20, 2}));
  CHECK(q.remove(up[0].id));
  CHECK(!q.remove(up[0].id));
  CHECK(tracks(q) == (V{3, 21, 10, 11, 12, 1, 20, 2}));
  // skip drops the next entry
  auto skipped = q.skip();
  CHECK(skipped && skipped->track_id == 3 && q.size() == 7);
  // shuffle keeps the play-next block in front and the same set of tracks
  PlayQueue s;
  s.set_seed(7);
  s.add_end({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
  s.play_next({100, 101});
  s.shuffle();
  const V t = tracks(s);
  CHECK(t.size() == 14 && t[0] == 100 && t[1] == 101);
  V sorted(t.begin() + 2, t.end());
  std::sort(sorted.begin(), sorted.end());
  CHECK(sorted == (V{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
  CHECK(V(t.begin() + 2, t.end()) != (V{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
  // fill replaces
  const QueueAdd f = s.fill({5, 6, 7}, false, true);
  CHECK(f.entry_ids.size() == 3 && tracks(s) == (V{5, 6, 7}));
  s.clear();
  CHECK(s.size() == 0);
  std::puts("queue: ordering ok");
}

void test_transport_and_history() {
  PlayQueue q;
  q.add_end({1, 2, 3});
  CHECK(!q.pending() && !q.current());
  auto e = q.begin_pending();
  CHECK(e && e->track_id == 1 && q.size() == 2 && q.pending());
  CHECK(!q.begin_pending());  // one pending at a time
  q.abort_pending();
  CHECK(tracks(q) == (V{1, 2, 3}) && !q.pending());
  q.begin_pending();
  auto c = q.commit_pending(1000);
  CHECK(c && q.current() && q.current()->track_id == 1 && !q.pending());
  q.begin_pending();
  q.commit_pending(2000);
  CHECK(q.current()->track_id == 2 && q.history(10).size() == 2 && q.history(10)[0].track_id == 2);
  // back: the previous track (1) first, then the one playing now (2) again
  CHECK(q.back());
  CHECK(tracks(q) == (V{1, 2, 3}));
  CHECK(q.history(10).size() == 0);
  PlayQueue empty;
  CHECK(!empty.back());
  // the entry playing now can be given overrides (its mix-out still counts)
  AutomixPoints p;
  p.max_play_ms = 90000;
  CHECK(q.set_overrides(q.upcoming()[0].id, p) && q.upcoming()[0].overrides.max_play_ms && *q.upcoming()[0].overrides.max_play_ms == 90000);
  std::puts("queue: transport and history ok");
}

void test_duplicates() {
  PlayQueue q;
  q.begin_pending();
  q.add_end({1, 2, 3, 4});
  q.begin_pending();
  q.commit_pending(1);  // 1 plays now
  QueuePolicy pol;
  pol.skip_recent = true;
  pol.window = 3;
  q.set_policy(pol);
  QueueAdd a = q.add_end({1, 5, 5, 2});  // 1 is playing, 5 twice (the second is already queued), 2 already waits
  CHECK(a.entry_ids.size() == 1 && a.skipped == 3);
  a = q.add_end({1}, true);  // forced
  CHECK(a.entry_ids.size() == 1);
  pol.skip_recent = false;
  q.set_policy(pol);
  CHECK(q.add_end({1, 1}).entry_ids.size() == 2);
  std::puts("queue: duplicates policy ok");
}

void test_persistence() {
  const std::string path = fresh("deckhand_test_queue.db");
  std::int64_t second_id = 0;
  {
    Library lib(path);
    PlayQueue q(&lib);
    q.add_end({1, 2, 3});
    q.play_next({9});
    AutomixPoints p;
    p.max_play_ms = 90000;
    p.mix_out_ms = 120000;
    p.transition_type = "cut";
    second_id = q.upcoming()[1].id;
    CHECK(q.set_overrides(second_id, p));
    q.begin_pending();
    q.commit_pending(500);     // 9 plays
    q.begin_pending();         // 1 is scheduled but has not started when the app stops
    QueuePolicy pol;
    pol.skip_recent = true;
    pol.window = 7;
    q.set_policy(pol);
  }
  {
    Library lib(path);
    PlayQueue q(&lib);
    // the pending entry is first again; the playing one is not restored, it is in the history
    CHECK(tracks(q) == (V{1, 2, 3}));
    CHECK(q.upcoming()[0].id == second_id);
    CHECK(q.upcoming()[0].overrides.max_play_ms && *q.upcoming()[0].overrides.max_play_ms == 90000);
    CHECK(q.upcoming()[0].overrides.transition_type && *q.upcoming()[0].overrides.transition_type == "cut");
    CHECK(q.upcoming()[0].overrides.mix_out_ms && *q.upcoming()[0].overrides.mix_out_ms == 120000);
    CHECK(!q.current() && !q.pending());
    CHECK(q.history(10).size() == 1 && q.history(10)[0].track_id == 9 && q.history(10)[0].played_at_ms == 500);
    CHECK(q.policy().skip_recent && q.policy().window == 7);
    const auto added = q.add_end({4});
    CHECK(added.entry_ids.size() == 1 && added.entry_ids[0] > second_id);  // ids keep counting up
    q.clear();
  }
  {
    Library lib(path);
    PlayQueue q(&lib);
    CHECK(q.size() == 0);
  }
  std::puts("queue: persists across a restart");
}

}  // namespace

int main() {
  test_ordering();
  test_transport_and_history();
  test_duplicates();
  test_persistence();
  std::puts("test_queue ok");
  return 0;
}
