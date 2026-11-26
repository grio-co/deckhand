#include "deckhand/app/play_queue.hpp"

#include <algorithm>
#include <set>

#include "deckhand/log/log.hpp"

namespace deckhand {

namespace {
constexpr std::size_t kHistoryCached = 1000;
}

PlayQueue::PlayQueue(Library* lib) : lib_(lib), rng_(std::random_device{}()) { load(); }

void PlayQueue::load() {
  std::lock_guard<std::mutex> lock(m_);
  if (!lib_) return;
  try {
    policy_.skip_recent = lib_->setting("queue.dup_policy", "allow") == "skip_recent";
    policy_.window = std::clamp(std::atoi(lib_->setting("queue.dup_window", "10").c_str()), 1, 1000);
    std::vector<QueueEntry> pend;
    for (const QueueRow& r : lib_->play_queue()) {
      QueueEntry e;
      e.id = r.entry_id;
      e.track_id = r.track_id;
      e.play_next = r.play_next;
      e.overrides = r.overrides;
      if (r.pending) {  // the app stopped between scheduling a transition and the track starting: it is next again
        e.play_next = true;
        pend.push_back(std::move(e));
      } else {
        up_.push_back(std::move(e));
      }
    }
    up_.insert(up_.begin(), pend.begin(), pend.end());
    const auto rows = lib_->queue_history(static_cast<int>(kHistoryCached));
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) hist_.push_back({it->entry_id, it->track_id, it->played_at_ms});
    next_id_ = lib_->max_queue_entry_id() + 1;
    normalise_locked();
  } catch (const std::exception& e) {
    DH_LOG(Warn, "play queue: could not read the saved queue: %s", e.what());
  }
}

void PlayQueue::persist_locked() {
  if (!lib_) return;
  try {
    std::vector<QueueRow> rows;
    const auto row = [](const QueueEntry& e, bool pending) {
      QueueRow r;
      r.entry_id = e.id;
      r.track_id = e.track_id;
      r.play_next = e.play_next;
      r.pending = pending;
      r.overrides = e.overrides;
      return r;
    };
    if (pending_) rows.push_back(row(*pending_, true));
    for (const QueueEntry& e : up_) rows.push_back(row(e, false));
    lib_->save_play_queue(rows);
  } catch (const std::exception& e) {
    DH_LOG(Warn, "play queue: could not save: %s", e.what());
  }
}

void PlayQueue::normalise_locked() {
  bool leading = true;
  for (QueueEntry& e : up_) {
    if (!e.play_next) leading = false;
    if (!leading) e.play_next = false;
  }
}

void PlayQueue::notify(std::unique_lock<std::mutex>& lock) {
  std::function<void()> cb;
  if (dirty_) {
    dirty_ = false;
    cb = on_change_;
  }
  lock.unlock();
  if (cb) cb();
}

std::vector<std::int64_t> PlayQueue::recent_ids_locked() const {
  std::vector<std::int64_t> ids;
  int budget = policy_.window;
  if (current_) {
    ids.push_back(current_->track_id);
    --budget;
  }
  if (pending_) ids.push_back(pending_->track_id);
  // the history's newest entry is the track playing now: do not count it twice
  std::size_t skip = current_ && !hist_.empty() && hist_.back().entry_id == current_->id ? 1 : 0;
  for (auto it = hist_.rbegin(); it != hist_.rend() && budget > 0; ++it) {
    if (skip) {
      --skip;
      continue;
    }
    ids.push_back(it->track_id);
    --budget;
  }
  for (const QueueEntry& e : up_) ids.push_back(e.track_id);
  return ids;
}

bool PlayQueue::refused_locked(std::int64_t track_id, bool force) const {
  if (force || !policy_.skip_recent) return false;
  const auto ids = recent_ids_locked();
  return std::find(ids.begin(), ids.end(), track_id) != ids.end();
}

QueueAdd PlayQueue::insert_locked(const std::vector<std::int64_t>& tracks, std::size_t at, bool flag, bool force) {
  QueueAdd out;
  at = std::min(at, up_.size());
  for (const std::int64_t t : tracks) {
    if (t <= 0 || refused_locked(t, force)) {
      ++out.skipped;
      continue;
    }
    QueueEntry e;
    e.id = next_id_++;
    e.track_id = t;
    e.play_next = flag;
    up_.insert(up_.begin() + static_cast<std::ptrdiff_t>(at), std::move(e));
    out.entry_ids.push_back(up_[at].id);
    ++at;
  }
  normalise_locked();
  return out;
}

