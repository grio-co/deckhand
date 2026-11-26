#pragma once
// The play queue: what automix (or the DJ) plays next. An ordered list of upcoming entries (a track id plus optional
// per-entry automix overrides), the entry playing now, the one whose transition is scheduled but has not started yet
// (`pending`) and the history of what was played. Every change is written to the library, so the queue survives a
// restart. Thread-safe; the optional change callback runs outside the queue's lock.
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"

namespace deckhand {

struct QueueEntry {
  std::int64_t id = 0;        // unique for the life of the library's queue; what the API addresses
  std::int64_t track_id = 0;
  bool play_next = false;     // part of the leading "play next" block (later play-nexts go behind it)
  AutomixPoints overrides;    // per-entry overrides of the track's automix points
};

struct PlayedEntry {
  std::int64_t entry_id = 0, track_id = 0, played_at_ms = 0;
};

// Duplicates: allow everything, or refuse a track that played within the last `window` tracks (the one playing now
// counts) or is already waiting in the queue.
struct QueuePolicy {
  bool skip_recent = false;
  int window = 10;
};

struct QueueAdd {
  std::vector<std::int64_t> entry_ids;  // entries created, in order
  int skipped = 0;                      // refused by the duplicates policy
};

class PlayQueue {
 public:
  // `lib` (may be null: memory only) is used under the queue's own lock; give it a connection of its own.
  explicit PlayQueue(Library* lib = nullptr);

  // ---- reading ----
  std::vector<QueueEntry> upcoming() const;
  std::optional<QueueEntry> peek() const;      // the next entry to be played
  std::optional<QueueEntry> current() const;   // playing now
  std::optional<QueueEntry> pending() const;   // transition scheduled
  std::size_t size() const;                    // upcoming entries
  std::vector<PlayedEntry> history(int limit = 50) const;  // newest first
  std::uint64_t revision() const;              // changes with every edit
  QueuePolicy policy() const;
  void set_policy(const QueuePolicy& p);       // stored in the library settings (queue.dup_policy, queue.dup_window)
  void set_on_change(std::function<void()> cb);
  void set_seed(std::uint32_t seed);           // shuffle (tests)

  // ---- editing (any thread) ----
  QueueAdd add_end(const std::vector<std::int64_t>& tracks, bool force = false);
  QueueAdd play_next(const std::vector<std::int64_t>& tracks, bool force = false);  // behind earlier play-nexts, before the rest
  QueueAdd insert(const std::vector<std::int64_t>& tracks, int position, bool force = false);
  bool move(std::int64_t entry_id, int position);   // new index among the upcoming entries (clamped)
  bool remove(std::int64_t entry_id);
  std::optional<QueueEntry> skip();                 // drops the next entry without playing it
  void clear();                                     // upcoming entries only
  void shuffle();                                   // upcoming entries behind the play-next block
  // Replaces (replace = true) or extends the queue with `tracks`, optionally shuffled.
  QueueAdd fill(const std::vector<std::int64_t>& tracks, bool shuffle_them, bool replace, bool force = false);
  bool set_overrides(std::int64_t entry_id, const AutomixPoints& overrides);
  // Puts the previous track at the front (and the one playing now behind it). False when there is no history.
  bool back();

  // ---- transport (the automix engine) ----
  std::optional<QueueEntry> begin_pending();        // next entry becomes pending (kept in the library until it plays)
  void abort_pending();                             // pending goes back to the front
  std::optional<QueueEntry> commit_pending(std::int64_t now_ms);  // pending starts playing: now current, in the history
  std::optional<QueueEntry> adopt_current(std::int64_t track_id, std::int64_t now_ms);  // something already playing
  void clear_current();                             // nothing plays any more

 private:
  void load();
  void persist_locked();
  void normalise_locked();
  bool refused_locked(std::int64_t track_id, bool force) const;
  QueueAdd insert_locked(const std::vector<std::int64_t>& tracks, std::size_t at, bool flag, bool force);
  std::vector<std::int64_t> recent_ids_locked() const;
  void changed_locked() { ++revision_; dirty_ = true; }
  void notify(std::unique_lock<std::mutex>& lock);

  mutable std::mutex m_;
  Library* lib_;
  std::vector<QueueEntry> up_;
  std::optional<QueueEntry> current_, pending_;
  std::vector<PlayedEntry> hist_;  // oldest first, capped
  QueuePolicy policy_;
  std::int64_t next_id_ = 1;
  std::uint64_t revision_ = 1;
  bool dirty_ = false;
  std::function<void()> on_change_;
  std::mt19937 rng_;
};

}  // namespace deckhand
