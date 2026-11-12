#include "deckhand/api/server.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include "deckhand/log/log.hpp"
#include "net.hpp"

namespace deckhand::api {

Response Response::error(int status, const std::string& message) {
  Response r;
  r.status = status;
  Json e = Json::object();
  e.set("code", status);
  e.set("message", message);
  r.body = Json::object();
  r.body.set("error", std::move(e));
  return r;
}

namespace {

const char* reason(int s) {
  switch (s) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 422: return "Unprocessable Entity";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return s < 400 ? "OK" : "Error";
  }
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return std::string(s);
}

bool is_token_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

int hex_val(char h) {
  if (h >= '0' && h <= '9') return h - '0';
  if (h >= 'a' && h <= 'f') return h - 'a' + 10;
  if (h >= 'A' && h <= 'F') return h - 'A' + 10;
  return -1;
}

bool percent_decode(std::string_view in, bool plus_is_space, std::string* out) {
  out->clear();
  for (std::size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '%') {
      if (i + 3 > in.size()) return false;
      int a = hex_val(in[i + 1]), b = hex_val(in[i + 2]);
      if (a < 0 || b < 0 || (a == 0 && b == 0)) return false;
      *out += static_cast<char>(a * 16 + b);
      i += 2;
    } else if (c == '+' && plus_is_space) {
      *out += ' ';
    } else {
      *out += c;
    }
  }
  return true;
}

std::vector<std::string> split_path(std::string_view p) {
  std::vector<std::string> segs;
  if (p.empty() || p[0] != '/') return segs;
  p.remove_prefix(1);
  std::size_t start = 0;
  for (;;) {
    auto e = p.find('/', start);
    if (e == std::string_view::npos) { segs.emplace_back(p.substr(start)); break; }
    segs.emplace_back(p.substr(start, e - start));
    start = e + 1;
  }
  return segs;
}

bool param_safe_for_scope(const std::string& v) {
  if (v.empty() || v.size() > 64) return false;
  for (char c : v)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
  return true;
}

// Replaces {name} in a scope template. False if a value is unusable inside a scope.
bool expand_scope(const std::string& tmpl, const std::map<std::string, std::string>& vals, std::string* out) {
  out->clear();
  for (std::size_t i = 0; i < tmpl.size(); ++i) {
    if (tmpl[i] != '{') { *out += tmpl[i]; continue; }
    auto e = tmpl.find('}', i);
    if (e == std::string::npos) return false;
    auto it = vals.find(tmpl.substr(i + 1, e - i - 1));
    if (it == vals.end() || !param_safe_for_scope(it->second)) return false;
    *out += it->second;
    i = e;
  }
  return valid_scope(*out);
}

std::string frame_text(const std::string& payload) {
  std::string f;
  f += static_cast<char>(0x81);
  if (payload.size() < 126) {
    f += static_cast<char>(payload.size());
  } else if (payload.size() <= 0xFFFF) {
    f += static_cast<char>(126);
    f += static_cast<char>(payload.size() >> 8);
    f += static_cast<char>(payload.size() & 0xFF);
  } else {
    f += static_cast<char>(127);
    for (int i = 7; i >= 0; --i) f += static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (8 * i)) & 0xFF);
  }
  f += payload;
  return f;
}

std::string frame_control(int op, const std::string& payload) {
  std::string f;
  f += static_cast<char>(0x80 | op);
  f += static_cast<char>(payload.size() & 0x7F);
  f += payload;
  return f;
}

std::string close_payload(int code, const std::string& why) {
  std::string p;
  p += static_cast<char>(code >> 8);
  p += static_cast<char>(code & 0xFF);
  p += why.substr(0, 100);
  return p;
}

bool topic_valid(const std::string& t) {
  if (t.empty() || t.size() > 128) return false;
  for (char c : t)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-' || c == ':' || c == '/' || c == '*'))
      return false;
  return true;
}

bool topic_matches(const std::string& pattern, const std::string& topic) {
  if (pattern == "*" || pattern == topic) return true;
  if (pattern.size() >= 2 && pattern.compare(pattern.size() - 2, 2, ".*") == 0)
    return topic.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;
  return false;
}

struct Route {
  std::string method;
  std::vector<std::pair<bool, std::string>> segs;  // (is_capture, literal or capture name)
  std::string scope;
  Handler handler;
  std::size_t literals = 0;
};

struct Head {
  std::string method, target, version;
  std::map<std::string, std::string> headers;  // lower-case names
  std::string header(const std::string& n) const {
    auto it = headers.find(n);
    return it == headers.end() ? std::string() : it->second;
  }
};

bool has_token(const std::string& list, const std::string& want) {
  std::size_t i = 0;
  while (i <= list.size()) {
    auto e = list.find(',', i);
    if (e == std::string::npos) e = list.size();
    if (lower(trim(std::string_view(list).substr(i, e - i))) == want) return true;
    i = e + 1;
  }
  return false;
}

}  // namespace

struct ApiServer::Impl {
  struct WsClient {
    std::uint64_t id = 0;
    Principal principal;
    net::Wake wake;
    std::mutex mu;
    std::deque<std::shared_ptr<const std::string>> q;
    std::size_t q_bytes = 0;
    std::vector<std::string> subs;
    bool dropped = false;
  };
  struct Slot {
    std::shared_ptr<net::Stream> stream;
    std::thread th;
    std::atomic<bool> done{false};
    std::string ip;
  };

  Impl(TokenStore& t, ApiConfig c)
      : tokens(t), cfg(std::move(c)), token_limiter(cfg.token_rate_per_sec, cfg.token_burst),
        ip_limiter(cfg.ip_rate_per_sec, cfg.ip_burst),
        failures(cfg.lockout_failures, cfg.lockout_window_sec, cfg.lockout_duration_sec) {}

