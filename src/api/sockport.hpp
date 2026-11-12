#pragma once
// Internal: the few differences between BSD sockets and Winsock, so net.cpp reads the same on both.
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
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
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace deckhand::api::net {

#if defined(_WIN32)
using Fd = std::intptr_t;  // a SOCKET; INVALID_SOCKET converts to -1
constexpr Fd kBadFd = -1;
using SockLen = int;
using IoLen = int;
inline SOCKET raw(Fd f) { return static_cast<SOCKET>(f); }
inline void ensure_sockets() {
  static const bool once = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  (void)once;
}
inline int last_error() { return WSAGetLastError(); }
inline bool would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINTR; }
inline std::string error_text(int e) { return "error " + std::to_string(e); }
inline void close_socket(Fd f) { ::closesocket(raw(f)); }
inline void shutdown_both(Fd f) { ::shutdown(raw(f), SD_BOTH); }
inline void set_nonblocking(Fd f) {
  u_long one = 1;
  ioctlsocket(raw(f), FIONBIO, &one);
}
inline void set_nodelay(Fd f) {
  int one = 1;
  setsockopt(raw(f), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}
struct PollItem {
  Fd fd;
  short events;
  short revents;
};
constexpr short kPollIn = POLLRDNORM;
constexpr short kPollOut = POLLWRNORM;
constexpr short kPollErr = POLLERR | POLLNVAL;
constexpr short kPollHup = POLLHUP;
inline int poll_items(PollItem* items, int n, int timeout_ms) {
  WSAPOLLFD p[4];
  if (n > 4) return -1;
  for (int i = 0; i < n; ++i) p[i] = WSAPOLLFD{raw(items[i].fd), items[i].events, 0};
  // WSAPoll rejects an invalid socket in the set: replace it by a quiet one.
  int rc;
  if (n == 1) rc = WSAPoll(p, 1, timeout_ms);
  else rc = WSAPoll(p, static_cast<ULONG>(n), timeout_ms);
  for (int i = 0; i < n; ++i) items[i].revents = p[i].revents;
  return rc;
}
inline long sock_recv(Fd f, void* b, std::size_t n) { return ::recv(raw(f), static_cast<char*>(b), static_cast<int>(n), 0); }
inline long sock_send(Fd f, const void* b, std::size_t n) {
  return ::send(raw(f), static_cast<const char*>(b), static_cast<int>(n > (1u << 30) ? (1u << 30) : n), 0);
}
#else
using Fd = int;
constexpr Fd kBadFd = -1;
using SockLen = socklen_t;
inline int raw(Fd f) { return f; }
inline void ensure_sockets() {
  static const bool once = [] {
    signal(SIGPIPE, SIG_IGN);
    return true;
  }();
  (void)once;
}
inline int last_error() { return errno; }
inline bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }
inline std::string error_text(int e) { return std::strerror(e); }
inline void close_socket(Fd f) { ::close(f); }
inline void shutdown_both(Fd f) { ::shutdown(f, SHUT_RDWR); }
inline void set_nonblocking(Fd f) {
  int fl = fcntl(f, F_GETFL, 0);
  if (fl >= 0) fcntl(f, F_SETFL, fl | O_NONBLOCK);
}
inline void set_nodelay(Fd f) {
  int one = 1;
#ifdef SO_NOSIGPIPE
  setsockopt(f, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  setsockopt(f, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}
struct PollItem {
  Fd fd;
  short events;
  short revents;
};
constexpr short kPollIn = POLLIN;
constexpr short kPollOut = POLLOUT;
constexpr short kPollErr = POLLERR | POLLNVAL;
constexpr short kPollHup = POLLHUP;
inline int poll_items(PollItem* items, int n, int timeout_ms) {
  pollfd p[4];
  if (n > 4) return -1;
  for (int i = 0; i < n; ++i) p[i] = pollfd{items[i].fd, items[i].events, 0};
  const int rc = ::poll(p, static_cast<nfds_t>(n), timeout_ms);
  for (int i = 0; i < n; ++i) items[i].revents = p[i].revents;
  return rc;
}
inline long sock_recv(Fd f, void* b, std::size_t n) { return static_cast<long>(::recv(f, b, n, 0)); }
inline long sock_send(Fd f, const void* b, std::size_t n) {
#ifdef MSG_NOSIGNAL
  return static_cast<long>(::send(f, b, n, MSG_NOSIGNAL));
#else
  return static_cast<long>(::send(f, b, n, 0));
#endif
}
#endif

}  // namespace deckhand::api::net
