#pragma once
// Embedded HTTP/1.1 + WebSocket server: transport and security only. The application registers
// routes and publishes events; the server knows nothing about decks.
//
// Threading contract
//   * start() spawns one accept thread plus one thread per open connection (bounded by
//     ApiConfig::max_connections). stop() (also called by the destructor) joins them all.
//   * Route and WebSocket-command handlers run on those connection threads, possibly many at
//     once, so they must be thread-safe and should return quickly. They must not call stop().
//     A handler that blocks delays shutdown.
//   * add_route/add_ws_command, publish(), ws_client_count() are callable from any thread.
//   * publish() never blocks on a network peer: it appends to a bounded per-client queue and
//     wakes the client's thread. A client whose queue overflows is disconnected (close 1013).
//   * Nothing here throws across the public API.
//
// Security model: every route requires a bearer token with the route's scope unless the route
// is registered with an empty scope (public). Tokens are never accepted in URLs. See auth.hpp
// for scope matching. Plain HTTP is only allowed on loopback; start() refuses to bind another
// address without TLS (TLS 1.2 minimum).
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "deckhand/api/auth.hpp"
#include "deckhand/api/json.hpp"

namespace deckhand::api {

struct Principal {
  std::string token_id;
  std::string name;
  std::vector<std::string> scopes;
  std::string remote_ip;
  bool allows(std::string_view scope) const { return scopes_allow(scopes, scope); }
};

struct Request {
  std::string method;                        // upper case
  std::string path;                          // decoded, without query
  std::map<std::string, std::string> params; // {name} captures from the route pattern (decoded)
  std::map<std::string, std::string> query;  // decoded; last duplicate wins
  Json body;                                 // Null when the request had no body
  Principal principal;                       // empty for public routes
};

struct Response {
  int status = 200;
  Json body;  // Null => empty body (always for 204)
  std::vector<std::pair<std::string, std::string>> headers;  // extra response headers
  static Response ok(Json b = Json()) { Response r; r.body = std::move(b); return r; }
  static Response error(int status, const std::string& message);
};

using Handler = std::function<Response(const Request&)>;

// WebSocket client message: {"id": <any>, "cmd": "name", "args": {...}}. The reply is
// {"id", "ok", "status", "result"|"error"}. Built-in commands: subscribe/unsubscribe
// ({"args":{"topics":[...]}}) and ping.
struct WsRequest {
  std::string command;
  Json args;
  Principal principal;
  std::uint64_t connection_id = 0;
};
using WsHandler = std::function<Response(const WsRequest&)>;

struct ApiConfig {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 0;                  // 0 = ephemeral (see ApiServer::port())
  std::string tls_cert, tls_key;           // PEM paths; both set = TLS on
  std::vector<std::string> allowed_origins;  // CORS and WebSocket Origin allowlist; empty = none
  std::string ws_path = "/v1/ws";

  std::size_t max_body_bytes = 1 << 20;
  std::size_t max_header_bytes = 16 * 1024;
  std::size_t max_connections = 128;
  std::size_t max_connections_per_ip = 32;
  int header_timeout_ms = 10000;           // from first byte of a request to end of headers
  int body_timeout_ms = 10000;
  int idle_timeout_ms = 30000;             // keep-alive connection with no request
  int write_timeout_ms = 10000;
  int tls_handshake_timeout_ms = 5000;
  std::size_t max_requests_per_connection = 1000;

  std::size_t ws_max_message_bytes = 256 * 1024;
  std::size_t ws_max_clients = 64;
  std::size_t ws_max_subscriptions = 256;
  std::size_t ws_queue_messages = 512;     // per client, before it is dropped
  std::size_t ws_queue_bytes = 4 << 20;
  int ws_ping_interval_ms = 15000;
  int ws_idle_timeout_ms = 45000;          // no frame at all from the client

  double token_rate_per_sec = 100;         // requests (and WS commands); <= 0 disables
  double token_burst = 200;
  double ip_rate_per_sec = 200;
  double ip_burst = 400;
  int lockout_failures = 10;               // failed auth attempts per window before lockout
  double lockout_window_sec = 60;
  double lockout_duration_sec = 60;
};

class ApiServer {
 public:
  // `tokens` must outlive the server.
  ApiServer(TokenStore& tokens, ApiConfig config);
  ~ApiServer();
  ApiServer(const ApiServer&) = delete;
  ApiServer& operator=(const ApiServer&) = delete;

  // Pattern "/v1/decks/{deck}/play": literal segments and {name} captures (one whole segment).
  // required_scope may reference captures ("control:deck:{deck}"); a capture used in a scope
  // must consist of [a-z0-9_.-] or the request is rejected with 400. Empty scope = public
  // (no authentication). Methods: GET POST PUT PATCH DELETE. Literal segments win over
  // captures. False (and *error) for invalid arguments or a duplicate method+pattern.
  bool add_route(const std::string& method, const std::string& pattern, const std::string& required_scope,
                 Handler handler, std::string* error = nullptr);
  // WebSocket command; scope templates may reference top-level scalar fields of "args".
  bool add_ws_command(const std::string& name, const std::string& required_scope, WsHandler handler,
                      std::string* error = nullptr);

  bool start(std::string* error = nullptr);
  void stop();
  bool running() const;
  std::uint16_t port() const;  // actual bound port after start()
  bool tls_enabled() const;

  // Delivers {"topic":..., "data":...} to WebSocket clients subscribed to `topic` whose token
  // grants `required_scope`. Subscriptions: exact topic, "prefix.*", or "*".
  void publish(const std::string& topic, const Json& data, const std::string& required_scope = "read");
  std::size_t ws_client_count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Creates a self-signed EC P-256 certificate valid for 10 years with SANs for localhost,
// 127.0.0.1, ::1 and `hostnames` (DNS names or IP literals). The key file gets mode 0600.
bool generate_self_signed(const std::string& cert_path, const std::string& key_path,
                          const std::vector<std::string>& hostnames, std::string* error = nullptr);
// SHA-256 of the certificate's DER encoding, lower-case hex (for trust-on-first-use pinning).
bool certificate_fingerprint(const std::string& cert_path, std::string* fingerprint, std::string* error = nullptr);

}  // namespace deckhand::api