std::vector<QueueEntry> PlayQueue::upcoming() const {
  std::lock_guard<std::mutex> lock(m_);
  return up_;
}
std::optional<QueueEntry> PlayQueue::peek() const {
  std::lock_guard<std::mutex> lock(m_);
  if (up_.empty()) return std::nullopt;
  return up_.front();
}
std::optional<QueueEntry> PlayQueue::current() const {
  std::lock_guard<std::mutex> lock(m_);
  return current_;
}
std::optional<QueueEntry> PlayQueue::pending() const {
  std::lock_guard<std::mutex> lock(m_);
  return pending_;
}
std::size_t PlayQueue::size() const {
  std::lock_guard<std::mutex> lock(m_);
  return up_.size();
}
std::vector<PlayedEntry> PlayQueue::history(int limit) const {
  std::lock_guard<std::mutex> lock(m_);
  std::vector<PlayedEntry> out;
  for (auto it = hist_.rbegin(); it != hist_.rend() && static_cast<int>(out.size()) < limit; ++it) out.push_back(*it);
  return out;
}
std::uint64_t PlayQueue::revision() const {
  std::lock_guard<std::mutex> lock(m_);
  return revision_;
}
QueuePolicy PlayQueue::policy() const {
  std::lock_guard<std::mutex> lock(m_);
  return policy_;
}
void PlayQueue::set_policy(const QueuePolicy& p) {
  std::unique_lock<std::mutex> lock(m_);
  policy_.skip_recent = p.skip_recent;
  policy_.window = std::clamp(p.window, 1, 1000);
  if (lib_) {
    try {
      lib_->set_setting("queue.dup_policy", policy_.skip_recent ? "skip_recent" : "allow");
      lib_->set_setting("queue.dup_window", std::to_string(policy_.window));
    } catch (const std::exception& e) {
      DH_LOG(Warn, "play queue: could not save the duplicates policy: %s", e.what());
    }
  }
  changed_locked();
  notify(lock);
}
void PlayQueue::set_on_change(std::function<void()> cb) {
  std::lock_guard<std::mutex> lock(m_);
  on_change_ = std::move(cb);
}
void PlayQueue::set_seed(std::uint32_t seed) {
  std::lock_guard<std::mutex> lock(m_);
  rng_.seed(seed);
}

QueueAdd PlayQueue::add_end(const std::vector<std::int64_t>& tracks, bool force) {
  std::unique_lock<std::mutex> lock(m_);
  QueueAdd r = insert_locked(tracks, up_.size(), false, force);
  persist_locked();
  changed_locked();
  notify(lock);
  return r;
}

QueueAdd PlayQueue::play_next(const std::vector<std::int64_t>& tracks, bool force) {
  std::unique_lock<std::mutex> lock(m_);
  std::size_t at = 0;
  while (at < up_.size() && up_[at].play_next) ++at;
  QueueAdd r = insert_locked(tracks, at, true, force);
  persist_locked();
  changed_locked();
  notify(lock);
  return r;
}

QueueAdd PlayQueue::insert(const std::vector<std::int64_t>& tracks, int position, bool force) {
  std::unique_lock<std::mutex> lock(m_);
  const std::size_t at = position < 0 ? up_.size() : static_cast<std::size_t>(position);
  QueueAdd r = insert_locked(tracks, at, false, force);
  persist_locked();
  changed_locked();
  notify(lock);
  return r;
}

bool PlayQueue::move(std::int64_t entry_id, int position) {
  std::unique_lock<std::mutex> lock(m_);
  const auto it = std::find_if(up_.begin(), up_.end(), [&](const QueueEntry& e) { return e.id == entry_id; });
  if (it == up_.end()) return false;
  QueueEntry e = *it;
  up_.erase(it);
  const std::size_t at = position < 0 ? 0 : std::min<std::size_t>(static_cast<std::size_t>(position), up_.size());
  up_.insert(up_.begin() + static_cast<std::ptrdiff_t>(at), std::move(e));
  up_[at].play_next = false;
  normalise_locked();
  persist_locked();
  changed_locked();
  notify(lock);
  return true;
}

bool PlayQueue::remove(std::int64_t entry_id) {
  std::unique_lock<std::mutex> lock(m_);
  const auto it = std::find_if(up_.begin(), up_.end(), [&](const QueueEntry& e) { return e.id == entry_id; });
  if (it == up_.end()) return false;
  up_.erase(it);
  normalise_locked();
  persist_locked();
  changed_locked();
  notify(lock);
  return true;
}

std::optional<QueueEntry> PlayQueue::skip() {
  std::unique_lock<std::mutex> lock(m_);
  if (up_.empty()) return std::nullopt;
  QueueEntry e = up_.front();
  up_.erase(up_.begin());
  persist_locked();
  changed_locked();
  notify(lock);
  return e;
}

void PlayQueue::clear() {
  std::unique_lock<std::mutex> lock(m_);
  up_.clear();
  persist_locked();
  changed_locked();
  notify(lock);
}

