#include "net.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include "deckhand/api/auth.hpp"
#include "deckhand/api/test_client.hpp"

namespace deckhand::api::net {

namespace {
void set_nonblock(Fd fd) {
  set_nonblocking(fd);
  set_nodelay(fd);
}

std::string ip_of(const sockaddr_storage& ss) {
  char host[NI_MAXHOST] = {};
  if (getnameinfo(reinterpret_cast<const sockaddr*>(&ss), sizeof ss, host, sizeof host, nullptr, 0, NI_NUMERICHOST) != 0)
    return {};
  std::string s = host;
  auto pct = s.find('%');
  if (pct != std::string::npos) s.resize(pct);
  if (s.rfind("::ffff:", 0) == 0 && s.find('.') != std::string::npos) s = s.substr(7);
  return s;
}

std::string ssl_error_string() {
  unsigned long e = ERR_get_error();
  char buf[160] = {};
  if (e) ERR_error_string_n(e, buf, sizeof buf);
  ERR_clear_error();
  return buf;
}

int ms_left(std::chrono::steady_clock::time_point deadline) {
  auto d = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
  return d < 0 ? 0 : static_cast<int>(d);
}
}  // namespace

void init() {
  ensure_sockets();
}

void close_fd(Fd fd) {
  if (fd != kBadFd) close_socket(fd);
}

bool is_loopback_address(const std::string& a) {
  if (a == "localhost") return true;
  in_addr a4;
  in6_addr a6;
  if (inet_pton(AF_INET, a.c_str(), &a4) == 1) return (ntohl(a4.s_addr) >> 24) == 127;
  if (inet_pton(AF_INET6, a.c_str(), &a6) == 1) return IN6_IS_ADDR_LOOPBACK(&a6) != 0;
  return false;
}

Fd listen_tcp(const std::string& addr, std::uint16_t port, std::uint16_t* bound, std::string* err) {
  init();
  std::string host = addr == "localhost" ? "127.0.0.1" : addr;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE | AI_NUMERICSERV;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
    if (err) *err = "invalid bind address: " + addr;
    return kBadFd;
  }
  Fd fd = static_cast<Fd>(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
  if (fd < 0) {
    freeaddrinfo(res);
    if (err) *err = "socket() failed";
    return kBadFd;
  }
#if !defined(_WIN32)  // on Windows SO_REUSEADDR would let another process share the port
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
  if (::bind(raw(fd), res->ai_addr, static_cast<SockLen>(res->ai_addrlen)) != 0 || ::listen(raw(fd), 64) != 0) {
    if (err) *err = std::string("cannot listen on ") + addr + ": " + error_text(last_error());
    freeaddrinfo(res);
    close_socket(fd);
    return kBadFd;
  }
  freeaddrinfo(res);
  set_nonblock(fd);
  sockaddr_storage ss{};
  SockLen sl = sizeof ss;
  if (getsockname(raw(fd), reinterpret_cast<sockaddr*>(&ss), &sl) == 0 && bound) {
    *bound = ss.ss_family == AF_INET ? ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port)
                                     : ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
  }
  return fd;
}

Fd accept_conn(Fd listen, Fd wake, int timeout_ms, std::string* peer_ip) {
  PollItem p[2] = {{listen, kPollIn, 0}, {wake, kPollIn, 0}};
  int n = poll_items(p, wake == kBadFd ? 1 : 2, timeout_ms);
  if (n <= 0 || !(p[0].revents & kPollIn)) return kBadFd;
  sockaddr_storage ss{};
  SockLen sl = sizeof ss;
  Fd c = static_cast<Fd>(::accept(raw(listen), reinterpret_cast<sockaddr*>(&ss), &sl));
  if (c < 0) return kBadFd;
  set_nonblock(c);
  if (peer_ip) *peer_ip = ip_of(ss);
  return c;
}

Fd connect_tcp(const std::string& host, std::uint16_t port, std::string* err) {
  init();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
    if (err) *err = "resolve failed";
    return kBadFd;
  }
  Fd fd = static_cast<Fd>(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
  if (fd >= 0 && ::connect(raw(fd), res->ai_addr, static_cast<SockLen>(res->ai_addrlen)) != 0) {
    close_socket(fd);
    fd = kBadFd;
  }
  freeaddrinfo(res);
  if (fd == kBadFd) {
    if (err) *err = "connect failed";
    return kBadFd;
  }
  set_nonblock(fd);
  return fd;
}

