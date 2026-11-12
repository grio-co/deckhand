#include "sock.hpp"

#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace deckhand::broadcast::net {

double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

#ifndef _WIN32

namespace {

void configure(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

}  // namespace

int poll(std::vector<PollItem>& items, int timeout_ms) {
  std::vector<pollfd> p(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    p[i].fd = items[i].fd;
    p[i].events = static_cast<short>((items[i].want_read ? POLLIN : 0) | (items[i].want_write ? POLLOUT : 0));
    p[i].revents = 0;
  }
  const int r = ::poll(p.data(), static_cast<nfds_t>(p.size()), timeout_ms);
  for (std::size_t i = 0; i < items.size(); ++i) {
    items[i].readable = r > 0 && (p[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
    items[i].writable = r > 0 && (p[i].revents & (POLLOUT | POLLERR | POLLHUP)) != 0;
  }
  if (r < 0 && errno == EINTR) return 0;
  return r;
}

int listen_loopback(std::uint16_t port, std::uint16_t* bound) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 16) != 0) {
    ::close(fd);
    return -1;
  }
  configure(fd);
  if (bound) {
    socklen_t len = sizeof a;
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    *bound = ntohs(a.sin_port);
  }
  return fd;
}

int accept_client(int listen_fd) {
  const int fd = ::accept(listen_fd, nullptr, nullptr);
  if (fd < 0) return -1;
  configure(fd);
  return fd;
}

int connect_tcp(const std::string& host, std::uint16_t port, int timeout_ms, int abort_fd) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return -1;
  int result = -1;
  const double deadline = now_seconds() + timeout_ms / 1000.0;
  for (addrinfo* ai = res; ai && result < 0; ai = ai->ai_next) {
    const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    configure(fd);
    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      for (;;) {
        const int left = static_cast<int>((deadline - now_seconds()) * 1000);
        if (left <= 0) { rc = -1; break; }
        std::vector<PollItem> it{{fd, false, true}};
        if (abort_fd >= 0) it.push_back({abort_fd, true, false});
        if (net::poll(it, left) < 0) { rc = -1; break; }
        if (abort_fd >= 0 && it[1].readable) { rc = -1; break; }
        if (it[0].writable) {
          int err = 0;
          socklen_t len = sizeof err;
          getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
          rc = err == 0 ? 0 : -1;
          break;
        }
      }
    }
    if (rc == 0) {
      const int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      result = fd;
    } else {
      ::close(fd);
    }
  }
  freeaddrinfo(res);
  return result;
}

void close_fd(int fd) {
  if (fd >= 0) ::close(fd);
}

long send_some(int fd, const char* data, std::size_t n) {
  const ssize_t r = ::send(fd, data, n, kSendFlags);
  if (r >= 0) return static_cast<long>(r);
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
}

long recv_some(int fd, char* data, std::size_t n) {
  const ssize_t r = ::recv(fd, data, n, 0);
  if (r > 0) return static_cast<long>(r);
  if (r == 0) return -1;
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
}

bool send_all(int fd, const std::string& data, int timeout_ms, int abort_fd) {
  std::size_t off = 0;
  const double deadline = now_seconds() + timeout_ms / 1000.0;
  while (off < data.size()) {
    const long n = send_some(fd, data.data() + off, data.size() - off);
    if (n < 0) return false;
    off += static_cast<std::size_t>(n);
    if (off >= data.size()) break;
    if (n == 0) {
      const int left = static_cast<int>((deadline - now_seconds()) * 1000);
      if (left <= 0) return false;
      std::vector<PollItem> it{{fd, false, true}};
      if (abort_fd >= 0) it.push_back({abort_fd, true, false});
      if (net::poll(it, left) < 0) return false;
      if (abort_fd >= 0 && it[1].readable) return false;
    }
  }
  return true;
}

Wake::Wake() {
  int p[2];
  if (::pipe(p) == 0) {
    r_ = p[0];
    w_ = p[1];
    for (int fd : p) {
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
      fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
  }
}

Wake::~Wake() {
  close_fd(r_);
  close_fd(w_);
}

void Wake::poke() {
  if (w_ < 0) return;
  const char c = 1;
  const ssize_t r = ::write(w_, &c, 1);
  (void)r;
}

void Wake::drain() {
  char buf[64];
  while (r_ >= 0 && ::read(r_, buf, sizeof buf) > 0) {
  }
}

#else  // _WIN32: Winsock. Descriptors are SOCKETs narrowed to int (kernel handle values always fit).

namespace {
SOCKET sk(int fd) { return static_cast<SOCKET>(fd); }
int to_fd(SOCKET s) { return s == INVALID_SOCKET ? -1 : static_cast<int>(s); }

void ensure_winsock() {
  static const bool once = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  (void)once;
}

void configure(int fd) {
  u_long one = 1;
  ioctlsocket(sk(fd), FIONBIO, &one);
}
bool again(int e) { return e == WSAEWOULDBLOCK || e == WSAEINTR; }
}  // namespace

int poll(std::vector<PollItem>& items, int timeout_ms) {
  if (items.empty()) {
    Sleep(static_cast<DWORD>(timeout_ms < 0 ? 0 : timeout_ms));
    return 0;
  }
  std::vector<WSAPOLLFD> p(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    p[i].fd = items[i].fd < 0 ? INVALID_SOCKET : sk(items[i].fd);
    p[i].events = static_cast<SHORT>((items[i].want_read ? POLLRDNORM : 0) | (items[i].want_write ? POLLWRNORM : 0));
    p[i].revents = 0;
  }
  const int r = WSAPoll(p.data(), static_cast<ULONG>(p.size()), timeout_ms);
  for (std::size_t i = 0; i < items.size(); ++i) {
    items[i].readable = r > 0 && (p[i].revents & (POLLRDNORM | POLLHUP | POLLERR | POLLNVAL)) != 0;
    items[i].writable = r > 0 && (p[i].revents & (POLLWRNORM | POLLERR | POLLHUP)) != 0;
  }
  if (r < 0 && again(WSAGetLastError())) return 0;
  return r;
}

int listen_loopback(std::uint16_t port, std::uint16_t* bound) {
  ensure_winsock();
  const SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) return -1;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 16) != 0) {
    ::closesocket(s);
    return -1;
  }
  configure(to_fd(s));
  if (bound) {
    int len = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    *bound = ntohs(a.sin_port);
  }
  return to_fd(s);
}

