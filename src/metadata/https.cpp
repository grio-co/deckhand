// HTTPS GET over OpenSSL: certificate chain and host name verified against the system trust store, timeouts, at most three
// redirects (https, same host), response size cap. Built only with a TLS library; otherwise a transport that always fails.
#include "deckhand/metadata/metadata.hpp"
#include "http_parse.hpp"

#if DECKHAND_METADATA_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "../api/sockport.hpp"
#if defined(_WIN32)
#include <wincrypt.h>
#endif
#endif

namespace deckhand::metadata {

#if DECKHAND_METADATA_TLS
namespace {
namespace net = deckhand::api::net;
using Clk = std::chrono::steady_clock;

int ms_left(Clk::time_point deadline) {
  const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clk::now()).count();
  return d > 0 ? static_cast<int>(d) : 0;
}

bool file_exists(const std::string& p) { return std::ifstream(p).good(); }

bool load_trust(SSL_CTX* ctx, const std::string& ca_file) {
  if (!ca_file.empty()) return SSL_CTX_load_verify_locations(ctx, ca_file.c_str(), nullptr) == 1;
  bool ok = SSL_CTX_set_default_verify_paths(ctx) == 1;
#if defined(_WIN32)
  if (HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT")) {
    X509_STORE* xs = SSL_CTX_get_cert_store(ctx);
    for (PCCERT_CONTEXT c = CertEnumCertificatesInStore(store, nullptr); c; c = CertEnumCertificatesInStore(store, c)) {
      const unsigned char* p = c->pbCertEncoded;
      if (X509* x = d2i_X509(nullptr, &p, static_cast<long>(c->cbCertEncoded))) {
        if (X509_STORE_add_cert(xs, x) == 1) ok = true;
        X509_free(x);
      }
    }
    CertCloseStore(store, 0);
  }
#else
  for (const char* f : {"/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt"})
    if (file_exists(f) && SSL_CTX_load_verify_locations(ctx, f, nullptr) == 1) ok = true;
#endif
  return ok;
}

struct Conn {
  net::Fd fd = net::kBadFd;
  SSL* ssl = nullptr;
  ~Conn() {
    if (ssl) SSL_free(ssl);
    if (fd != net::kBadFd) net::close_socket(fd);
  }
};

bool wait_fd(net::Fd fd, short ev, Clk::time_point deadline) {
  net::PollItem p[1] = {{fd, ev, 0}};
  const int left = ms_left(deadline);
  if (left == 0) return false;
  return net::poll_items(p, 1, left) > 0;
}

bool connect_to(const http::Url& u, Clk::time_point deadline, Conn& c, std::string* err) {
  net::ensure_sockets();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res) {
    *err = "could not resolve the host";
    return false;
  }
  for (addrinfo* a = res; a; a = a->ai_next) {
    const net::Fd fd = static_cast<net::Fd>(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
    if (fd == net::kBadFd || fd < 0) continue;
    net::set_nonblocking(fd);
    int rc = ::connect(net::raw(fd), a->ai_addr, static_cast<net::SockLen>(a->ai_addrlen));
    bool ok = rc == 0;
    if (!ok && !net::would_block(net::last_error())) {
#if !defined(_WIN32)
      if (net::last_error() != EINPROGRESS)
#endif
      { net::close_socket(fd); continue; }
    }
    if (!ok && wait_fd(fd, net::kPollOut, deadline)) {
      int so = 0;
      net::SockLen sl = sizeof so;
      getsockopt(net::raw(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &sl);
      ok = so == 0;
    }
    if (ok) {
      c.fd = fd;
      freeaddrinfo(res);
      return true;
    }
    net::close_socket(fd);
  }
  freeaddrinfo(res);
  *err = "could not connect";
  return false;
}

// One request on one connection. Returns false with *err on transport failure.
bool fetch_once(SSL_CTX* ctx, const http::Url& u, const std::string& ua, Clk::time_point deadline, std::size_t cap,
                http::Head* head, std::string* body, std::string* err) {
  Conn c;
  if (!connect_to(u, deadline, c, err)) return false;
  c.ssl = SSL_new(ctx);
  if (!c.ssl) {
    *err = "TLS setup failed";
    return false;
  }
  SSL_set_fd(c.ssl, static_cast<int>(c.fd));
  SSL_set_tlsext_host_name(c.ssl, u.host.c_str());
  SSL_set1_host(c.ssl, u.host.c_str());  // verified together with the chain (SSL_VERIFY_PEER)
  for (;;) {
    const int r = SSL_connect(c.ssl);
    if (r == 1) break;
    const int e = SSL_get_error(c.ssl, r);
    if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) ||
        !wait_fd(c.fd, e == SSL_ERROR_WANT_READ ? net::kPollIn : net::kPollOut, deadline)) {
      const long v = SSL_get_verify_result(c.ssl);
      *err = v != X509_V_OK ? std::string("certificate not trusted: ") + X509_verify_cert_error_string(v)
                            : "TLS handshake failed or timed out";
      ERR_clear_error();
      return false;
    }
  }
  if (SSL_get_verify_result(c.ssl) != X509_V_OK) {
    *err = "certificate not trusted";
    return false;
  }
  const std::string req = http::build_request(u, ua);
  for (std::size_t sent = 0; sent < req.size();) {
    const int w = SSL_write(c.ssl, req.data() + sent, static_cast<int>(req.size() - sent));
    if (w > 0) {
      sent += static_cast<std::size_t>(w);
      continue;
    }
    const int e = SSL_get_error(c.ssl, w);
    if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) ||
        !wait_fd(c.fd, e == SSL_ERROR_WANT_READ ? net::kPollIn : net::kPollOut, deadline)) {
      ERR_clear_error();
      *err = "send failed or timed out";
      return false;
    }
  }
  std::string raw;
  bool eof = false;
  while (!eof) {
    char buf[8192];
    const int n = SSL_read(c.ssl, buf, sizeof buf);
    if (n > 0) {
      raw.append(buf, static_cast<std::size_t>(n));
      if (raw.size() > cap + 16384) {
        *err = "response too large";
        return false;
      }
      continue;
    }
    const int e = SSL_get_error(c.ssl, n);
    if (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL || e == SSL_ERROR_SSL) {
      eof = true;  // Connection: close ends the body (a truncated chunked body is detected below)
      ERR_clear_error();
    } else if (!wait_fd(c.fd, e == SSL_ERROR_WANT_READ ? net::kPollIn : net::kPollOut, deadline)) {
      *err = "timed out";
      return false;
    }
  }
  const std::size_t split = raw.find("\r\n\r\n");
  if (split == std::string::npos || !http::parse_head(raw.substr(0, split), head)) {
    *err = "malformed response";
    return false;
  }
  std::string payload = raw.substr(split + 4);
  const auto te = head->headers.find("transfer-encoding");
  if (te != head->headers.end() && te->second.find("chunked") != std::string::npos) {
    bool complete = false;
    if (!http::decode_chunked(payload, body, cap, &complete) || !complete) {
      *err = "malformed or truncated response body";
      return false;
    }
  } else {
    const auto cl = head->headers.find("content-length");
    if (cl != head->headers.end() && payload.size() < static_cast<std::size_t>(std::strtoull(cl->second.c_str(), nullptr, 10))) {
      *err = "truncated response";
      return false;
    }
    if (payload.size() > cap) {
      *err = "response too large";
      return false;
    }
    *body = std::move(payload);
  }
  return true;
}