#if defined(_WIN32)
// Winsock has no pipe(): a connected loopback pair does the same job.
bool Wake::open() {
  ensure_sockets();
  SOCKET l = ::socket(AF_INET, SOCK_STREAM, 0);
  if (l == INVALID_SOCKET) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int len = sizeof a;
  SOCKET c = INVALID_SOCKET, s = INVALID_SOCKET;
  bool ok = ::bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::listen(l, 1) == 0 &&
            getsockname(l, reinterpret_cast<sockaddr*>(&a), &len) == 0;
  if (ok) {
    c = ::socket(AF_INET, SOCK_STREAM, 0);
    ok = c != INVALID_SOCKET && ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
  }
  if (ok) {
    s = ::accept(l, nullptr, nullptr);
    ok = s != INVALID_SOCKET;
  }
  ::closesocket(l);
  if (!ok) {
    if (c != INVALID_SOCKET) ::closesocket(c);
    if (s != INVALID_SOCKET) ::closesocket(s);
    return false;
  }
  r = static_cast<Fd>(s);
  w = static_cast<Fd>(c);
  set_nonblocking(r);
  set_nonblocking(w);
  return true;
}
void Wake::signal() {
  if (w != kBadFd) {
    char c = 1;
    (void)sock_send(w, &c, 1);
  }
}
void Wake::drain() {
  char buf[64];
  while (r != kBadFd && sock_recv(r, buf, sizeof buf) > 0) {
  }
}
#else
bool Wake::open() {
  int fds[2];
  if (::pipe(fds) != 0) return false;
  r = fds[0];
  w = fds[1];
  fcntl(r, F_SETFL, O_NONBLOCK);
  fcntl(w, F_SETFL, O_NONBLOCK);
  fcntl(r, F_SETFD, FD_CLOEXEC);
  fcntl(w, F_SETFD, FD_CLOEXEC);
  return true;
}
void Wake::signal() {
  if (w != kBadFd) {
    char c = 1;
    (void)!::write(w, &c, 1);
  }
}
void Wake::drain() {
  char buf[64];
  while (r != kBadFd && ::read(r, buf, sizeof buf) > 0) {
  }
}
#endif
void Wake::close() {
  close_fd(r);
  close_fd(w);
  r = w = kBadFd;
}

// ---- TLS ------------------------------------------------------------------------------------

TlsContext::~TlsContext() {
  if (ctx_) SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
}

