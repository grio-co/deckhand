// POSIX (macOS, Linux) implementation of the OS interface in deckhand/audio/ipc.hpp.
// This is the only file that touches system headers for IPC; a Windows port adds ipc_win32.cpp with the same classes.
#include "deckhand/audio/ipc.hpp"

#if defined(__unix__) || defined(__APPLE__)

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace deckhand::ipc {

namespace {
std::string errno_text(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

void set_cloexec(int fd) {
  const int fl = fcntl(fd, F_GETFD);
  if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

void no_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}
}  // namespace

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint32_t current_pid() { return static_cast<std::uint32_t>(getpid()); }

bool process_alive(std::uint32_t pid) {
  if (pid == 0) return false;
  return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

bool reap_child(std::uint32_t pid) {
  if (pid == 0) return false;
  int st = 0;
  return waitpid(static_cast<pid_t>(pid), &st, WNOHANG) == static_cast<pid_t>(pid);
}

std::string executable_dir() {
#if defined(__APPLE__)
  char buf[4096];
  std::uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) != 0) return {};
  std::error_code ec;
  const auto p = std::filesystem::weakly_canonical(buf, ec);
  return (ec ? std::filesystem::path(buf) : p).parent_path().string();
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return {};
  buf[n] = 0;
  return std::filesystem::path(buf).parent_path().string();
#endif
}

// ---- shared memory ------------------------------------------------------------------------------
Shm& Shm::operator=(Shm&& o) noexcept {
  if (this != &o) {
    close();
    base_ = o.base_;
    size_ = o.size_;
    o.base_ = nullptr;
    o.size_ = 0;
  }
  return *this;
}

void Shm::close() {
  if (base_) munmap(base_, size_);
  base_ = nullptr;
  size_ = 0;
}

bool Shm::create(const std::string& name, std::size_t size, Shm& out, std::string& error, bool replace) {
  out.close();
  if (replace) shm_unlink(name.c_str());
  const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    error = errno_text("shm_open");
    return false;
  }
  set_cloexec(fd);
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
    error = errno_text("ftruncate");
    ::close(fd);
    shm_unlink(name.c_str());
    return false;
  }
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) {
    error = errno_text("mmap");
    shm_unlink(name.c_str());
    return false;
  }
  out.base_ = p;
  out.size_ = size;
  return true;
}

bool Shm::open(const std::string& name, bool writable, Shm& out, std::string& error) {
  out.close();
  const int fd = shm_open(name.c_str(), writable ? O_RDWR : O_RDONLY, 0);
  if (fd < 0) {
    error = errno_text("shm_open");
    return false;
  }
  set_cloexec(fd);
  struct stat st {};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    error = "segment is empty";
    ::close(fd);
    return false;
  }
  const std::size_t size = static_cast<std::size_t>(st.st_size);
  void* p = mmap(nullptr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) {
    error = errno_text("mmap");
    return false;
  }
  out.base_ = p;
  out.size_ = size;
  return true;
}

void Shm::remove(const std::string& name) { shm_unlink(name.c_str()); }

// ---- instance lock ------------------------------------------------------------------------------
bool InstanceLock::acquire(const std::string& path, std::string& error) {
  release();
  for (int attempt = 0; attempt < 5; ++attempt) {
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
      error = errno_text("open lock");
      return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
      error = "another engine already runs for this data folder";
      ::close(fd);
      return false;
    }
    struct stat a {}, b {};
    if (fstat(fd, &a) == 0 && stat(path.c_str(), &b) == 0 && a.st_ino == b.st_ino && a.st_dev == b.st_dev) {
      fd_ = fd;
      path_ = path;
      return true;
    }
    ::close(fd);  // the previous owner deleted the file while we opened it: lock the new one
  }
  error = "could not take the instance lock";
  return false;
}

void InstanceLock::release() {
  if (fd_ >= 0) {
    flock(fd_, LOCK_UN);
    ::close(fd_);
  }
  fd_ = -1;
}

void InstanceLock::release_and_remove() {
  if (fd_ >= 0 && !path_.empty()) unlink(path_.c_str());
  release();
}

// ---- sockets ------------------------------------------------------------------------------------
namespace {
bool fill_addr(const std::string& path, sockaddr_un& a) {
  std::memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  if (path.size() >= sizeof a.sun_path) return false;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  return true;
}
}  // namespace

void Conn::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool Conn::connect(const std::string& path, Conn& out) {
  out.close();
  sockaddr_un a;
  if (!fill_addr(path, a)) return false;
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  set_cloexec(fd);
  no_sigpipe(fd);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return false;
  }
  out.fd_ = fd;
  return true;
}