class HttpsTransport : public Transport {
 public:
  explicit HttpsTransport(std::string ca) : ca_(std::move(ca)) {}
  HttpResponse get(const HttpRequest& rq) override {
    HttpResponse out;
    // Kill switch for test runs: no socket is ever opened while this variable is set (CMake sets it for every ctest test).
    if (const char* off = std::getenv("DECKHAND_NO_NETWORK"); off && *off && *off != '0') {
      out.error = "network disabled (DECKHAND_NO_NETWORK)";
      return out;
    }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
      out.error = "TLS setup failed";
      return out;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    if (!load_trust(ctx, ca_)) {
      SSL_CTX_free(ctx);
      out.error = "no system certificate store found";
      return out;
    }
    const auto deadline = Clk::now() + std::chrono::milliseconds(rq.timeout_ms);
    std::string url = rq.url;
    for (int hop = 0; hop <= 3; ++hop) {
      http::Url u;
      if (!http::parse_https_url(url, &u)) {
        out.error = "only https URLs are allowed";
        break;
      }
      http::Head head;
      std::string body, err;
      if (!fetch_once(ctx, u, rq.user_agent, deadline, rq.max_bytes, &head, &body, &err)) {
        out.error = err;
        break;
      }
      if (head.status >= 301 && head.status <= 308 && head.status != 304 && head.status != 306) {
        const auto loc = head.headers.find("location");
        const std::string next = loc == head.headers.end() ? std::string() : http::resolve_redirect(u, loc->second);
        if (next.empty() || hop == 3) {
          out.error = "redirect refused";
          break;
        }
        url = next;
        continue;
      }
      out.status = head.status;
      out.body = std::move(body);
      const auto ra = head.headers.find("retry-after");
      if (ra != head.headers.end()) out.retry_after = ra->second;
      break;
    }
    SSL_CTX_free(ctx);
    return out;
  }

 private:
  std::string ca_;
};
}  // namespace
bool network_built() { return true; }
std::shared_ptr<Transport> make_https_transport(const std::string& ca_file) { return std::make_shared<HttpsTransport>(ca_file); }

#else  // stub

namespace {
class NoTransport : public Transport {
 public:
  HttpResponse get(const HttpRequest&) override {
    HttpResponse r;
    r.error = "this build has no network lookup";
    return r;
  }
};
}  // namespace
bool network_built() { return false; }
std::shared_ptr<Transport> make_https_transport(const std::string&) { return std::make_shared<NoTransport>(); }
#endif

}  // namespace deckhand::metadata