std::unique_ptr<TlsContext> TlsContext::server(const std::string& cert, const std::string& key, std::string* err) {
  SSL_CTX* c = SSL_CTX_new(TLS_server_method());
  if (!c) {
    if (err) *err = "SSL_CTX_new failed";
    return nullptr;
  }
  SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
  SSL_CTX_set_options(c, SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE | SSL_OP_NO_RENEGOTIATION);
  SSL_CTX_set_mode(c, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if (SSL_CTX_use_certificate_chain_file(c, cert.c_str()) != 1 ||
      SSL_CTX_use_PrivateKey_file(c, key.c_str(), SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(c) != 1) {
    if (err) *err = "cannot load TLS certificate/key: " + ssl_error_string();
    SSL_CTX_free(c);
    return nullptr;
  }
  std::unique_ptr<TlsContext> t(new TlsContext);
  t->ctx_ = c;
  return t;
}

std::unique_ptr<TlsContext> TlsContext::client(int max_version, std::string* err) {
  SSL_CTX* c = SSL_CTX_new(TLS_client_method());
  if (!c) {
    if (err) *err = "SSL_CTX_new failed";
    return nullptr;
  }
  if (max_version != 0) SSL_CTX_set_min_proto_version(c, 0);  // allow old versions for protocol tests
  if (max_version != 0 && SSL_CTX_set_max_proto_version(c, max_version) != 1) {
    if (err) *err = "unsupported TLS version";
    SSL_CTX_free(c);
    return nullptr;
  }
  SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);
  SSL_CTX_set_mode(c, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  std::unique_ptr<TlsContext> t(new TlsContext);
  t->ctx_ = c;
  return t;
}

Stream::Stream(Fd fd) : fd_(fd) {}

Stream::~Stream() {
  if (ssl_) SSL_free(static_cast<SSL*>(ssl_));
  close_fd(fd_.exchange(kBadFd));
}

void Stream::shutdown() {
  Fd f = fd_.load();
  if (f != kBadFd) shutdown_both(f);
}

int Stream::io_wait(short events, int timeout_ms) {
  PollItem p{fd_.load(), events, 0};
  int n = poll_items(&p, 1, timeout_ms);
  if (n < 0) return would_block(last_error()) ? 0 : -1;
  if (n == 0) return 0;
  return (p.revents & kPollErr) ? -1 : 1;
}

static bool tls_handshake(Stream& s, SSL* ssl, bool server, int timeout_ms,
                          int (*wait)(Stream&, short, int)) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    int r = server ? SSL_accept(ssl) : SSL_connect(ssl);
    if (r == 1) return true;
    int e = SSL_get_error(ssl, r);
    short ev;
    if (e == SSL_ERROR_WANT_READ) ev = kPollIn;
    else if (e == SSL_ERROR_WANT_WRITE) ev = kPollOut;
    else {
      ERR_clear_error();
      return false;
    }
    int left = ms_left(deadline);
    if (left == 0 || wait(s, ev, left) != 1) {
      ERR_clear_error();
      return false;
    }
  }
}

bool Stream::start_tls_server(TlsContext& ctx, int timeout_ms) {
  SSL* ssl = SSL_new(static_cast<SSL_CTX*>(ctx.handle()));
  if (!ssl) return false;
  SSL_set_fd(ssl, static_cast<int>(fd_.load()));
  ssl_ = ssl;
  return tls_handshake(*this, ssl, true, timeout_ms, [](Stream& s, short ev, int t) { return s.io_wait(ev, t); });
}

bool Stream::start_tls_client(TlsContext& ctx, int timeout_ms) {
  SSL* ssl = SSL_new(static_cast<SSL_CTX*>(ctx.handle()));
  if (!ssl) return false;
  SSL_set_fd(ssl, static_cast<int>(fd_.load()));
  ssl_ = ssl;
  return tls_handshake(*this, ssl, false, timeout_ms, [](Stream& s, short ev, int t) { return s.io_wait(ev, t); });
}

std::string Stream::tls_version() const { return ssl_ ? SSL_get_version(static_cast<SSL*>(ssl_)) : ""; }

std::string Stream::peer_cert_fingerprint() const {
  if (!ssl_) return {};
  X509* x = SSL_get1_peer_certificate(static_cast<SSL*>(ssl_));
  if (!x) return {};
  unsigned char der[8192];
  unsigned char* p = der;
  int n = i2d_X509(x, nullptr);
  std::string out;
  if (n > 0 && n <= static_cast<int>(sizeof der) && i2d_X509(x, &p) == n)
    out = sha256_hex(std::string_view(reinterpret_cast<char*>(der), static_cast<std::size_t>(n)));
  X509_free(x);
  return out;
}

int Stream::wait_readable(Fd wake, int timeout_ms) {
  if (ssl_ && SSL_pending(static_cast<SSL*>(ssl_)) > 0) return 1;
  PollItem p[2] = {{fd_.load(), kPollIn, 0}, {wake, kPollIn, 0}};
  int n = poll_items(p, wake == kBadFd ? 1 : 2, timeout_ms);
  if (n < 0) return would_block(last_error()) ? 0 : -1;
  if (n == 0) return 0;
  if (p[0].revents & (kPollIn | kPollHup | kPollErr)) return 1;
  if (wake != kBadFd && (p[1].revents & kPollIn)) return 2;
  return 0;
}

