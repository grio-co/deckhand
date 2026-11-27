// The engine process's verdict on the UI's heartbeat: a stalled UI is not a lost one, a gone process is lost at once,
// and the log lines are rate limited (first occurrence, then a counted summary). An injected clock: nothing waits.
#include <cstdio>

#include "check.hpp"
#include "deckhand/audio/ui_liveness.hpp"

using namespace deckhand;

namespace {
constexpr std::uint64_t kS = 1000000000ull;
const auto exists = [](std::uint32_t) { return true; };
const auto gone = [](std::uint32_t) { return false; };

void test_stall_is_not_loss() {
  UiLiveness l;
  CHECK(!l.alive());
  UiLivenessReport r = l.update(42, 100 * kS, 100 * kS, exists);
  CHECK(l.alive() && r.kind == UiLivenessReport::Attached && r.pid == 42);
  // quiet for 3 s (the old threshold), 5 s, 9 s: still alive, nothing logged
  for (const std::uint64_t quiet : {3ull, 5ull, 9ull}) {
    r = l.update(42, 100 * kS, (100 + quiet) * kS, exists);
    CHECK(l.alive() && r.kind == UiLivenessReport::None);
  }
  // past the limit: lost
  r = l.update(42, 100 * kS, 111 * kS, exists);
  CHECK(!l.alive() && r.kind == UiLivenessReport::Lost);
  // a beat arrives again
  r = l.update(42, 112 * kS, 112 * kS, exists);
  CHECK(l.alive() && r.kind == UiLivenessReport::Attached);
}

void test_dead_process_is_lost_at_once() {
  UiLiveness l;
  l.update(7, 10 * kS, 10 * kS, exists);
  CHECK(l.alive());
  UiLivenessReport r = l.update(7, 10 * kS, 10 * kS + kS / 2, gone);  // quiet for half a second: not asked yet
  CHECK(l.alive() && r.kind == UiLivenessReport::None);
  r = l.update(7, 10 * kS, 12 * kS, gone);  // quiet for 2 s and the process does not exist
  CHECK(!l.alive() && r.kind == UiLivenessReport::Lost);
  // a process that exists but is silent: kept alive up to the limit
  UiLiveness m;
  m.update(7, 10 * kS, 10 * kS, exists);
  m.update(7, 10 * kS, 15 * kS, exists);
  CHECK(m.alive());
  // detached cleanly
  r = l.update(0, 0, 13 * kS, exists);
  CHECK(!l.alive());
}

void test_log_is_rate_limited() {
  UiLiveness l;
  std::uint64_t t = 1000 * kS;
  int lines = 0, summaries = 0, lost_total = 0;
  UiLivenessReport r = l.update(9, t, t, exists);
  CHECK(r.kind == UiLivenessReport::Attached);
  ++lines;
  // 20 stalls of 15 s with a beat in between, over about 10 minutes of fake time
  for (int i = 0; i < 20; ++i) {
    std::uint64_t beat = t;
    t += 15 * kS;
    r = l.update(9, beat, t, exists);  // lost
    if (r.kind != UiLivenessReport::None) ++lines;
    if (r.kind == UiLivenessReport::Summary) ++summaries, lost_total += r.lost;
    beat = t;
    t += 15 * kS;
    r = l.update(9, beat, t - 14 * kS, exists);  // a beat is fresh again
    if (r.kind != UiLivenessReport::None) ++lines;
    if (r.kind == UiLivenessReport::Summary) ++summaries, lost_total += r.lost;
  }
  // 40 changes happened; far fewer lines were written, and the summaries account for the rest
  CHECK(lines < 15);
  CHECK(summaries >= 1);
  CHECK(lost_total >= 1);
}

void test_first_occurrence_pair() {
  UiLiveness l;
  l.update(5, 10 * kS, 10 * kS, exists);
  UiLivenessReport r = l.update(5, 10 * kS, 30 * kS, exists);
  CHECK(r.kind == UiLivenessReport::Lost);
  r = l.update(5, 31 * kS, 31 * kS, exists);
  CHECK(r.kind == UiLivenessReport::Attached);  // the first pair is logged completely
  r = l.update(5, 31 * kS, 50 * kS, exists);
  CHECK(r.kind == UiLivenessReport::None);       // the next ones are counted
  r = l.update(5, 51 * kS, 51 * kS, exists);
  CHECK(r.kind == UiLivenessReport::None);
  r = l.update(5, 51 * kS, 100 * kS, exists);  // a minute after the first line: the summary (and the loss it saw)
  CHECK(r.kind == UiLivenessReport::Summary || r.kind == UiLivenessReport::None);
  r = l.update(5, 100 * kS, 100 * kS, exists);
  CHECK(l.alive());
}
}  // namespace

int main() {
  test_stall_is_not_loss();
  test_dead_process_is_lost_at_once();
  test_log_is_rate_limited();
  test_first_occurrence_pair();
  std::puts("ui liveness ok");
  return 0;
}