  TokenStore& tokens;
  ApiConfig cfg;
  RateLimiter token_limiter, ip_limiter;
  FailureTracker failures;

  std::shared_mutex routes_mu;
  std::vector<Route> routes;
  std::map<std::string, std::pair<std::string, WsHandler>> ws_commands;

  net::Fd listen_fd = net::kBadFd;
  net::Wake accept_wake;
  std::uint16_t bound_port = 0;
  std::unique_ptr<net::TlsContext> tls;
  std::atomic<bool> running{false};
  std::atomic<bool> stopping{false};
  std::thread accept_thread;
  std::mutex slots_mu;
  std::list<std::shared_ptr<Slot>> slots;

  mutable std::mutex clients_mu;
  std::vector<std::shared_ptr<WsClient>> clients;
  std::atomic<std::uint64_t> next_client_id{1};

  // ---- reading ---------------------------------------------------------------------------

  // 1 = data appended, 0 = timeout, 2 = woken, -1 = closed/error/stopping.
  int fill(net::Stream& s, std::string& buf, net::Fd wake, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      if (stopping) return -1;
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (left < 0) left = 0;
      int w = s.wait_readable(wake, static_cast<int>(std::min<long long>(left, 200)));
      if (w < 0) return -1;
      if (w == 2) return 2;
      if (w == 1) {
        char tmp[16384];
        long r = s.read(tmp, sizeof tmp);
        if (r < 0) return -1;
        if (r > 0) {
          buf.append(tmp, static_cast<std::size_t>(r));
          return 1;
        }
      }
      if (left == 0) return 0;
    }
  }

  // ---- responses -------------------------------------------------------------------------

  bool send_response(net::Stream& s, const Response& r, bool keep_alive,
                     const std::vector<std::pair<std::string, std::string>>& extra) {
    std::string body = (r.status == 204 || r.body.is_null()) ? std::string() : r.body.dump();
    std::string h = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    if (r.status != 204) h += "Content-Type: application/json; charset=utf-8\r\n";
    h += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    h += std::string("Connection: ") + (keep_alive ? "keep-alive" : "close") + "\r\n";
    h += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    for (const auto& kv : extra) h += kv.first + ": " + kv.second + "\r\n";
    for (const auto& kv : r.headers) h += kv.first + ": " + kv.second + "\r\n";
    h += "\r\n";
    h += body;
    return s.write_all(h.data(), h.size(), cfg.write_timeout_ms);
  }

  // ---- routing ---------------------------------------------------------------------------

  struct Match {
    const Route* route = nullptr;
    std::map<std::string, std::string> params;
    bool path_known = false;  // some route matches the path with another method
    std::string allow;
    bool bad_encoding = false;
  };

  Match match_route(const std::string& method, const std::vector<std::string>& segs) {
    Match m;
    std::shared_lock<std::shared_mutex> lk(routes_mu);
    std::vector<std::string> decoded;
    for (const auto& s : segs) {
      std::string d;
      if (!percent_decode(s, false, &d)) { m.bad_encoding = true; return m; }
      decoded.push_back(std::move(d));
    }
    for (const auto& r : routes) {
      if (r.segs.size() != decoded.size()) continue;
      bool ok = true;
      for (std::size_t i = 0; i < decoded.size() && ok; ++i) {
        if (decoded[i].empty()) ok = false;
        else if (!r.segs[i].first) ok = r.segs[i].second == decoded[i];
      }
      if (!ok) continue;
      m.path_known = true;
      if (m.allow.find(r.method) == std::string::npos) m.allow += (m.allow.empty() ? "" : ", ") + r.method;
      if (r.method != method) continue;
      if (!m.route || r.literals > m.route->literals) {
        m.route = &r;
        m.params.clear();
        for (std::size_t i = 0; i < decoded.size(); ++i)
          if (r.segs[i].first) m.params[r.segs[i].second] = decoded[i];
      }
    }
    return m;
  }

  // ---- auth ------------------------------------------------------------------------------

  struct AuthOutcome {
    bool ok = false;
    Principal principal;
    Response error;
  };

  AuthOutcome authenticate(const std::string& secret, const std::string& ip) {
    AuthOutcome o;
    const double now = monotonic_seconds();
    auto lock_response = [&](double retry) {
      Response r = Response::error(429, "too many failed authentication attempts");
      r.headers.emplace_back("Retry-After", std::to_string(static_cast<int>(retry) + 1));
      return r;
    };
    double retry = 0;
    if (failures.locked(ip, now, &retry)) {
      o.error = lock_response(retry);
      return o;
    }
    if (secret.empty()) {
      o.error = Response::error(401, "missing bearer token");
      o.error.headers.emplace_back("WWW-Authenticate", "Bearer");
      return o;
    }
    TokenInfo info;
    auto res = tokens.authenticate(secret, ip, &info);
    if (res != TokenStore::Result::Ok) {
      failures.failure(ip, now);
      if (res == TokenStore::Result::SourceDenied) {
        o.error = Response::error(403, "token not valid from this address");
      } else {
        o.error = Response::error(401, res == TokenStore::Result::Expired ? "token expired" : "invalid token");
        o.error.headers.emplace_back("WWW-Authenticate", "Bearer error=\"invalid_token\"");
      }
      return o;
    }
    failures.success(ip);
    o.ok = true;
    o.principal.token_id = info.id;
    o.principal.name = info.name;
    o.principal.scopes = info.scopes;
    o.principal.remote_ip = ip;
    return o;
  }

  bool origin_allowed(const std::string& origin) const {
    return std::find(cfg.allowed_origins.begin(), cfg.allowed_origins.end(), origin) != cfg.allowed_origins.end();
  }

  // ---- HTTP connection loop --------------------------------------------------------------

  // 1 = head parsed, 0 = close silently, -1 = error response needed.
  int read_head(net::Stream& s, std::string& buf, std::string& head, int* st, std::string* msg, bool first) {
    auto start = std::chrono::steady_clock::now();
    bool started = !buf.empty();
    for (;;) {
      while (buf.size() >= 2 && buf[0] == '\r' && buf[1] == '\n') buf.erase(0, 2);
      auto pos = buf.find("\r\n\r\n");
      if (pos != std::string::npos) {
        if (pos + 4 > cfg.max_header_bytes) { *st = 431; *msg = "headers too large"; return -1; }
        head = buf.substr(0, pos + 2);
        buf.erase(0, pos + 4);
        return 1;
      }
      if (buf.size() > cfg.max_header_bytes) { *st = 431; *msg = "headers too large"; return -1; }
      int budget;
      if (started) {
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        budget = cfg.header_timeout_ms - static_cast<int>(el);
        if (budget <= 0) { *st = 408; *msg = "request timeout"; return -1; }
      } else {
        budget = first ? cfg.header_timeout_ms : cfg.idle_timeout_ms;
      }
      int r = fill(s, buf, net::kBadFd, budget);
      if (r < 0) return 0;
      if (r == 0) {
        if (started) { *st = 408; *msg = "request timeout"; return -1; }
        return 0;
      }
      if (!started) {
        started = true;
        start = std::chrono::steady_clock::now();
      }
    }
  }

  bool parse_head(const std::string& head, Head* h, int* st, std::string* msg) {
    auto bad = [&](const char* m) {
      *st = 400;
      *msg = m;
      return false;
    };
    std::size_t pos = 0;
    bool first = true;
    while (pos < head.size()) {
      auto e = head.find("\r\n", pos);
      if (e == std::string::npos) return bad("malformed request");
      std::string line = head.substr(pos, e - pos);
      pos = e + 2;
      if (first) {
        first = false;
        auto a = line.find(' ');
        auto b = a == std::string::npos ? a : line.find(' ', a + 1);
        if (b == std::string::npos) return bad("malformed request line");
        h->method = line.substr(0, a);
        h->target = line.substr(a + 1, b - a - 1);
        h->version = line.substr(b + 1);
        if (h->method.empty()) return bad("malformed request line");
        for (char c : h->method)
          if (!is_token_char(c)) return bad("malformed method");
        if (h->version != "HTTP/1.1" && h->version != "HTTP/1.0") return bad("unsupported HTTP version");
        if (h->target.size() > 4096) { *st = 414; *msg = "URI too long"; return false; }
        if (h->target.empty() || h->target[0] != '/') return bad("request target must be an absolute path");
        for (unsigned char c : h->target)
          if (c <= 0x20 || c == 0x7f) return bad("invalid character in request target");
        continue;
      }
      auto colon = line.find(':');
      if (colon == std::string::npos || colon == 0) return bad("malformed header");
      std::string name = line.substr(0, colon);
      for (char c : name)
        if (!is_token_char(c)) return bad("malformed header name");
      std::string val = trim(std::string_view(line).substr(colon + 1));
      for (unsigned char c : val)
        if ((c < 0x20 && c != '\t') || c == 0x7f) return bad("invalid character in header value");
      name = lower(name);
      auto it = h->headers.find(name);
      if (it != h->headers.end()) {
        if (name == "content-length" || name == "authorization" || name == "host") {
          if (it->second != val || name == "authorization") return bad("duplicate header");
        } else {
          it->second += ", " + val;
        }
      } else {
        h->headers[name] = val;
        if (h->headers.size() > 100) return bad("too many headers");
      }
    }
    return true;
  }

  void serve(const std::shared_ptr<Slot>& slot) {
    net::Stream& s = *slot->stream;
    try {
      if (tls && !s.start_tls_server(*tls, cfg.tls_handshake_timeout_ms)) return;
      std::string buf;
      for (std::size_t n = 0; n < cfg.max_requests_per_connection && !stopping; ++n) {
        std::string raw;
        int st = 400;
        std::string msg;
        int r = read_head(s, buf, raw, &st, &msg, n == 0);
        if (r == 0) return;
        Head h;
        if (r < 0 || !parse_head(raw, &h, &st, &msg)) {
          send_response(s, Response::error(st, msg), false, {});
          return;
        }
        bool keep = h.version == "HTTP/1.1" ? !has_token(h.header("connection"), "close")
                                           : has_token(h.header("connection"), "keep-alive");
        if (!h.header("transfer-encoding").empty()) {
          send_response(s, Response::error(400, "chunked request bodies are not supported"), false, {});
          return;
        }
        std::size_t clen = 0;
        std::string cl = h.header("content-length");
        if (!cl.empty()) {
          if (cl.size() > 15 || cl.find_first_not_of("0123456789") != std::string::npos) {
            send_response(s, Response::error(400, "invalid Content-Length"), false, {});
            return;
          }
          clen = static_cast<std::size_t>(std::stoull(cl));
          if (clen > cfg.max_body_bytes) {
            send_response(s, Response::error(413, "request body too large"), false, {});
            return;
          }
        }
        if (buf.size() < clen && lower(h.header("expect")) == "100-continue") {
          static const char k[] = "HTTP/1.1 100 Continue\r\n\r\n";
          s.write_all(k, sizeof k - 1, cfg.write_timeout_ms);
        }
        auto bstart = std::chrono::steady_clock::now();
        while (buf.size() < clen) {
          auto el = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - bstart).count();
          int budget = cfg.body_timeout_ms - static_cast<int>(el);
          int fr = budget > 0 ? fill(s, buf, net::kBadFd, budget) : 0;
          if (fr < 0) return;
          if (fr == 0) {
            send_response(s, Response::error(408, "request timeout"), false, {});
            return;
          }
        }
        std::string body = buf.substr(0, clen);
        buf.erase(0, clen);
        if (!handle_request(slot, h, body, keep, buf)) return;
        if (!keep) return;
      }
    } catch (const std::exception& e) {
      DH_LOG(Warn, "api: connection error: %s", e.what());
    } catch (...) {
    }
  }

  // Returns false when the connection must close afterwards.
  bool handle_request(const std::shared_ptr<Slot>& slot, const Head& h, const std::string& body, bool& keep,
                      std::string& rbuf) {
    net::Stream& s = *slot->stream;
    const std::string& ip = slot->ip;
    std::vector<std::pair<std::string, std::string>> extra;
    auto reply = [&](Response r) { return send_response(s, r, keep, extra); };

    // Per-IP throttling and lockout come first and cost almost nothing.
    if (!ip_limiter.allow(ip, monotonic_seconds())) {
      Response r = Response::error(429, "rate limit exceeded");
      r.headers.emplace_back("Retry-After", "1");
      return reply(r);
    }
    double retry = 0;
    if (failures.locked(ip, monotonic_seconds(), &retry)) {
      Response r = Response::error(429, "too many failed authentication attempts");
      r.headers.emplace_back("Retry-After", std::to_string(static_cast<int>(retry) + 1));
      return reply(r);
    }

    std::string path_raw = h.target, query_raw;
    auto q = path_raw.find('?');
    if (q != std::string::npos) {
      query_raw = path_raw.substr(q + 1);
      path_raw.resize(q);
    }

    // CORS: nothing is added unless the Origin is on the allowlist.
    const std::string origin = h.header("origin");
    if (!origin.empty() && origin_allowed(origin)) {
      extra.emplace_back("Access-Control-Allow-Origin", origin);
      extra.emplace_back("Vary", "Origin");
    }

    const bool ws_upgrade = has_token(lower(h.header("connection")), "upgrade") &&
                            has_token(lower(h.header("upgrade")), "websocket");
    std::string decoded_path;
    if (!percent_decode(path_raw, false, &decoded_path)) return reply(Response::error(400, "bad percent-encoding"));

    if (h.method == "OPTIONS" && !origin.empty()) {
      if (!origin_allowed(origin)) return reply(Response::error(403, "origin not allowed"));
      Response r;
      r.status = 204;
      extra.emplace_back("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
      extra.emplace_back("Access-Control-Allow-Headers", "Authorization, Content-Type");
      extra.emplace_back("Access-Control-Max-Age", "600");
      return reply(r);
    }

    if (decoded_path == cfg.ws_path && ws_upgrade) {
      keep = false;
      return handle_ws(slot, h, origin, rbuf);
    }

    std::map<std::string, std::string> query;
    {
      std::size_t i = 0;
      while (i <= query_raw.size() && !query_raw.empty()) {
        auto e = query_raw.find('&', i);
        if (e == std::string::npos) e = query_raw.size();
        std::string kv = query_raw.substr(i, e - i);
        if (!kv.empty()) {
          auto eq = kv.find('=');
          std::string k, v;
          if (!percent_decode(kv.substr(0, eq), true, &k) ||
              (eq != std::string::npos && !percent_decode(kv.substr(eq + 1), true, &v)))
            return reply(Response::error(400, "bad percent-encoding in query"));
          query[k] = v;
        }
        i = e + 1;
      }
    }

    auto segs = split_path(path_raw);
    Match m = match_route(h.method, segs);
    if (m.bad_encoding) return reply(Response::error(400, "bad percent-encoding"));

    Principal principal;
    const bool is_public = m.route && m.route->scope.empty();
    if (!is_public) {
      std::string secret;
      std::string auth = h.header("authorization");
      if (auth.size() > 7 && lower(auth.substr(0, 7)) == "bearer ") secret = trim(auth.substr(7));
      else if (!auth.empty()) {
        // Wrong scheme: treat as an invalid credential.
        secret = "?";
      }
      AuthOutcome a = authenticate(secret, ip);
      if (!a.ok) return reply(a.error);
      principal = std::move(a.principal);
      if (!token_limiter.allow(principal.token_id, monotonic_seconds())) {
        Response r = Response::error(429, "rate limit exceeded");
        r.headers.emplace_back("Retry-After", "1");
        return reply(r);
      }
    }
    if (!m.route) {
      if (m.path_known) {
        Response r = Response::error(405, "method not allowed");
        r.headers.emplace_back("Allow", m.allow);
        return reply(r);
      }
      return reply(Response::error(404, "not found"));
    }
    if (!is_public) {
      std::string need;
      if (!expand_scope(m.route->scope, m.params, &need)) return reply(Response::error(400, "invalid path parameter"));
      if (!principal.allows(need)) return reply(Response::error(403, "token lacks required scope: " + need));
    }

    Request req;
    req.method = h.method;
    req.path = decoded_path;
    req.params = std::move(m.params);
    req.query = std::move(query);
    req.principal = std::move(principal);
    if (!body.empty()) {
      std::string err;
      JsonLimits lim;
      lim.max_bytes = cfg.max_body_bytes;
      if (!Json::try_parse(body, &req.body, &err, lim)) return reply(Response::error(400, "invalid JSON body: " + err));
    }
    Response resp;
    try {
      resp = m.route->handler(req);
    } catch (const std::exception& e) {
      DH_LOG(Warn, "api: handler threw: %s", e.what());
      resp = Response::error(500, "internal error");
    } catch (...) {
      resp = Response::error(500, "internal error");
    }
    return reply(std::move(resp));
  }

  // ---- WebSocket -------------------------------------------------------------------------

  bool handle_ws(const std::shared_ptr<Slot>& slot, const Head& h, const std::string& origin, std::string& rbuf) {
    net::Stream& s = *slot->stream;
    auto fail = [&](Response r) {
      send_response(s, r, false, {});
      return false;
    };
    if (h.method != "GET") return fail(Response::error(405, "WebSocket upgrade requires GET"));
    if (!origin.empty() && !origin_allowed(origin)) return fail(Response::error(403, "origin not allowed"));
    if (h.header("sec-websocket-version") != "13") {
      Response r = Response::error(426, "unsupported WebSocket version");
      r.headers.emplace_back("Sec-WebSocket-Version", "13");
      return fail(r);
    }
    std::string key = h.header("sec-websocket-key");
    if (key.size() != 24 || key.compare(22, 2, "==") != 0) return fail(Response::error(400, "bad Sec-WebSocket-Key"));

    std::string secret;
    std::string auth = h.header("authorization");
    std::string chosen_protocol;
    bool offered_v1 = false;
    std::string protocols = h.header("sec-websocket-protocol");
    std::string bearer_proto;
    {
      std::size_t i = 0;
      while (i <= protocols.size() && !protocols.empty()) {
        auto e = protocols.find(',', i);
        if (e == std::string::npos) e = protocols.size();
        std::string p = trim(std::string_view(protocols).substr(i, e - i));
        if (p == "deckhand.v1") offered_v1 = true;
        else if (p.rfind("bearer.", 0) == 0 && bearer_proto.empty()) bearer_proto = p;
        i = e + 1;
      }
    }
    if (auth.size() > 7 && lower(auth.substr(0, 7)) == "bearer ") secret = trim(auth.substr(7));
    else if (!auth.empty()) secret = "?";
    else if (!bearer_proto.empty()) secret = bearer_proto.substr(7);
    chosen_protocol = offered_v1 ? "deckhand.v1" : bearer_proto;
    if (!bearer_proto.empty() && secret != "?" && auth.empty() && !offered_v1) chosen_protocol = bearer_proto;

    AuthOutcome a = authenticate(secret, slot->ip);
    if (!a.ok) return fail(a.error);

    auto client = std::make_shared<WsClient>();
    {
      std::lock_guard<std::mutex> lk(clients_mu);
      if (clients.size() >= cfg.ws_max_clients) return fail(Response::error(503, "too many WebSocket clients"));
      if (!client->wake.open()) return fail(Response::error(500, "internal error"));
      client->id = next_client_id++;
      client->principal = a.principal;
      clients.push_back(client);
    }

    std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " + net::ws_accept_key(key) + "\r\n";
    if (!chosen_protocol.empty()) resp += "Sec-WebSocket-Protocol: " + chosen_protocol + "\r\n";
    resp += "\r\n";
    if (s.write_all(resp.data(), resp.size(), cfg.write_timeout_ms)) ws_loop(s, client, rbuf);
    {
      std::lock_guard<std::mutex> lk(clients_mu);
      clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
    }
    client->wake.close();
    return false;
  }

  void ws_send_close(net::Stream& s, int code, const std::string& why) {
    std::string f = frame_control(8, close_payload(code, why));
    s.write_all(f.data(), f.size(), 1000);
  }

  static bool close_code_ok(int c) {
    return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1011) || (c >= 3000 && c <= 4999);
  }

  void ws_reply(net::Stream& s, const Json& id, const Response& r, bool* ok) {
    Json j = Json::object();
    if (!id.is_null()) j.set("id", id);
    j.set("ok", r.status < 400);
    j.set("status", r.status);
    if (r.status < 400) {
      if (!r.body.is_null()) j.set("result", r.body);
    } else {
      j.set("error", r.body.contains("error") ? r.body["error"] : r.body);
    }
    std::string f = frame_text(j.dump());
    if (!s.write_all(f.data(), f.size(), cfg.write_timeout_ms)) *ok = false;
  }

  // Returns false if the connection must end.
  bool ws_message(net::Stream& s, const std::shared_ptr<WsClient>& c, const std::string& text) {
    bool ok = true;
    JsonLimits lim;
    lim.max_bytes = cfg.ws_max_message_bytes;
    lim.max_depth = 32;
    Json msg;
    std::string err;
    if (!Json::try_parse(text, &msg, &err, lim) || !msg.is_object()) {
      ws_reply(s, Json(), Response::error(400, "invalid JSON message: " + (err.empty() ? std::string("expected object") : err)), &ok);
      return ok;
    }
    const Json id = msg["id"];
    if (!tokens.still_valid(c->principal.token_id)) {
      ws_send_close(s, 1008, "token revoked or expired");
      return false;
    }
    if (!token_limiter.allow(c->principal.token_id, monotonic_seconds())) {
      ws_reply(s, id, Response::error(429, "rate limit exceeded"), &ok);
      return ok;
    }
    const std::string cmd = msg["cmd"].as_string();
    if (cmd.empty()) {
      ws_reply(s, id, Response::error(400, "missing cmd"), &ok);
      return ok;
    }
    if (cmd == "ping") {
      ws_reply(s, id, Response::ok(Json("pong")), &ok);
      return ok;
    }
    if (cmd == "subscribe" || cmd == "unsubscribe") {
      const Json& topics = msg["args"]["topics"];
      if (!topics.is_array() || topics.size() == 0 || topics.size() > 64) {
        ws_reply(s, id, Response::error(400, "args.topics must be a non-empty array"), &ok);
        return ok;
      }
      std::vector<std::string> list;
      for (std::size_t i = 0; i < topics.size(); ++i) {
        if (!topics.at(i).is_string() || !topic_valid(topics.at(i).as_string())) {
          ws_reply(s, id, Response::error(400, "invalid topic"), &ok);
          return ok;
        }
        list.push_back(topics.at(i).as_string());
      }
      bool too_many = false;
      Json now = Json::array();
      {
        std::lock_guard<std::mutex> lk(c->mu);
        for (const auto& t : list) {
          auto it = std::find(c->subs.begin(), c->subs.end(), t);
          if (cmd == "subscribe") {
            if (it == c->subs.end()) {
              if (c->subs.size() >= cfg.ws_max_subscriptions) { too_many = true; break; }
              c->subs.push_back(t);
            }
          } else if (it != c->subs.end()) {
            c->subs.erase(it);
          }
        }
        for (const auto& t : c->subs) now.push_back(t);
      }
      if (too_many) ws_reply(s, id, Response::error(429, "too many subscriptions"), &ok);
      else {
        Json res = Json::object();
        res.set("topics", std::move(now));
        ws_reply(s, id, Response::ok(std::move(res)), &ok);
      }
      return ok;
    }
    std::string scope_tmpl;
    WsHandler handler;
    {
      std::shared_lock<std::shared_mutex> lk(routes_mu);
      auto it = ws_commands.find(cmd);
      if (it != ws_commands.end()) {
        scope_tmpl = it->second.first;
        handler = it->second.second;
      }
    }
    if (!handler) {
      ws_reply(s, id, Response::error(404, "unknown command"), &ok);
      return ok;
    }
    const Json& args = msg["args"];
    if (!scope_tmpl.empty()) {
      std::map<std::string, std::string> vals;
      for (std::size_t i = 0; i < args.size() && args.is_object(); ++i) {
        const Json& v = args.at(i);
        if (v.is_string()) vals[args.key_at(i)] = v.as_string();
        else if (v.is_integer()) vals[args.key_at(i)] = std::to_string(v.as_int());
      }
      std::string need;
      if (!expand_scope(scope_tmpl, vals, &need)) {
        ws_reply(s, id, Response::error(400, "invalid or missing argument for scope"), &ok);
        return ok;
      }
      if (!c->principal.allows(need)) {
        ws_reply(s, id, Response::error(403, "token lacks required scope: " + need), &ok);
        return ok;
      }
    }
    WsRequest wr;
    wr.command = cmd;
    wr.args = args;
    wr.principal = c->principal;
    wr.connection_id = c->id;
    Response resp;
    try {
      resp = handler(wr);
    } catch (...) {
      resp = Response::error(500, "internal error");
    }
    ws_reply(s, id, resp, &ok);
    return ok;
  }

  void ws_loop(net::Stream& s, const std::shared_ptr<WsClient>& c, std::string& rbuf) {
    using clock = std::chrono::steady_clock;
    auto now = [] { return clock::now(); };
    auto last_rx = now(), last_ping = now(), last_auth = now();
    std::string frag;
    int frag_op = 0;
    auto ms_since = [&](clock::time_point t) {
      return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now() - t).count());
    };
    for (;;) {
      if (stopping) { ws_send_close(s, 1001, "server shutting down"); return; }
      // Outgoing events.
      for (;;) {
        std::shared_ptr<const std::string> m;
        bool dropped;
        {
          std::lock_guard<std::mutex> lk(c->mu);
          dropped = c->dropped;
          if (!dropped && !c->q.empty()) {
            m = c->q.front();
            c->q.pop_front();
            c->q_bytes -= m->size();
          }
        }
        if (dropped) { ws_send_close(s, 1013, "slow consumer"); return; }
        if (!m) break;
        if (!s.write_all(m->data(), m->size(), cfg.write_timeout_ms)) return;
      }
      // Incoming frames.
      for (;;) {
        if (rbuf.size() < 2) break;
        const auto* b = reinterpret_cast<const unsigned char*>(rbuf.data());
        const bool fin = b[0] & 0x80;
        const int op = b[0] & 0x0F;
        const bool masked = b[1] & 0x80;
        std::uint64_t len = b[1] & 0x7F;
        std::size_t hdr = 2;
        if ((b[0] & 0x70) != 0) { ws_send_close(s, 1002, "reserved bits set"); return; }
        if (!masked) { ws_send_close(s, 1002, "client frames must be masked"); return; }
        const bool control = op >= 8;
        if (control && (!fin || len > 125)) { ws_send_close(s, 1002, "bad control frame"); return; }
        if (len == 126) {
          if (rbuf.size() < 4) break;
          len = (static_cast<std::uint64_t>(b[2]) << 8) | b[3];
          hdr = 4;
          if (len < 126) { ws_send_close(s, 1002, "non-minimal length"); return; }
        } else if (len == 127) {
          if (rbuf.size() < 10) break;
          len = 0;
          for (int i = 0; i < 8; ++i) len = (len << 8) | b[2 + i];
          hdr = 10;
          if ((len >> 63) || len < 65536) { ws_send_close(s, len >> 63 ? 1009 : 1002, "bad length"); return; }
        }
        if (len > cfg.ws_max_message_bytes || (!control && frag.size() + len > cfg.ws_max_message_bytes)) {
          ws_send_close(s, 1009, "message too big");
          return;
        }
        if (rbuf.size() < hdr + 4 + len) break;
        unsigned char key[4];
        std::memcpy(key, b + hdr, 4);
        std::string payload(rbuf, hdr + 4, static_cast<std::size_t>(len));
        for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ key[i & 3]);
        rbuf.erase(0, hdr + 4 + static_cast<std::size_t>(len));
        last_rx = now();
        if (op == 9) {
          std::string f = frame_control(10, payload);
          if (!s.write_all(f.data(), f.size(), cfg.write_timeout_ms)) return;
        } else if (op == 10) {
          // pong: liveness already recorded
        } else if (op == 8) {
          int code = 1000;
          if (payload.size() == 1) { ws_send_close(s, 1002, "bad close payload"); return; }
          if (payload.size() >= 2) {
            code = (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
            if (!close_code_ok(code)) { ws_send_close(s, 1002, "bad close code"); return; }
            if (!valid_utf8(std::string_view(payload).substr(2))) { ws_send_close(s, 1007, "bad close reason"); return; }
          }
          ws_send_close(s, code, "");
          return;
        } else if (op == 0 || op == 1 || op == 2) {
          if (op == 2) { ws_send_close(s, 1003, "binary frames not supported"); return; }
          if (op == 0 && frag_op == 0) { ws_send_close(s, 1002, "unexpected continuation"); return; }
          if (op != 0 && frag_op != 0) { ws_send_close(s, 1002, "expected continuation"); return; }
          frag += payload;
          if (!fin) { frag_op = 1; continue; }
          std::string text = std::move(frag);
          frag.clear();
          frag_op = 0;
          if (!valid_utf8(text)) { ws_send_close(s, 1007, "invalid UTF-8"); return; }
          if (!ws_message(s, c, text)) return;
        } else {
          ws_send_close(s, 1002, "unknown opcode");
          return;
        }
      }
      // Liveness.
      if (ms_since(last_rx) > cfg.ws_idle_timeout_ms) { ws_send_close(s, 1001, "idle timeout"); return; }
      if (ms_since(last_ping) >= cfg.ws_ping_interval_ms) {
        std::string f = frame_control(9, "");
        if (!s.write_all(f.data(), f.size(), cfg.write_timeout_ms)) return;
        last_ping = now();
      }
      if (ms_since(last_auth) >= 2000) {
        last_auth = now();
        if (!tokens.still_valid(c->principal.token_id)) { ws_send_close(s, 1008, "token revoked or expired"); return; }
      }
      int r = fill(s, rbuf, c->wake.r, 200);
      if (r < 0) return;
      if (r == 2) c->wake.drain();
    }
  }

  // ---- accept loop -----------------------------------------------------------------------

  void reap(bool all) {
    std::list<std::shared_ptr<Slot>> finished;
    {
      std::lock_guard<std::mutex> lk(slots_mu);
      for (auto it = slots.begin(); it != slots.end();) {
        if (all || (*it)->done) {
          finished.push_back(*it);
          it = slots.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& sl : finished)
      if (sl->th.joinable()) sl->th.join();
  }

  void accept_loop() {
    while (!stopping) {
      std::string ip;
      net::Fd fd = net::accept_conn(listen_fd, accept_wake.r, 200, &ip);
      reap(false);
      if (fd == net::kBadFd) continue;
      auto slot = std::make_shared<Slot>();
      slot->stream = std::make_shared<net::Stream>(fd);
      slot->ip = ip;
      {
        std::lock_guard<std::mutex> lk(slots_mu);
        std::size_t same = 0;
        for (const auto& o : slots) same += (o->ip == ip && !o->done) ? 1 : 0;
        if (slots.size() >= cfg.max_connections || same >= cfg.max_connections_per_ip) continue;  // closes fd
        try {
          slot->th = std::thread([this, slot] {
            serve(slot);
            slot->done = true;
          });
        } catch (...) {
          continue;
        }
        slots.push_back(slot);
      }
    }
  }
};

// ---- ApiServer ----------------------------------------------------------------------------

ApiServer::ApiServer(TokenStore& tokens, ApiConfig config) : impl_(new Impl(tokens, std::move(config))) {}

ApiServer::~ApiServer() { stop(); }

bool ApiServer::add_route(const std::string& method, const std::string& pattern, const std::string& scope, Handler handler,
                          std::string* error) {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return false;
  };
  try {
    static const char* kMethods[] = {"GET", "POST", "PUT", "PATCH", "DELETE"};
    if (std::find(std::begin(kMethods), std::end(kMethods), method) == std::end(kMethods)) return fail("unsupported method");
    if (!handler) return fail("null handler");
    if (pattern.empty() || pattern[0] != '/') return fail("pattern must start with '/'");
    Route r;
    r.method = method;
    r.scope = scope;
    r.handler = std::move(handler);
    for (const auto& seg : split_path(pattern)) {
      if (seg.empty() && pattern != "/") return fail("empty path segment");
      if (seg.size() > 2 && seg.front() == '{' && seg.back() == '}') {
        r.segs.emplace_back(true, seg.substr(1, seg.size() - 2));
      } else {
        if (seg.find_first_of("{}") != std::string::npos) return fail("captures must be a whole segment");
        r.segs.emplace_back(false, seg);
        ++r.literals;
      }
    }
    if (pattern == "/") r.segs.assign(1, {false, ""});
    if (!scope.empty()) {
      std::map<std::string, std::string> probe;
      for (const auto& sg : r.segs)
        if (sg.first) probe[sg.second] = "x";
      std::string tmp;
      if (!expand_scope(scope, probe, &tmp)) return fail("invalid scope template");
    }
    std::unique_lock<std::shared_mutex> lk(impl_->routes_mu);
    for (const auto& o : impl_->routes) {
      if (o.method != method || o.segs.size() != r.segs.size()) continue;
      bool same = true;
      for (std::size_t i = 0; i < o.segs.size(); ++i)
        if (o.segs[i].first != r.segs[i].first || (!o.segs[i].first && o.segs[i].second != r.segs[i].second)) same = false;
      if (same) return fail("duplicate route");
    }
    impl_->routes.push_back(std::move(r));
    return true;
  } catch (...) {
    return fail("internal error");
  }
}

