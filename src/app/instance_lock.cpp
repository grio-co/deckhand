#include "deckhand/app/instance_lock.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace deckhand {

namespace fs = std::filesystem;

struct InstanceLock::Impl {
#if defined(_WIN32)
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~Impl() {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
#else
  int fd = -1;
  ~Impl() {
    if (fd >= 0) ::close(fd);  // closing drops the flock
  }
#endif
};

InstanceLock::~InstanceLock() = default;

namespace {
std::string pid_text() {
#if defined(_WIN32)
  return std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));
#else
  return std::to_string(static_cast<long>(::getpid()));
#endif
}
}  // namespace

std::unique_ptr<InstanceLock> InstanceLock::acquire(const std::string& data_dir, std::string& holder, std::string& error) {
  holder.clear();
  error.clear();
  std::error_code ec;
  fs::create_directories(fs::u8path(data_dir), ec);
  const fs::path file = fs::u8path(data_dir) / file_name();
  std::unique_ptr<InstanceLock> lock(new InstanceLock());
  lock->impl_.reset(new Impl());
  const std::string mine = pid_text();
#if defined(_WIN32)
  // Share mode 0: nobody else can open the file while we hold the handle (including to read the owner's pid, so the
  // message on Windows names no process).
  HANDLE h = CreateFileW(file.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD e = GetLastError();
    if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) return nullptr;
    error = "cannot open " + file.string() + " (error " + std::to_string(static_cast<unsigned long>(e)) + ")";
    return nullptr;
  }
  lock->impl_->handle = h;
  SetFilePointer(h, 0, nullptr, FILE_BEGIN);
  SetEndOfFile(h);
  DWORD written = 0;
  WriteFile(h, mine.data(), static_cast<DWORD>(mine.size()), &written, nullptr);
#else
  const int fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    error = "cannot open " + file.string();
    return nullptr;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    char text[32] = {};
    const ssize_t n = ::read(fd, text, sizeof text - 1);  // the owner's pid, written after it took the lock
    if (n > 0) holder = text;
    while (!holder.empty() && (holder.back() == '\n' || holder.back() == ' ')) holder.pop_back();
    ::close(fd);
    return nullptr;
  }
  lock->impl_->fd = fd;
  if (::ftruncate(fd, 0) == 0) {
    const ssize_t w = ::write(fd, mine.data(), mine.size());
    (void)w;
  }
#endif
  return lock;
}

std::string instance_conflict_message(const std::string& data_dir, const std::string& holder) {
  std::string m = "DeckHand is already running with this data folder";
  if (!holder.empty()) m += " (process " + holder + ")";
  m += ": " + data_dir + ".\nSwitch to that window, or quit it first. To run a second copy at the same time, give it its own data "
       "folder (a file named \"portable\" beside the app).";
  return m;
}

}  // namespace deckhand