int accept_client(int listen_fd) {
  const SOCKET s = ::accept(sk(listen_fd), nullptr, nullptr);
  if (s == INVALID_SOCKET) return -1;
  configure(to_fd(s));
  return to_fd(s);
}

int connect_tcp(const std::string& host, std::uint16_t port, int timeout_ms, int abort_fd) {
  ensure_winsock();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return -1;
  int result = -1;
  const double deadline = now_seconds() + timeout_ms / 1000.0;
  for (addrinfo* ai = res; ai && result < 0; ai = ai->ai_next) {
    const SOCKET s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCKET) continue;
    const int fd = to_fd(s);
    configure(fd);
    int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
    if (rc != 0 && again(WSAGetLastError())) {
      for (;;) {
        const int left = static_cast<int>((deadline - now_seconds()) * 1000);
        if (left <= 0) { rc = -1; break; }
        std::vector<PollItem> it{{fd, false, true}};
        if (abort_fd >= 0) it.push_back({abort_fd, true, false});
        if (net::poll(it, left) < 0) { rc = -1; break; }
        if (abort_fd >= 0 && it[1].readable) { rc = -1; break; }
        if (it[0].writable) {
          int err = 0;
          int len = sizeof err;
          getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
          rc = err == 0 ? 0 : -1;
          break;
        }
      }
    }
    if (rc == 0) {
      const int one = 1;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
      result = fd;
    } else {
      ::closesocket(s);
    }
  }
  freeaddrinfo(res);
  return result;
}

void close_fd(int fd) {
  if (fd >= 0) ::closesocket(sk(fd));
}

long send_some(int fd, const char* data, std::size_t n) {
  const int r = ::send(sk(fd), data, static_cast<int>(n > (1u << 30) ? (1u << 30) : n), 0);
  if (r >= 0) return r;
  return again(WSAGetLastError()) ? 0 : -1;
}

long recv_some(int fd, char* data, std::size_t n) {
  const int r = ::recv(sk(fd), data, static_cast<int>(n > (1u << 30) ? (1u << 30) : n), 0);
  if (r > 0) return r;
  if (r == 0) return -1;
  return again(WSAGetLastError()) ? 0 : -1;
}

bool send_all(int fd, const std::string& data, int timeout_ms, int abort_fd) {
  std::size_t off = 0;
  const double deadline = now_seconds() + timeout_ms / 1000.0;
  while (off < data.size()) {
    const long n = send_some(fd, data.data() + off, data.size() - off);
    if (n < 0) return false;
    off += static_cast<std::size_t>(n);
    if (off >= data.size()) break;
    if (n == 0) {
      const int left = static_cast<int>((deadline - now_seconds()) * 1000);
      if (left <= 0) return false;
      std::vector<PollItem> it{{fd, false, true}};
      if (abort_fd >= 0) it.push_back({abort_fd, true, false});
      if (net::poll(it, left) < 0) return false;
      if (abort_fd >= 0 && it[1].readable) return false;
    }
  }
  return true;
}

// Winsock has no pipe(): a connected loopback pair does the same job.
Wake::Wake() {
  std::uint16_t port = 0;
  const int l = listen_loopback(0, &port);
  if (l < 0) return;
  const int c = connect_tcp("127.0.0.1", port, 2000, -1);
  int s = -1;
  if (c >= 0) {
    std::vector<PollItem> it{{l, true, false}};
    if (net::poll(it, 2000) > 0) s = accept_client(l);
  }
  close_fd(l);
  if (c < 0 || s < 0) {
    close_fd(c);
    close_fd(s);
    return;
  }
  r_ = s;
  w_ = c;
}

Wake::~Wake() {
  close_fd(r_);
  close_fd(w_);
}

void Wake::poke() {
  if (w_ < 0) return;
  const char c = 1;
  (void)send_some(w_, &c, 1);
}

void Wake::drain() {
  char buf[64];
  while (r_ >= 0 && recv_some(r_, buf, sizeof buf) > 0) {
  }
}

#endif

}  // namespace deckhand::broadcast::net
