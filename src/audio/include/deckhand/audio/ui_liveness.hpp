#pragma once
// Is the UI that is attached to the engine process alive? The UI writes a heartbeat about every 10 ms from its control
// loop; under heavy load that loop can stall for seconds while the audio is fine. So a quiet heartbeat alone is not
// "lost": the UI must have been silent for `lost_after_ns` (many missed beats), or its process must be gone. The engine
// only uses the verdict to stop asking a UI that is not there (browse requests are dropped instead of queued) and to log.
// Log lines are rate limited: the first change is logged, then a counted summary at most once per `summary_every_ns`.
// Pure logic with an injected clock: tests drive it without waiting.
#include <cstdint>
#include <functional>

namespace deckhand {

struct UiLivenessConfig {
  std::uint64_t lost_after_ns = 10ull * 1000000000ull;      // silent this long: lost (1000 missed beats)
  std::uint64_t check_process_after_ns = 1ull * 1000000000ull;  // silent this long: ask whether the process still exists
  std::uint64_t summary_every_ns = 60ull * 1000000000ull;
};

struct UiLivenessReport {
  enum Kind { None, Attached, Lost, Summary } kind = None;
  std::uint32_t pid = 0;
  int lost = 0, attached = 0;  // Summary: how many times in the period (not logged one by one)
};

class UiLiveness {
 public:
  explicit UiLiveness(UiLivenessConfig c = {}) : cfg_(c) {}
  bool alive() const { return alive_; }

  // pid = the attached UI (0 = none), beat_ns = its last heartbeat, now_ns = the same clock. `process_exists` is asked only
  // when the heartbeat has been quiet for a while. Returns what to log, if anything.
  UiLivenessReport update(std::uint32_t pid, std::uint64_t beat_ns, std::uint64_t now_ns,
                          const std::function<bool(std::uint32_t)>& process_exists) {
    bool now_alive = false;
    if (pid != 0) {
      last_pid_ = pid;
      const std::uint64_t quiet = now_ns > beat_ns ? now_ns - beat_ns : 0;
      if (quiet < cfg_.check_process_after_ns) now_alive = true;
      else if (process_exists && !process_exists(pid)) now_alive = false;  // truly gone: no waiting
      else now_alive = quiet < cfg_.lost_after_ns;
    }
    UiLivenessReport r;
    r.pid = pid ? pid : last_pid_;
    if (now_alive != alive_) {
      alive_ = now_alive;
      const UiLivenessReport::Kind kind = now_alive ? UiLivenessReport::Attached : UiLivenessReport::Lost;
      if (now_alive && !seen_alive_) {  // the UI attaching for the first time is news, not a flap
        seen_alive_ = true;
        r.kind = kind;
      } else if (period_start_ == 0) {  // first occurrence of a period: logged, and so is the matching reverse change
        period_start_ = now_ns ? now_ns : 1;
        lost_ = attached_ = 0;
        pair_open_ = !now_alive;
        r.kind = kind;
      } else if (now_alive && pair_open_) {
        pair_open_ = false;
        r.kind = kind;
      } else {
        (now_alive ? attached_ : lost_) += 1;
      }
    }
    if (r.kind == UiLivenessReport::None && period_start_ != 0 && now_ns >= period_start_ && now_ns - period_start_ >= cfg_.summary_every_ns) {
      if (lost_ + attached_ > 0) {
        r.kind = UiLivenessReport::Summary;
        r.lost = lost_;
        r.attached = attached_;
        lost_ = attached_ = 0;
        period_start_ = now_ns ? now_ns : 1;
      } else {
        period_start_ = 0;  // a quiet period: the next change is a first occurrence again
      }
    }
    return r;
  }

 private:
  UiLivenessConfig cfg_;
  bool alive_ = false, pair_open_ = false, seen_alive_ = false;
  int lost_ = 0, attached_ = 0;
  std::uint64_t period_start_ = 0;
  std::uint32_t last_pid_ = 0;
};

}  // namespace deckhand
