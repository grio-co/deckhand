#pragma once
// One DeckHand per data folder. A second copy on the same folder would fight the first for the engine process (and the
// library). The lock is a file <data folder>/deckhand.lock held with an OS lock for as long as the instance lives:
// flock on POSIX, a share-none handle on Windows. The OS drops it when the process ends, however it ends, so a crashed
// instance never leaves a stale lock; the file itself (holding the owner's process id as text) may stay behind.
#include <memory>
#include <string>

namespace deckhand {

class InstanceLock {
 public:
  ~InstanceLock();
  InstanceLock(const InstanceLock&) = delete;
  InstanceLock& operator=(const InstanceLock&) = delete;

  // Takes the lock. Null when another instance holds it (`holder` = that instance's process id as text, "" if unknown) or
  // when the folder cannot be used (`holder` empty and `error` set). A folder that cannot hold a lock does not stop the app:
  // the caller decides (see dh_app_create); `error` is empty for the plain "already running" case.
  static std::unique_ptr<InstanceLock> acquire(const std::string& data_dir, std::string& holder, std::string& error);

  static std::string file_name() { return "deckhand.lock"; }

 private:
  InstanceLock() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The message shown to the user when a second instance is refused.
std::string instance_conflict_message(const std::string& data_dir, const std::string& holder);

}  // namespace deckhand
