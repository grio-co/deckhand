#include "deckhand/lighting/net.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace deckhand::lighting {

namespace {
#ifdef _WIN32
using Sock = SOCKET;
const Sock kBad = INVALID_SOCKET;
void ensure_winsock() {
  static const bool once = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
  (void)once;
}
void close_sock(Sock s) { ::closesocket(s); }
bool set_nonblocking(Sock s) { u_long on = 1; return ::ioctlsocket(s, FIONBIO, &on) == 0; }
#else
using Sock = int;
const Sock kBad = -1;
void ensure_winsock() {}
void close_sock(Sock s) { ::close(s); }
bool set_nonblocking(Sock s) {
  const int fl = ::fcntl(s, F_GETFL, 0);
  return fl >= 0 && ::fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
}
#endif
Sock as_sock(std::intptr_t h) { return static_cast<Sock>(h); }
bool make_addr(const std::string& ip, std::uint16_t port, sockaddr_in* a) {
  std::memset(a, 0, sizeof *a);
  a->sin_family = AF_INET;
  a->sin_port = htons(port);
  return ::inet_pton(AF_INET, ip.c_str(), &a->sin_addr) == 1;
}
}  // namespace

bool parse_ipv4(const std::string& s, std::uint8_t out[4]) {
  in_addr a;
  ensure_winsock();
  if (s.empty() || ::inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
  std::memcpy(out, &a, 4);
  return true;
}

bool UdpSocket::open(const std::string& bind_ip, std::uint16_t port, bool reuse, std::string* err) {
  close();
  ensure_winsock();
  sockaddr_in a;
  if (!make_addr(bind_ip.empty() ? "0.0.0.0" : bind_ip, port, &a)) {
    if (err) *err = "bad bind address: " + bind_ip;
    return false;
  }
  Sock s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kBad) {
    if (err) *err = "socket() failed";
    return false;
  }
  if (reuse) {
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#if defined(SO_REUSEPORT) && !defined(_WIN32)
    ::setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
  }
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || !set_nonblocking(s)) {
    if (err) *err = "cannot bind " + bind_ip + ":" + std::to_string(port);
    close_sock(s);
    return false;
  }
  sockaddr_in got;
  socklen_t gl = sizeof got;
  port_ = ::getsockname(s, reinterpret_cast<sockaddr*>(&got), &gl) == 0 ? ntohs(got.sin_port) : port;
  h_ = static_cast<std::intptr_t>(s);
  return true;
}

void UdpSocket::close() {
  if (h_ != -1) close_sock(as_sock(h_));
  h_ = -1;
  port_ = 0;
}

bool UdpSocket::set_broadcast(bool on) {
  if (h_ == -1) return false;
  int v = on ? 1 : 0;
  return ::setsockopt(as_sock(h_), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&v), sizeof v) == 0;
}

bool UdpSocket::set_multicast_send(int ttl, const std::string& interface_ip) {
  if (h_ == -1) return false;
#ifdef _WIN32
  DWORD t = static_cast<DWORD>(ttl);
#else
  unsigned char t = static_cast<unsigned char>(ttl);
#endif
  bool ok = ::setsockopt(as_sock(h_), IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&t), sizeof t) == 0;
  if (!interface_ip.empty()) {
    in_addr ia;
    if (::inet_pton(AF_INET, interface_ip.c_str(), &ia) != 1) return false;
    ok = ::setsockopt(as_sock(h_), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&ia), sizeof ia) == 0 && ok;
  }
  return ok;
}

long UdpSocket::send_to(const std::string& ip, std::uint16_t port, const std::uint8_t* data, std::size_t n) {
  sockaddr_in a;
  if (h_ == -1 || !make_addr(ip, port, &a)) return -1;
  const auto r = ::sendto(as_sock(h_), reinterpret_cast<const char*>(data), static_cast<int>(n), 0,
                          reinterpret_cast<sockaddr*>(&a), sizeof a);
  return r < 0 ? -1 : static_cast<long>(r);
}

long UdpSocket::recv_from(std::uint8_t* buf, std::size_t cap, std::string* from_ip, std::uint16_t* from_port) {
  if (h_ == -1) return -1;
  sockaddr_in a;
  socklen_t al = sizeof a;
  const auto r = ::recvfrom(as_sock(h_), reinterpret_cast<char*>(buf), static_cast<int>(cap), 0, reinterpret_cast<sockaddr*>(&a), &al);
  if (r < 0) {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return (e == WSAEWOULDBLOCK || e == WSAECONNRESET) ? 0 : -1;  // CONNRESET: ICMP unreachable from an earlier send
#else
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
#endif
  }
  if (from_ip) {
    char t[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &a.sin_addr, t, sizeof t);
    *from_ip = t;
  }
  if (from_port) *from_port = ntohs(a.sin_port);
  return static_cast<long>(r);
}

}  // namespace deckhand::lighting
