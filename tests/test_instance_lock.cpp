// One DeckHand per data folder: the lock file is held with an OS lock, refused while held, free again after release, never
// stale after a crash (the OS drops it), and separate folders (portable mode) do not interfere. Two holders in one process
// and, on POSIX, a second process.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "check.hpp"
#include "deckhand/app/instance_lock.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {

fs::path fresh(const char* name) {
  const fs::path p = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(p, ec);
  return p;
}

void test_two_holders() {
  const fs::path dir = fresh("deckhand_test_instance_lock");
  std::string holder, error;
  auto a = InstanceLock::acquire(dir.string(), holder, error);  // the folder is created
  CHECK(a && error.empty() && fs::exists(dir / InstanceLock::file_name()));
  auto b = InstanceLock::acquire(dir.string(), holder, error);
  CHECK(!b && error.empty());  // refused because it is held, not because of a failure
#if !defined(_WIN32)
  CHECK(!holder.empty() && std::stol(holder) == static_cast<long>(getpid()));
#endif
  CHECK(instance_conflict_message(dir.string(), holder).find("already running") != std::string::npos);
  a.reset();  // released on exit
  b = InstanceLock::acquire(dir.string(), holder, error);
  CHECK(b);
  // another data folder (portable mode) is unaffected
  const fs::path other = fresh("deckhand_test_instance_lock_other");
  auto c = InstanceLock::acquire(other.string(), holder, error);
  CHECK(c);
  b.reset();
  c.reset();
  // a lock file left behind by a crashed instance (text in it, nobody holding it) is not a lock
  {
    std::ofstream f(dir / InstanceLock::file_name());
    f << "99999";
  }
  auto d = InstanceLock::acquire(dir.string(), holder, error);
  CHECK(d);
  d.reset();
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::remove_all(other, ec);
}

#if !defined(_WIN32)
// A second process asks while this one holds the lock: refused (exit 7); after release it gets it (exit 0).
int ask_in_child(const std::string& dir) {
  const pid_t pid = fork();
  if (pid == 0) {
    std::string holder, error;
    auto l = InstanceLock::acquire(dir, holder, error);
    _exit(l ? 0 : (error.empty() ? 7 : 9));
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void test_second_process() {
  const fs::path dir = fresh("deckhand_test_instance_lock_proc");
  std::string holder, error;
  auto a = InstanceLock::acquire(dir.string(), holder, error);
  CHECK(a);
  CHECK(ask_in_child(dir.string()) == 7);
  a.reset();
  CHECK(ask_in_child(dir.string()) == 0);
  // a child that took the lock and ended (a crash looks the same to the OS) leaves nothing behind
  auto b = InstanceLock::acquire(dir.string(), holder, error);
  CHECK(b);
  std::error_code ec;
  b.reset();
  fs::remove_all(dir, ec);
}
#endif

}  // namespace

int main() {
  test_two_holders();
#if !defined(_WIN32)
  test_second_process();
#endif
  std::puts("instance lock ok");
  return 0;
}