long Stream::read(void* buf, std::size_t n) {
  if (ssl_) {
    SSL* ssl = static_cast<SSL*>(ssl_);
    int r = SSL_read(ssl, buf, static_cast<int>(n));
    if (r > 0) return r;
    int e = SSL_get_error(ssl, r);
    ERR_clear_error();
    return (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) ? 0 : -1;
  }
  long r = sock_recv(fd_.load(), buf, n);
  if (r > 0) return r;
  if (r < 0 && would_block(last_error())) return 0;
  return -1;
}

bool Stream::write_all(const void* data, std::size_t n, int timeout_ms) {
  const char* p = static_cast<const char*>(data);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (n > 0) {
    short ev = kPollOut;
    if (ssl_) {
      SSL* ssl = static_cast<SSL*>(ssl_);
      int r = SSL_write(ssl, p, static_cast<int>(n > 1 << 20 ? 1 << 20 : n));
      if (r > 0) {
        p += r;
        n -= static_cast<std::size_t>(r);
        continue;
      }
      int e = SSL_get_error(ssl, r);
      ERR_clear_error();
      if (e == SSL_ERROR_WANT_READ) ev = kPollIn;
      else if (e != SSL_ERROR_WANT_WRITE) return false;
    } else {
      long r = sock_send(fd_.load(), p, n);
      if (r > 0) {
        p += r;
        n -= static_cast<std::size_t>(r);
        continue;
      }
      if (r < 0 && !would_block(last_error())) return false;
    }
    int left = ms_left(deadline);
    if (left == 0 || io_wait(ev, left) < 0) return false;
    if (ms_left(deadline) == 0) return false;
  }
  return true;
}

std::string ws_accept_key(const std::string& key) {
  static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::string in = key + kGuid;
  unsigned char md[20];
  unsigned int n = 0;
  if (EVP_Digest(in.data(), in.size(), md, &n, EVP_sha1(), nullptr) != 1) return {};
  unsigned char out[64];
  int len = EVP_EncodeBlock(out, md, static_cast<int>(n));
  return std::string(reinterpret_cast<char*>(out), static_cast<std::size_t>(len));
}

}  // namespace deckhand::api::net

// ---- test client ------------------------------------------------------------------------------

namespace deckhand::api::testing {
namespace {
int ms_left(std::chrono::steady_clock::time_point d) {
  auto v = std::chrono::duration_cast<std::chrono::milliseconds>(d - std::chrono::steady_clock::now()).count();
  return v < 0 ? 0 : static_cast<int>(v);
}
}  // namespace

struct TestClient::Impl {
  std::unique_ptr<net::Stream> stream;
  std::unique_ptr<net::TlsContext> ctx;
};

TestClient::TestClient() : impl_(new Impl) {}
TestClient::~TestClient() = default;

bool TestClient::connect(const std::string& host, std::uint16_t port, bool tls, std::string* error, int max_ver) {
  close();
  net::Fd fd = net::connect_tcp(host, port, error);
  if (fd == net::kBadFd) return false;
  impl_->stream = std::make_unique<net::Stream>(fd);
  if (tls) {
    impl_->ctx = net::TlsContext::client(max_ver, error);
    if (!impl_->ctx || !impl_->stream->start_tls_client(*impl_->ctx, 5000)) {
      if (error && error->empty()) *error = "TLS handshake failed";
      impl_->stream.reset();
      return false;
    }
  }
  return true;
}

bool TestClient::send(const std::string& d, int timeout_ms) {
  return impl_->stream && impl_->stream->write_all(d.data(), d.size(), timeout_ms);
}

long TestClient::recv(char* buf, std::size_t n, int timeout_ms) {
  if (!impl_->stream) return -1;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    long r = impl_->stream->read(buf, n);
    if (r != 0) return r;
    int left = ms_left(deadline);
    if (left == 0) return 0;
    int w = impl_->stream->wait_readable(net::kBadFd, left);
    if (w < 0) return -1;
  }
}

void TestClient::close() { impl_->stream.reset(); }
bool TestClient::connected() const { return impl_->stream != nullptr; }
std::string TestClient::tls_version() const { return impl_->stream ? impl_->stream->tls_version() : ""; }
std::string TestClient::peer_cert_fingerprint() const {
  return impl_->stream ? impl_->stream->peer_cert_fingerprint() : "";
}

}  // namespace deckhand::api::testing