bool ApiServer::add_ws_command(const std::string& name, const std::string& scope, WsHandler handler, std::string* error) {
  try {
    if (name.empty() || !handler || name == "subscribe" || name == "unsubscribe" || name == "ping") {
      if (error) *error = "invalid command";
      return false;
    }
    std::map<std::string, std::string> probe;
    for (std::size_t i = 0; i < scope.size(); ++i) {
      if (scope[i] != '{') continue;
      auto e = scope.find('}', i);
      if (e == std::string::npos) break;
      probe[scope.substr(i + 1, e - i - 1)] = "x";
    }
    std::string tmp;
    if (!scope.empty() && !expand_scope(scope, probe, &tmp)) {
      if (error) *error = "invalid scope template";
      return false;
    }
    std::unique_lock<std::shared_mutex> lk(impl_->routes_mu);
    if (!impl_->ws_commands.emplace(name, std::make_pair(scope, std::move(handler))).second) {
      if (error) *error = "duplicate command";
      return false;
    }
    return true;
  } catch (...) {
    if (error) *error = "internal error";
    return false;
  }
}

bool ApiServer::start(std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  try {
    Impl& I = *impl_;
    if (I.running) return fail("already running");
    const bool want_tls = !I.cfg.tls_cert.empty() || !I.cfg.tls_key.empty();
    if (want_tls && (I.cfg.tls_cert.empty() || I.cfg.tls_key.empty())) return fail("TLS needs both certificate and key");
    if (!want_tls && !net::is_loopback_address(I.cfg.bind_address))
      return fail("refusing to bind non-loopback address " + I.cfg.bind_address + " without TLS");
    if (want_tls) {
      std::string e;
      I.tls = net::TlsContext::server(I.cfg.tls_cert, I.cfg.tls_key, &e);
      if (!I.tls) return fail(e);
    }
    std::string e;
    I.listen_fd = net::listen_tcp(I.cfg.bind_address, I.cfg.port, &I.bound_port, &e);
    if (I.listen_fd == net::kBadFd) return fail(e);
    if (!I.accept_wake.open()) {
      net::close_fd(I.listen_fd);
      I.listen_fd = net::kBadFd;
      return fail("cannot create wake pipe");
    }
    I.stopping = false;
    I.running = true;
    I.accept_thread = std::thread([&I] { I.accept_loop(); });
    DH_LOG(Info, "api: listening on %s:%u (%s)", I.cfg.bind_address.c_str(), I.bound_port, want_tls ? "TLS" : "plain HTTP");
    return true;
  } catch (const std::exception& ex) {
    impl_->running = false;
    return fail(std::string("start failed: ") + ex.what());
  }
}

