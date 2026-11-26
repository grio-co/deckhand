#pragma once
// One small UDP socket wrapper for POSIX and Winsock. IPv4 only. Non-blocking.
#include <cstddef>
#include <cstdint>
#include <string>

namespace deckhand::lighting {

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket() { close(); }
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // Binds bind_ip:port (port 0 = ephemeral). reuse sets SO_REUSEADDR (and SO_REUSEPORT where it exists) so that a
  // well known port can be shared with another program on the machine.
  bool open(const std::string& bind_ip, std::uint16_t port, bool reuse, std::string* err = nullptr);
  void close();
  bool is_open() const { return h_ != -1; }
  std::uint16_t local_port() const { return port_; }
  bool set_broadcast(bool on);
  // Multicast sending: TTL and the local interface address ("" = system default).
  bool set_multicast_send(int ttl, const std::string& interface_ip);
  // Returns bytes sent, -1 on error (a full buffer counts as an error: the next frame replaces the lost one).
  long send_to(const std::string& ip, std::uint16_t port, const std::uint8_t* data, std::size_t n);
  // Non-blocking: bytes received, 0 when nothing is pending, -1 on error.
  long recv_from(std::uint8_t* buf, std::size_t cap, std::string* from_ip, std::uint16_t* from_port);

 private:
  std::intptr_t h_ = -1;  // fd on POSIX, SOCKET on Windows
  std::uint16_t port_ = 0;
};

// True for "a.b.c.d" dotted quads only (no DNS lookups are ever done).
bool parse_ipv4(const std::string& s, std::uint8_t out[4]);

}  // namespace deckhand::lighting
