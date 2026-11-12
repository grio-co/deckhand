#pragma once
// Private thin socket layer (POSIX). On platforms without it every call fails, so start() just returns false.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace deckhand::broadcast::net {

struct PollItem {
  int fd = -1;
  bool want_read = false;
  bool want_write = false;
  bool readable = false;  // data, EOF or error pending: a recv will tell
  bool writable = false;
};

double now_seconds();                                  // steady clock
int poll(std::vector<PollItem>& items, int timeout_ms);  // <0 on failure
int listen_loopback(std::uint16_t port, std::uint16_t* bound);  // non-blocking; 127.0.0.1 only
int accept_client(int listen_fd);                      // non-blocking client fd or -1
// Connects (non-blocking socket returned); abort_fd (may be -1) becoming readable cancels the attempt.
int connect_tcp(const std::string& host, std::uint16_t port, int timeout_ms, int abort_fd);
void close_fd(int fd);
long send_some(int fd, const char* data, std::size_t n);  // bytes sent, 0 = would block, -1 = error
long recv_some(int fd, char* data, std::size_t n);        // bytes, 0 = would block, -1 = closed/error
bool send_all(int fd, const std::string& data, int timeout_ms, int abort_fd);

// Self-pipe used to wake a poll loop from another thread.
class Wake {
 public:
  Wake();
  ~Wake();
  Wake(const Wake&) = delete;
  Wake& operator=(const Wake&) = delete;
  int fd() const { return r_; }
  void poke();
  void drain();

 private:
  int r_ = -1, w_ = -1;
};

}  // namespace deckhand::broadcast::net