void ApiServer::stop() {
  Impl& I = *impl_;
  try {
    if (!I.running.exchange(false)) return;
    I.stopping = true;
    I.accept_wake.signal();
    if (I.accept_thread.joinable()) I.accept_thread.join();
    {
      std::lock_guard<std::mutex> lk(I.slots_mu);
      for (auto& s : I.slots) s->stream->shutdown();
    }
    {
      std::lock_guard<std::mutex> lk(I.clients_mu);
      for (auto& c : I.clients) c->wake.signal();
    }
    I.reap(true);
    net::close_fd(I.listen_fd);
    I.listen_fd = net::kBadFd;
    I.accept_wake.close();
    I.tls.reset();
  } catch (...) {
  }
}

bool ApiServer::running() const { return impl_->running; }
std::uint16_t ApiServer::port() const { return impl_->bound_port; }
bool ApiServer::tls_enabled() const { return !impl_->cfg.tls_cert.empty(); }

void ApiServer::publish(const std::string& topic, const Json& data, const std::string& scope) {
  try {
    Json j = Json::object();
    j.set("topic", topic);
    j.set("data", data);
    auto frame = std::make_shared<const std::string>(frame_text(j.dump()));
    std::lock_guard<std::mutex> lk(impl_->clients_mu);
    for (auto& c : impl_->clients) {
      if (!scope.empty() && !c->principal.allows(scope)) continue;
      {
        std::lock_guard<std::mutex> ck(c->mu);
        if (c->dropped) continue;
        bool sub = false;
        for (const auto& p : c->subs)
          if (topic_matches(p, topic)) { sub = true; break; }
        if (!sub) continue;
        if (c->q.size() >= impl_->cfg.ws_queue_messages || c->q_bytes + frame->size() > impl_->cfg.ws_queue_bytes) {
          c->dropped = true;
          c->q.clear();
          c->q_bytes = 0;
        } else {
          c->q.push_back(frame);
          c->q_bytes += frame->size();
        }
      }
      c->wake.signal();
    }
  } catch (...) {
  }
}

std::size_t ApiServer::ws_client_count() const {
  std::lock_guard<std::mutex> lk(impl_->clients_mu);
  return impl_->clients.size();
}

}  // namespace deckhand::api