void PlayQueue::shuffle() {
  std::unique_lock<std::mutex> lock(m_);
  std::size_t at = 0;
  while (at < up_.size() && up_[at].play_next) ++at;
  std::shuffle(up_.begin() + static_cast<std::ptrdiff_t>(at), up_.end(), rng_);
  persist_locked();
  changed_locked();
  notify(lock);
}

QueueAdd PlayQueue::fill(const std::vector<std::int64_t>& tracks, bool shuffle_them, bool replace, bool force) {
  std::unique_lock<std::mutex> lock(m_);
  if (replace) up_.clear();
  std::vector<std::int64_t> list = tracks;
  if (shuffle_them) std::shuffle(list.begin(), list.end(), rng_);
  QueueAdd r = insert_locked(list, up_.size(), false, force);
  persist_locked();
  changed_locked();
  notify(lock);
  return r;
}

bool PlayQueue::set_overrides(std::int64_t entry_id, const AutomixPoints& overrides) {
  std::unique_lock<std::mutex> lock(m_);
  const auto it = std::find_if(up_.begin(), up_.end(), [&](const QueueEntry& e) { return e.id == entry_id; });
  if (it == up_.end()) {
    if (current_ && current_->id == entry_id) {  // the playing entry: its mix-out and length still apply
      current_->overrides = overrides;
      changed_locked();
      notify(lock);
      return true;
    }
    return false;
  }
  it->overrides = overrides;
  persist_locked();
  changed_locked();
  notify(lock);
  return true;
}

bool PlayQueue::back() {
  std::unique_lock<std::mutex> lock(m_);
  const bool playing_now = current_ && !hist_.empty() && hist_.back().entry_id == current_->id;
  const std::size_t need = playing_now ? 2 : 1;
  if (hist_.size() < need) return false;
  std::vector<QueueEntry> front;
  PlayedEntry cur{};
  if (playing_now) {
    cur = hist_.back();
    hist_.pop_back();
    if (lib_) try { lib_->pop_queue_history(); } catch (...) {}
  }
  const PlayedEntry prev = hist_.back();
  hist_.pop_back();
  if (lib_) try { lib_->pop_queue_history(); } catch (...) {}
  QueueEntry p;
  p.id = next_id_++;
  p.track_id = prev.track_id;
  p.play_next = true;
  front.push_back(p);
  if (playing_now) {  // the track playing now comes back right after it
    QueueEntry c = *current_;
    c.id = next_id_++;
    c.play_next = true;
    front.push_back(c);
  }
  up_.insert(up_.begin(), front.begin(), front.end());
  normalise_locked();
  persist_locked();
  changed_locked();
  notify(lock);
  return true;
}

std::optional<QueueEntry> PlayQueue::begin_pending() {
  std::unique_lock<std::mutex> lock(m_);
  if (pending_ || up_.empty()) return std::nullopt;
  pending_ = up_.front();
  up_.erase(up_.begin());
  persist_locked();
  changed_locked();
  QueueEntry e = *pending_;
  notify(lock);
  return e;
}

void PlayQueue::abort_pending() {
  std::unique_lock<std::mutex> lock(m_);
  if (!pending_) return;
  pending_->play_next = true;
  up_.insert(up_.begin(), *pending_);
  pending_.reset();
  normalise_locked();
  persist_locked();
  changed_locked();
  notify(lock);
}

std::optional<QueueEntry> PlayQueue::commit_pending(std::int64_t now_ms) {
  std::unique_lock<std::mutex> lock(m_);
  if (!pending_) return std::nullopt;
  current_ = pending_;
  pending_.reset();
  hist_.push_back({current_->id, current_->track_id, now_ms});
  if (hist_.size() > kHistoryCached) hist_.erase(hist_.begin());
  if (lib_) try { lib_->add_queue_history(current_->id, current_->track_id, now_ms); } catch (...) {}
  persist_locked();
  changed_locked();
  QueueEntry e = *current_;
  notify(lock);
  return e;
}

std::optional<QueueEntry> PlayQueue::adopt_current(std::int64_t track_id, std::int64_t now_ms) {
  std::unique_lock<std::mutex> lock(m_);
  QueueEntry e;
  e.id = next_id_++;
  e.track_id = track_id;
  current_ = e;
  hist_.push_back({e.id, e.track_id, now_ms});
  if (hist_.size() > kHistoryCached) hist_.erase(hist_.begin());
  if (lib_) try { lib_->add_queue_history(e.id, e.track_id, now_ms); } catch (...) {}
  changed_locked();
  notify(lock);
  return e;
}

void PlayQueue::clear_current() {
  std::unique_lock<std::mutex> lock(m_);
  if (!current_) return;
  current_.reset();
  changed_locked();
  notify(lock);
}

}  // namespace deckhand
