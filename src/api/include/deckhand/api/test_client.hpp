#pragma once
// Test-only blocking client (plain TCP or TLS without certificate verification) so tests do
// not need OpenSSL headers. Not for production use.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace deckhand::api::testing {

class TestClient {
 public:
  TestClient();
  ~TestClient();
  TestClient(const TestClient&) = delete;
  TestClient& operator=(const TestClient&) = delete;

  // tls_max_version: 0 = library default, else an OpenSSL version constant (e.g. 0x0302 = TLS 1.1).
  bool connect(const std::string& host, std::uint16_t port, bool tls, std::string* error = nullptr,
               int tls_max_version = 0);
  bool send(const std::string& data, int timeout_ms = 5000);
  // >0 bytes read, 0 = timeout, -1 = connection closed/failed.
  long recv(char* buf, std::size_t n, int timeout_ms);
  void close();
  bool connected() const;
  std::string tls_version() const;
  std::string peer_cert_fingerprint() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deckhand::api::testing