bool Conn::send_all(const void* data, std::size_t n) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  while (n > 0) {
#if defined(MSG_NOSIGNAL)
    const ssize_t w = ::send(fd_, p, n, MSG_NOSIGNAL);
#else
    const ssize_t w = ::send(fd_, p, n, 0);
#endif
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return false;
    p += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}

bool Conn::recv_all(void* data, std::size_t n, int timeout_ms, bool* timed_out) {
  if (timed_out) *timed_out = false;
  auto* p = static_cast<std::uint8_t*>(data);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (n > 0) {
    const long long left = std::max<long long>(
        0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    pollfd pf{fd_, POLLIN, 0};
    const int r = poll(&pf, 1, static_cast<int>(left));
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) return false;
    if (r == 0) {
      if (timed_out) *timed_out = true;
      return false;
    }
    const ssize_t got = ::read(fd_, p, n);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
    p += got;
    n -= static_cast<std::size_t>(got);
  }
  return true;
}

void Listener::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  if (!path_.empty()) unlink(path_.c_str());
  path_.clear();
}

bool Listener::listen(const std::string& path, std::string& error) {
  close();
  sockaddr_un a;
  if (!fill_addr(path, a)) {
    error = "socket path too long";
    return false;
  }
  unlink(path.c_str());
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    error = errno_text("socket");
    return false;
  }
  set_cloexec(fd);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 8) != 0) {
    error = errno_text("bind/listen");
    ::close(fd);
    return false;
  }
  chmod(path.c_str(), 0600);
  fd_ = fd;
  path_ = path;
  return true;
}

bool Listener::accept(Conn& out, int timeout_ms) {
  pollfd pf{fd_, POLLIN, 0};
  const int r = poll(&pf, 1, timeout_ms);
  if (r <= 0) return false;
  const int c = ::accept(fd_, nullptr, nullptr);
  if (c < 0) return false;
  set_cloexec(c);
  no_sigpipe(c);
  out.close();
  out.fd_ = c;
  return true;
}

// ---- naming -------------------------------------------------------------------------------------
std::string SessionNames::block(int slot) const {
  char n[8];
  std::snprintf(n, sizeof n, "%02d", slot < 0 ? 0 : slot % 100);
  return shm_block_prefix + n;
}

bool session_names(const std::string& data_dir, SessionNames& out, std::string& error) {
  std::error_code ec;
  const std::string canon = std::filesystem::weakly_canonical(data_dir, ec).string();
  const std::string key = std::to_string(static_cast<unsigned long>(getuid())) + ":" + (ec ? data_dir : canon);
  std::uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (const unsigned char c : key) h = (h ^ c) * 1099511628211ull;
  char hex[16];
  std::snprintf(hex, sizeof hex, "%08x", static_cast<unsigned>((h >> 32) ^ (h & 0xFFFFFFFFu)));

  const char* over = std::getenv("DECKHAND_IPC_DIR");  // tests and unusual setups
  out.dir = over && *over ? over : "/tmp/dh-" + std::to_string(static_cast<unsigned long>(getuid()));
  mkdir(out.dir.c_str(), 0700);
  struct stat st {};
  if (lstat(out.dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
    error = "unsafe IPC folder " + out.dir;
    return false;
  }
  chmod(out.dir.c_str(), 0700);
  out.lock = out.dir + "/" + hex + ".lock";
  out.socket = out.dir + "/" + hex + ".sock";
  out.shm_main = std::string("/dh") + hex + ".m";
  out.shm_block_prefix = std::string("/dh") + hex + ".b";
  if (out.socket.size() >= sizeof(sockaddr_un::sun_path) - 1 || out.shm_main.size() > 30) {
    error = "IPC names too long";
    return false;
  }
  return true;
}

bool spawn_detached(const std::string& exe, const std::vector<std::string>& args, const std::string& log_file,
                    std::uint32_t& pid, std::string& error) {
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, log_file.empty() ? "/dev/null" : log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  short flags = 0;
#if defined(POSIX_SPAWN_SETSID)
  flags |= POSIX_SPAWN_SETSID;
#endif
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  posix_spawnattr_setflags(&attr, flags);
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(exe.c_str()));
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t child = 0;
  const int rc = posix_spawn(&child, exe.c_str(), &fa, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    error = "could not start " + exe + ": " + std::strerror(rc);
    return false;
  }
  pid = static_cast<std::uint32_t>(child);
  return true;
}

}  // namespace deckhand::ipc

#endif  // POSIX
