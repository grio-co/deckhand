#pragma once
// Internal: the only place that touches sockets and OpenSSL. Everything else in the module
// talks to these wrappers, so a Winsock port only has to replace net.cpp.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace deckhand::api::net {

using Fd = int;
constexpr Fd kBadFd = -1;

void init();  // process-wide setup (ignores SIGPIPE)
void close_fd(Fd fd);
bool is_loopback_address(const std::string& addr);
// Binds and listens (non-blocking). Returns kBadFd with *err on failure.
Fd listen_tcp(const std::string& addr, std::uint16_t port, std::uint16_t* bound, std::string* err);
// Waits up to timeout_ms for a connection or for `wake`; kBadFd when none. Peer address is
// normalized (IPv4-mapped IPv6 becomes plain IPv4).
Fd accept_conn(Fd listen, Fd wake, int timeout_ms, std::string* peer_ip);
Fd connect_tcp(const std::string& host, std::uint16_t port, std::string* err);

// Self-pipe used to wake a poll() from another thread.
struct Wake {
  Fd r = kBadFd, w = kBadFd;
  bool open();
  void signal();
  void drain();
  void close();
};

class TlsContext {
 public:
  static std::unique_ptr<TlsContext> server(const std::string& cert, const std::string& key, std::string* err);
  static std::unique_ptr<TlsContext> client(int max_version, std::string* err);  // no verification
  ~TlsContext();
  void* handle() const { return ctx_; }

 private:
  TlsContext() = default;
  void* ctx_ = nullptr;
};

// Non-blocking byte stream, plain or TLS. read/write/wait are only used by one thread at a
// time; shutdown() may be called from any thread.
class Stream {
 public:
  explicit Stream(Fd fd);
  ~Stream();
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  bool start_tls_server(TlsContext& ctx, int timeout_ms);
  bool start_tls_client(TlsContext& ctx, int timeout_ms);
  bool tls() const { return ssl_ != nullptr; }
  std::string tls_version() const;
  std::string peer_cert_fingerprint() const;  // SHA-256 hex of the peer certificate (DER)

  // 0 = timeout, 1 = readable (or TLS data pending), 2 = woken via `wake`, -1 = error.
  int wait_readable(Fd wake, int timeout_ms);
  // >0 bytes read, 0 = would block, -1 = closed or failed.
  long read(void* buf, std::size_t n);
  bool write_all(const void* data, std::size_t n, int timeout_ms);
  void shutdown();  // thread-safe; the owner still closes via the destructor

 private:
  int io_wait(short events, int timeout_ms);
  std::atomic<Fd> fd_;
  void* ssl_ = nullptr;
};

std::string ws_accept_key(const std::string& client_key);  // base64(SHA1(key + GUID))

}  // namespace deckhand::api::net
