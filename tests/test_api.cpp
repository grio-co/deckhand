#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "check.hpp"
#include "deckhand/api/auth.hpp"
#include "deckhand/api/json.hpp"
#include "deckhand/api/server.hpp"
#include "deckhand/api/test_client.hpp"

using namespace deckhand::api;
using deckhand::api::testing::TestClient;

namespace {

// ---- JSON ---------------------------------------------------------------------------------

Json P(const std::string& s) {
  std::string err;
  Json j;
  CHECK(Json::try_parse(s, &j, &err));
  return j;
}
bool bad(const std::string& s, const JsonLimits& l = {}) {
  Json j;
  return !Json::try_parse(s, &j, nullptr, l);
}

void test_json() {
  CHECK(P("null").is_null());
  CHECK(P(" true ").as_bool());
  CHECK(P("-12").as_int() == -12 && P("-12").is_integer());
  CHECK(P("1.5e2").as_double() == 150.0 && !P("1.5e2").is_integer());
  CHECK(P("9223372036854775807").as_int() == INT64_MAX);
  CHECK(!P("9223372036854775808").is_integer());  // falls back to double
  CHECK(P("\"a\\n\\u00e9\\ud83d\\ude00\"").as_string() == "a\n\xC3\xA9\xF0\x9F\x98\x80");
  Json o = P("{\"b\":1,\"a\":[1,2,{\"x\":null}],\"b\":2}");
  CHECK(o.size() == 2 && o.key_at(0) == "b" && o["b"].as_int() == 2);  // order kept, last dup wins
  CHECK(o["a"].size() == 3 && o["a"].at(2)["x"].is_null() && o["missing"].is_null());
  CHECK(o.dump() == "{\"b\":2,\"a\":[1,2,{\"x\":null}]}");
  // round trips
  for (const char* t : {"[]", "{}", "[1,2.5,-3,\"x\",true,false,null]", "{\"k\":{\"n\":[[]]}}",
                        "\"\\u0001\\\"\\\\\\b\\f\\n\\r\\t\"", "0.1", "1e-7", "123456789012345678"}) {
    Json a = P(t);
    CHECK(P(a.dump()) == a);
  }
  CHECK(Json("\xC3\x28").dump() == "\"\xEF\xBF\xBD\x28\"");  // invalid UTF-8 never emitted
  Json b = Json::object();
  b.set("n", 5).set("s", "x").set("n", 6);
  b.set("arr", Json::array().push_back(1).push_back("two"));
  CHECK(b.dump() == "{\"n\":6,\"s\":\"x\",\"arr\":[1,\"two\"]}");
  CHECK(Json(0.5).dump() == "0.5" && Json(3).dump() == "3");
  // malformed
  for (const char* t : {"", " ", "{", "[1,]", "{\"a\":1,}", "[1 2]", "01", "+1", "1.", ".5", "1e", "-", "--1", "NaN",
                        "Infinity", "'a'", "\"abc", "\"\\x\"", "\"\\u12\"", "\"\\ud800\"", "\"\\udc00\"",
                        "\"\\ud800\\u0041\"", "\"a\tb\"", "{a:1}", "{\"a\" 1}", "[1]x", "nul", "truee", "// c\n1",
                        "\xEF\xBB\xBF" "1", "\"\xC0\xAF\"", "\"\xED\xA0\x80\"", "\"\xF4\x90\x80\x80\"", "1e999"})
    CHECK(bad(t));
  JsonLimits lim;
  lim.max_depth = 4;
  CHECK(bad("[[[[[1]]]]]", lim));
  CHECK(!bad("[[[1]]]", lim));
  CHECK(bad(std::string(100, '[') + std::string(100, ']')));  // default depth 64
  lim = {};
  lim.max_bytes = 8;
  CHECK(bad("[1,2,3,4,5]", lim) && !bad("[1,2]", lim));
  std::string err;
  Json::parse("[1,}", &err);
  CHECK(err.find("offset") != std::string::npos);
  CHECK(valid_utf8("\xE2\x82\xAC") && !valid_utf8("\xE2\x82") && !valid_utf8("\xC1\xBF"));
}

// ---- scopes / CIDR / tokens -----------------------------------------------------------------

void test_auth_units() {
  CHECK(scope_allows("admin", "anything:at:all"));
  CHECK(scope_allows("read", "read"));
  CHECK(!scope_allows("read", "control"));
  CHECK(scope_allows("control", "control:deck:1"));
  CHECK(!scope_allows("control:deck:1", "control"));
  CHECK(scope_allows("control:deck:1", "control:deck:1"));
  CHECK(!scope_allows("control:deck:1", "control:deck:2"));
  CHECK(scope_allows("control:deck:*", "control:deck:7"));
  CHECK(!scope_allows("control:deck:*", "control"));
  CHECK(!scope_allows("control:deck:*", "library:deck:7"));
  CHECK(!scope_allows("*", "read") && !valid_scope("*") && !valid_scope("a::b") && !valid_scope("A") && !valid_scope(""));
  CHECK(!scope_allows("control", "controls"));
  CHECK(valid_scope("control:deck:*") && valid_scope("library"));

  CHECK(cidr_contains("10.0.0.0/8", "10.9.8.7") && !cidr_contains("10.0.0.0/8", "11.0.0.1"));
  CHECK(cidr_contains("192.168.1.5", "192.168.1.5") && !cidr_contains("192.168.1.5", "192.168.1.6"));
  CHECK(cidr_contains("192.168.0.0/23", "192.168.1.200") && !cidr_contains("192.168.0.0/23", "192.168.2.1"));
  CHECK(cidr_contains("127.0.0.0/8", "::ffff:127.0.0.1"));
  CHECK(cidr_contains("::1/128", "::1") && !cidr_contains("::1/128", "::2"));
  CHECK(cidr_contains("fd00::/8", "fd12::1") && !cidr_contains("fd00::/8", "fe80::1"));
  CHECK(!cidr_contains("::1/128", "127.0.0.1") && !cidr_contains("10.0.0.0/8", "::1"));
  CHECK(valid_cidr("10.0.0.0/8") && !valid_cidr("10.0.0.0/33") && !valid_cidr("nope") && !valid_cidr("1.2.3.4/"));

  CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(random_hex(32).size() == 64 && random_hex(8) != random_hex(8));

  RateLimiter rl(10, 3);
  CHECK(rl.allow("a", 0) && rl.allow("a", 0) && rl.allow("a", 0) && !rl.allow("a", 0));
  CHECK(rl.allow("b", 0));          // separate bucket
  CHECK(rl.allow("a", 0.15));       // refilled 1.5 tokens
  CHECK(!rl.allow("a", 0.15));
  FailureTracker ft(3, 10, 5);
  ft.failure("x", 0); ft.failure("x", 1);
  CHECK(!ft.locked("x", 1.5));
  ft.failure("x", 2);
  double retry = 0;
  CHECK(ft.locked("x", 3, &retry) && retry > 3.9 && retry < 4.1);
  CHECK(!ft.locked("x", 7.5) && !ft.locked("y", 3));
  ft.failure("z", 0); ft.failure("z", 100);  // window expired, count restarts
  ft.failure("z", 101);
  CHECK(!ft.locked("z", 101));
}

std::string read_all(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void test_token_store(const std::string& dir) {
  const std::string path = dir + "/tokens.json";
  TokenStore ts(path);
  std::string err;
  CHECK(ts.load(&err));  // missing file is fine
  TokenStore::Created c;
  CHECK(!ts.create("", {"read"}, 0, {}, &c, &err));
  CHECK(!ts.create("n", {}, 0, {}, &c, &err));
  CHECK(!ts.create("n", {"Bad Scope"}, 0, {}, &c, &err));
  CHECK(!ts.create("n", {"read"}, 0, {"zzz"}, &c, &err));
  CHECK(ts.create("stage", {"read", "control:deck:1"}, 0, {"127.0.0.0/8"}, &c, &err));
  CHECK(c.secret.size() == 3 + 64 && c.secret.rfind("dh_", 0) == 0);
  struct stat st;
  CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  std::string disk = read_all(path);
  CHECK(disk.find(c.secret) == std::string::npos);                         // secret is never stored
  CHECK(disk.find(sha256_hex(c.secret)) != std::string::npos);             // only its hash
  TokenInfo info;
  CHECK(ts.authenticate(c.secret, "127.0.0.1", &info) == TokenStore::Result::Ok && info.name == "stage");
  CHECK(ts.authenticate(c.secret, "8.8.8.8", &info) == TokenStore::Result::SourceDenied);
  CHECK(ts.authenticate(c.secret + "x", "127.0.0.1", &info) == TokenStore::Result::Unknown);
  CHECK(ts.authenticate("", "127.0.0.1", &info) == TokenStore::Result::Unknown);
  CHECK(ts.authenticate(sha256_hex(c.secret), "127.0.0.1", &info) == TokenStore::Result::Unknown);  // hash is not a credential
  TokenStore::Created e;
  CHECK(ts.create("old", {"read"}, 1000, {}, &e, &err));
  CHECK(ts.authenticate(e.secret, "127.0.0.1", &info, 999) == TokenStore::Result::Ok);
  CHECK(ts.authenticate(e.secret, "127.0.0.1", &info, 1000) == TokenStore::Result::Expired);
  // persistence
  TokenStore ts2(path);
  CHECK(ts2.load(&err) && ts2.list().size() == 2);
  CHECK(ts2.authenticate(c.secret, "127.0.0.1", &info) == TokenStore::Result::Ok && info.scopes.size() == 2);
  CHECK(ts2.revoke(c.info.id) && !ts2.revoke(c.info.id));
  CHECK(ts2.authenticate(c.secret, "127.0.0.1", &info) == TokenStore::Result::Unknown);
  TokenStore ts3(path);
  CHECK(ts3.load(&err) && ts3.list().size() == 1);
  // corrupt file: refuse to load and refuse to overwrite
  { std::ofstream f(path); f << "{not json"; }
  TokenStore ts4(path);
  CHECK(!ts4.load(&err) && !err.empty());
  CHECK(!ts4.create("x", {"read"}, 0, {}, &c, &err));
  CHECK(read_all(path) == "{not json");
}

// ---- minimal client -------------------------------------------------------------------------

struct Conn {
  TestClient c;
  std::string buf;
  bool open(std::uint16_t port, bool tls = false) {
    std::string err;
    return c.connect("127.0.0.1", port, tls, &err);
  }
  // 1 = more data, 0 = timeout, -1 = closed
  int fill(int ms) {
    char tmp[8192];
    long n = c.recv(tmp, sizeof tmp, ms);
    if (n > 0) buf.append(tmp, static_cast<std::size_t>(n));
    return n > 0 ? 1 : (n == 0 ? 0 : -1);
  }
};

struct Http {
  int status = 0;
  std::map<std::string, std::string> headers;
  std::string body;
  Json json() const { return Json::parse(body); }
};

bool read_http(Conn& c, Http* out, int ms = 3000) {
  for (;;) {
    auto p = c.buf.find("\r\n\r\n");
    if (p != std::string::npos) {
      std::string head = c.buf.substr(0, p);
      out->status = std::atoi(head.c_str() + 9);
      out->headers.clear();
      std::size_t pos = head.find("\r\n");
      while (pos != std::string::npos) {
        std::size_t e = head.find("\r\n", pos + 2);
        std::string line = head.substr(pos + 2, e == std::string::npos ? std::string::npos : e - pos - 2);
        auto col = line.find(':');
        if (col != std::string::npos) {
          std::string k = line.substr(0, col);
          for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          std::size_t vs = line.find_first_not_of(' ', col + 1);
          out->headers[k] = vs == std::string::npos ? "" : line.substr(vs);
        }
        pos = e;
      }
      std::size_t cl = out->headers.count("content-length") ? std::stoul(out->headers["content-length"]) : 0;
      while (c.buf.size() < p + 4 + cl)
        if (c.fill(ms) <= 0) return false;
      out->body = c.buf.substr(p + 4, cl);
      c.buf.erase(0, p + 4 + cl);
      return true;
    }
    if (c.fill(ms) <= 0) return false;
  }
}

std::string req(const std::string& method, const std::string& target, const std::string& token = "",
                const std::string& body = "", const std::string& extra = "") {
  std::string r = method + " " + target + " HTTP/1.1\r\nHost: x\r\n";
  if (!token.empty()) r += "Authorization: Bearer " + token + "\r\n";
  r += extra;
  if (!body.empty() || method == "POST") r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  return r + "\r\n" + body;
}

Http once(std::uint16_t port, const std::string& raw, bool tls = false) {
  Conn c;
  Http h;
  CHECK(c.open(port, tls));
  CHECK(c.c.send(raw));
  CHECK(read_http(c, &h));
  return h;
}

// WebSocket helpers
std::string ws_frame(int op, const std::string& payload, bool fin = true, bool mask = true) {
  std::string f;
  f += static_cast<char>((fin ? 0x80 : 0) | op);
  std::size_t n = payload.size();
  unsigned char mb = mask ? 0x80 : 0;
  if (n < 126) f += static_cast<char>(mb | n);
  else if (n <= 0xFFFF) { f += static_cast<char>(mb | 126); f += static_cast<char>(n >> 8); f += static_cast<char>(n & 255); }
  else { f += static_cast<char>(mb | 127); for (int i = 7; i >= 0; --i) f += static_cast<char>((static_cast<std::uint64_t>(n) >> (8 * i)) & 255); }
  if (mask) {
    const unsigned char key[4] = {0x12, 0x34, 0x56, 0x78};
    f.append(reinterpret_cast<const char*>(key), 4);
    for (std::size_t i = 0; i < n; ++i) f += static_cast<char>(payload[i] ^ key[i & 3]);
  } else {
    f += payload;
  }
  return f;
}

// Header announcing a huge payload without sending it.
std::string ws_big_header(std::uint64_t len) {
  std::string f;
  f += static_cast<char>(0x81);
  f += static_cast<char>(0x80 | 127);
  for (int i = 7; i >= 0; --i) f += static_cast<char>((len >> (8 * i)) & 255);
  f.append("\x01\x02\x03\x04", 4);
  return f;
}

struct Frame {
  int op = 0;
  std::string payload;
};
bool read_frame(Conn& c, Frame* f, int ms = 3000) {
  for (;;) {
    if (c.buf.size() >= 2) {
      std::size_t n = static_cast<unsigned char>(c.buf[1]) & 0x7F, hdr = 2;
      if (n == 126 && c.buf.size() >= 4) { n = (static_cast<unsigned char>(c.buf[2]) << 8) | static_cast<unsigned char>(c.buf[3]); hdr = 4; }
      else if (n == 126) n = SIZE_MAX;
      if (n == 127) {
        if (c.buf.size() < 10) { if (c.fill(ms) <= 0) return false; continue; }
        n = 0;
        for (int i = 0; i < 8; ++i) n = (n << 8) | static_cast<unsigned char>(c.buf[2 + i]);
        hdr = 10;
      }
      if (n != SIZE_MAX && c.buf.size() >= hdr + n) {
        f->op = c.buf[0] & 0x0F;
        f->payload = c.buf.substr(hdr, n);
        c.buf.erase(0, hdr + n);
        return true;
      }
    }
    if (c.fill(ms) <= 0) return false;
  }
}
int close_code(const Frame& f) {
  return f.payload.size() >= 2 ? (static_cast<unsigned char>(f.payload[0]) << 8) | static_cast<unsigned char>(f.payload[1]) : -1;
}

const char* kKey = "dGhlIHNhbXBsZSBub25jZQ==";

std::string ws_upgrade(const std::string& token_header, const std::string& extra = "", const std::string& key = kKey,
                       const std::string& path = "/v1/ws") {
  return "GET " + path + " HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
         "\r\nSec-WebSocket-Version: 13\r\n" + token_header + extra + "\r\n";
}

bool ws_connect(Conn& c, std::uint16_t port, const std::string& token, bool tls = false, Http* resp = nullptr) {
  if (!c.open(port, tls)) return false;
  c.c.send(ws_upgrade("Authorization: Bearer " + token + "\r\n"));
  Http h;
  if (!read_http(c, &h)) return false;
  if (resp) *resp = h;
  return h.status == 101;
}

bool send_cmd(Conn& c, const std::string& json) { return c.c.send(ws_frame(1, json)); }
Json recv_json(Conn& c) {
  Frame f;
  CHECK(read_frame(c, &f));
  CHECK(f.op == 1);
  return Json::parse(f.payload);
}

struct Tokens {
  TokenStore store;
  std::string admin, reader, deck1, expired, revoked, cidr;
  Tokens() : store("") {
    std::string e;
    TokenStore::Created c;
    CHECK(store.create("admin", {"admin"}, 0, {}, &c, &e)); admin = c.secret;
    CHECK(store.create("reader", {"read"}, 0, {}, &c, &e)); reader = c.secret;
    CHECK(store.create("deck1", {"read", "control:deck:1"}, 0, {}, &c, &e)); deck1 = c.secret;
    CHECK(store.create("expired", {"admin"}, 1, {}, &c, &e)); expired = c.secret;
    CHECK(store.create("revoked", {"admin"}, 0, {}, &c, &e)); revoked = c.secret;
    CHECK(store.revoke(c.info.id));
    CHECK(store.create("cidr", {"admin"}, 0, {"10.0.0.0/8"}, &c, &e)); cidr = c.secret;
  }
};

void add_test_routes(ApiServer& s) {
  CHECK(s.add_route("GET", "/v1/health", "", [](const Request&) { return Response::ok(P("{\"ok\":true}")); }));
  CHECK(s.add_route("GET", "/v1/decks/{deck}/state", "read", [](const Request& r) {
    Json j = Json::object();
    j.set("deck", r.params.at("deck"));
    j.set("who", r.principal.name);
    j.set("q", r.query.count("q") ? r.query.at("q") : "");
    return Response::ok(j);
  }));
  CHECK(s.add_route("POST", "/v1/decks/{deck}/play", "control:deck:{deck}", [](const Request& r) {
    Json j = Json::object();
    j.set("deck", r.params.at("deck"));
    j.set("body", r.body);
    return Response::ok(j);
  }));
  CHECK(s.add_route("GET", "/v1/decks/special/state", "read", [](const Request&) { return Response::ok(Json("literal")); }));
  CHECK(s.add_route("POST", "/v1/boom", "admin", [](const Request&) -> Response { throw 1; }));
  CHECK(s.add_route("DELETE", "/v1/thing", "admin", [](const Request&) {
    Response r;
    r.status = 204;
    return r;
  }));
  CHECK(!s.add_route("GET", "/v1/health", "", [](const Request&) { return Response::ok(); }));  // duplicate
  CHECK(!s.add_route("TRACE", "/x", "", [](const Request&) { return Response::ok(); }));
  CHECK(!s.add_route("GET", "/x/{a", "", [](const Request&) { return Response::ok(); }));
  CHECK(!s.add_route("GET", "/x/{a}", "control:{b}", [](const Request&) { return Response::ok(); }));
  CHECK(s.add_ws_command("deck.play", "control:deck:{deck}", [](const WsRequest& r) {
    Json j = Json::object();
    j.set("played", r.args["deck"]);
    j.set("who", r.principal.name);
    return Response::ok(j);
  }));
  CHECK(s.add_ws_command("echo", "read", [](const WsRequest& r) { return Response::ok(r.args); }));
}

// Everything that should hold both over plain HTTP and TLS.
void exercise_http(std::uint16_t port, bool tls, Tokens& t) {
  Http h = once(port, req("GET", "/v1/health", "", "", "Connection: close\r\n"), tls);
  CHECK(h.status == 200 && h.json()["ok"].as_bool());
  CHECK(h.headers["content-type"].find("application/json") == 0);
  CHECK(h.headers.count("access-control-allow-origin") == 0);  // CORS off by default

  h = once(port, req("GET", "/v1/decks/3/state?q=a%20b+c&x", t.reader), tls);
  CHECK(h.status == 200 && h.json()["deck"].as_string() == "3" && h.json()["q"].as_string() == "a b c" &&
        h.json()["who"].as_string() == "reader");
  h = once(port, req("GET", "/v1/decks/special/state", t.reader), tls);
  CHECK(h.json().as_string() == "literal");  // literal beats capture
  h = once(port, req("GET", "/v1/decks/a%2Fb/state", t.reader), tls);
  CHECK(h.status == 200 && h.json()["deck"].as_string() == "a/b");

  // auth failures
  h = once(port, req("GET", "/v1/decks/1/state"), tls);
  CHECK(h.status == 401 && h.headers["www-authenticate"].find("Bearer") == 0);
  CHECK(once(port, req("GET", "/v1/decks/1/state", "dh_wrong"), tls).status == 401);
  CHECK(once(port, req("GET", "/v1/decks/1/state", t.expired), tls).status == 401);
  CHECK(once(port, req("GET", "/v1/decks/1/state", t.revoked), tls).status == 401);
  CHECK(once(port, req("GET", "/v1/decks/1/state", t.cidr), tls).status == 403);  // source not allowed
  CHECK(once(port, req("GET", "/v1/decks/1/state", "", "", "Authorization: Basic abc\r\n"), tls).status == 401);
  CHECK(once(port, req("GET", "/nope", t.reader), tls).status == 404);
  CHECK(once(port, req("GET", "/nope"), tls).status == 401);  // no route enumeration without a token
  // scope checks
  CHECK(once(port, req("POST", "/v1/decks/1/play", t.reader, "{}"), tls).status == 403);
  CHECK(once(port, req("POST", "/v1/decks/2/play", t.deck1, "{}"), tls).status == 403);
  h = once(port, req("POST", "/v1/decks/1/play", t.deck1, "{\"a\":[1,2]}"), tls);
  CHECK(h.status == 200 && h.json()["body"]["a"].size() == 2);
  CHECK(once(port, req("POST", "/v1/decks/Bad:1/play", t.admin, "{}"), tls).status == 400);  // unusable in scope
  CHECK(once(port, req("POST", "/v1/decks/1/play", t.admin, ""), tls).status == 200);  // empty body = null
  // method handling
  h = once(port, req("PUT", "/v1/decks/1/state", t.reader), tls);
  CHECK(h.status == 405 && h.headers["allow"] == "GET");
  CHECK(once(port, req("DELETE", "/v1/thing", t.admin), tls).status == 204);
  CHECK(once(port, req("POST", "/v1/boom", t.admin, "{}"), tls).status == 500);
  // body problems
  CHECK(once(port, req("POST", "/v1/decks/1/play", t.admin, "{oops"), tls).status == 400);
  h = once(port, "POST /v1/decks/1/play HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + t.admin +
                     "\r\nContent-Length: 2000000\r\n\r\n", tls);
  CHECK(h.status == 413);
  CHECK(once(port, "POST /v1/decks/1/play HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", tls).status == 400);
  CHECK(once(port, "POST /v1/health HTTP/1.1\r\nContent-Length: abc\r\n\r\n", tls).status == 400);
  // malformed requests
  CHECK(once(port, "GARBAGE\r\n\r\n", tls).status == 400);
  CHECK(once(port, "GET /v1/health HTTP/9.9\r\n\r\n", tls).status == 400);
  CHECK(once(port, "GET v1/health HTTP/1.1\r\n\r\n", tls).status == 400);
  CHECK(once(port, "GET /v1/health HTTP/1.1\r\n bad: fold\r\n\r\n", tls).status == 400);
  CHECK(once(port, "GET /v1/health HTTP/1.1\r\nNoColon\r\n\r\n", tls).status == 400);
  CHECK(once(port, "GET /v1/decks/%zz/state HTTP/1.1\r\nAuthorization: Bearer " + t.admin + "\r\n\r\n", tls).status == 400);
  CHECK(once(port, "GET /" + std::string(5000, 'a') + " HTTP/1.1\r\n\r\n", tls).status == 414);
  CHECK(once(port, "GET /v1/health HTTP/1.1\r\nX: " + std::string(20000, 'a') + "\r\n\r\n", tls).status == 431);

  // keep-alive and pipelining on one connection
  Conn c;
  CHECK(c.open(port, tls));
  CHECK(c.c.send(req("GET", "/v1/health")));
  CHECK(read_http(c, &h) && h.status == 200 && h.headers["connection"] == "keep-alive");
  CHECK(c.c.send(req("GET", "/v1/decks/9/state", t.reader) + req("GET", "/v1/health")));  // pipelined
  CHECK(read_http(c, &h) && h.json()["deck"].as_string() == "9");
  CHECK(read_http(c, &h) && h.status == 200);
  CHECK(c.c.send(req("GET", "/v1/health", "", "", "Connection: close\r\n")));
  CHECK(read_http(c, &h) && h.headers["connection"] == "close");
  CHECK(c.fill(2000) < 0);  // server closed it
}

void exercise_ws(ApiServer& s, std::uint16_t port, bool tls, Tokens& t) {
  // handshake: RFC 6455 sample key
  Conn a;
  Http h;
  CHECK(ws_connect(a, port, t.reader, tls, &h));
  CHECK(h.headers["sec-websocket-accept"] == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  CHECK(h.headers.count("sec-websocket-protocol") == 0);

  // handshake rejections
  { Conn c; CHECK(c.open(port, tls)); c.c.send(ws_upgrade("")); CHECK(read_http(c, &h) && h.status == 401); }
  { Conn c; CHECK(c.open(port, tls)); c.c.send(ws_upgrade("Authorization: Bearer dh_nope\r\n")); CHECK(read_http(c, &h) && h.status == 401); }
  { Conn c; CHECK(c.open(port, tls)); c.c.send(ws_upgrade("Authorization: Bearer " + t.reader + "\r\n", "", "short")); CHECK(read_http(c, &h) && h.status == 400); }
  { Conn c; CHECK(c.open(port, tls));
    std::string r = ws_upgrade("Authorization: Bearer " + t.reader + "\r\n");
    r.replace(r.find("Version: 13"), 11, "Version: 8");
    c.c.send(r);
    CHECK(read_http(c, &h) && h.status == 426 && h.headers["sec-websocket-version"] == "13"); }
  { Conn c; CHECK(c.open(port, tls));
    c.c.send(ws_upgrade("Authorization: Bearer " + t.reader + "\r\n", "Origin: https://evil.example\r\n"));
    CHECK(read_http(c, &h) && h.status == 403); }  // origin not allowlisted
  // token via Sec-WebSocket-Protocol (browser form), never via URL
  { Conn c; CHECK(c.open(port, tls));
    c.c.send(ws_upgrade("Sec-WebSocket-Protocol: deckhand.v1, bearer." + t.reader + "\r\n"));
    CHECK(read_http(c, &h) && h.status == 101 && h.headers["sec-websocket-protocol"] == "deckhand.v1"); }
  { Conn c; CHECK(c.open(port, tls));
    c.c.send(ws_upgrade("", "", kKey, "/v1/ws?token=" + t.reader));  // query tokens are ignored
    CHECK(read_http(c, &h) && h.status == 401); }

  // subscribe + publish
  CHECK(send_cmd(a, "{\"id\":1,\"cmd\":\"subscribe\",\"args\":{\"topics\":[\"deck.1.position\",\"admin.*\"]}}"));
  Json r = recv_json(a);
  CHECK(r["id"].as_int() == 1 && r["ok"].as_bool() && r["result"]["topics"].size() == 2);
  s.publish("deck.2.position", P("{\"p\":0}"));                       // not subscribed
  s.publish("admin.secret", P("{\"x\":1}"), "admin");                  // subscribed but scope missing
  s.publish("deck.1.position", P("{\"p\":42}"));
  r = recv_json(a);
  CHECK(r["topic"].as_string() == "deck.1.position" && r["data"]["p"].as_int() == 42);  // only the right one arrived
  CHECK(send_cmd(a, "{\"cmd\":\"unsubscribe\",\"args\":{\"topics\":[\"deck.1.position\"]}}"));
  CHECK(recv_json(a)["ok"].as_bool());
  s.publish("deck.1.position", P("{\"p\":43}"));
  CHECK(send_cmd(a, "{\"id\":\"e\",\"cmd\":\"echo\",\"args\":{\"v\":1}}"));
  r = recv_json(a);
  CHECK(r["id"].as_string() == "e" && r["result"]["v"].as_int() == 1);  // no event between
  // wildcard subscription for an admin
  Conn adm;
  CHECK(ws_connect(adm, port, t.admin, tls));
  CHECK(send_cmd(adm, "{\"cmd\":\"subscribe\",\"args\":{\"topics\":[\"*\"]}}"));
  CHECK(recv_json(adm)["ok"].as_bool());
  s.publish("anything", Json(7), "admin");
  r = recv_json(adm);
  CHECK(r["topic"].as_string() == "anything" && r["data"].as_int() == 7);

  // command scope / errors
  CHECK(send_cmd(a, "{\"id\":2,\"cmd\":\"deck.play\",\"args\":{\"deck\":1}}"));
  r = recv_json(a);
  CHECK(!r["ok"].as_bool() && r["status"].as_int() == 403);
  CHECK(send_cmd(a, "{\"id\":3,\"cmd\":\"nope\"}"));
  CHECK(recv_json(a)["status"].as_int() == 404);
  CHECK(send_cmd(a, "not json"));
  CHECK(recv_json(a)["status"].as_int() == 400);
  CHECK(send_cmd(a, "{\"cmd\":\"subscribe\",\"args\":{\"topics\":[\"bad topic\"]}}"));
  CHECK(recv_json(a)["status"].as_int() == 400);
  Conn d1;
  CHECK(ws_connect(d1, port, t.deck1, tls));
  CHECK(send_cmd(d1, "{\"id\":4,\"cmd\":\"deck.play\",\"args\":{\"deck\":1}}"));
  r = recv_json(d1);
  CHECK(r["ok"].as_bool() && r["result"]["played"].as_int() == 1 && r["result"]["who"].as_string() == "deck1");
  CHECK(send_cmd(d1, "{\"id\":5,\"cmd\":\"deck.play\",\"args\":{\"deck\":2}}"));
  CHECK(recv_json(d1)["status"].as_int() == 403);

  // ping/pong, fragmentation, unsolicited pong
  CHECK(a.c.send(ws_frame(9, "hi")));
  Frame f;
  CHECK(read_frame(a, &f) && f.op == 10 && f.payload == "hi");
  CHECK(a.c.send(ws_frame(10, "")));
  CHECK(a.c.send(ws_frame(1, "{\"id\":9,\"cmd\"", false)) && a.c.send(ws_frame(9, "mid")) &&
        a.c.send(ws_frame(0, ":\"ping\"}", true)));
  CHECK(read_frame(a, &f) && f.op == 10 && f.payload == "mid");  // control frame between fragments
  r = recv_json(a);
  CHECK(r["id"].as_int() == 9 && r["result"].as_string() == "pong");
  CHECK(a.c.send(ws_frame(1, "{\"cmd\":\"echo\",\"args\":{\"s\":\"" + std::string(70000, 'x') + "\"}}")));
  r = recv_json(a);  // 16-bit length request, 64-bit length reply
  CHECK(r["result"]["s"].as_string().size() == 70000);

  // protocol violations each end with the right close code
  auto expect_close = [&](const std::string& bytes, int code) {
    Conn c;
    CHECK(ws_connect(c, port, t.reader, tls));
    CHECK(c.c.send(bytes));
    Frame fr;
    CHECK(read_frame(c, &fr) && fr.op == 8 && close_code(fr) == code);
    CHECK(c.fill(2000) < 0);
  };
  expect_close(ws_frame(1, "{}", true, false), 1002);                 // unmasked client frame
  expect_close(ws_frame(2, "bin"), 1003);                             // binary
  expect_close(ws_frame(1, "\xC3\x28"), 1007);                        // invalid UTF-8
  expect_close(ws_frame(0, "x"), 1002);                               // stray continuation
  expect_close(ws_frame(9, std::string(126, 'p')), 1002);             // oversize control frame
  expect_close(ws_frame(9, "p", false), 1002);                        // fragmented control frame
  expect_close(std::string(1, static_cast<char>(0xC1)) + std::string(1, static_cast<char>(0x80)) + "abcd", 1002);  // RSV1
  expect_close(ws_frame(3, ""), 1002);                                // reserved opcode
  expect_close(ws_big_header(10ull << 20), 1009);                     // announced size over the limit, never sent
  expect_close(ws_frame(1, std::string(300 * 1024, 'x')), 1009);      // oversize message
  expect_close(ws_frame(1, "a", false) + ws_frame(1, "b"), 1002);     // new data frame inside a fragmented message
  expect_close(ws_frame(8, std::string("\x03\xe8", 2) + "bye"), 1000);  // clean close is echoed
  expect_close(ws_frame(8, std::string("\x03\xe5", 2)), 1002);        // 1005 must not appear on the wire
  expect_close(ws_frame(8, ""), 1000);

  // revoking a token drops its live connection
  TokenStore::Created tmp;
  std::string e;
  CHECK(t.store.create("tmp", {"read"}, 0, {}, &tmp, &e));
  Conn rv;
  CHECK(ws_connect(rv, port, tmp.secret, tls));
  CHECK(t.store.revoke(tmp.info.id));
  Frame fr;
  CHECK(read_frame(rv, &fr, 6000) && fr.op == 8 && close_code(fr) == 1008);

  // normal close from the client
  CHECK(a.c.send(ws_frame(8, std::string("\x03\xe8", 2))));
  CHECK(read_frame(a, &fr) && fr.op == 8 && close_code(fr) == 1000);
  CHECK(a.fill(2000) < 0);
  for (int i = 0; i < 50 && s.ws_client_count() > 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(s.ws_client_count() == 2);  // adm + d1 remain
}

ApiConfig base_config() {
  ApiConfig c;
  c.lockout_failures = 1000;
  c.allowed_origins = {"https://app.example"};
  return c;
}

void test_server(const std::string& dir) {
  Tokens t;
  // ---- plain HTTP on loopback
  {
    ApiServer s(t.store, base_config());
    add_test_routes(s);
    std::string err;
    CHECK(s.start(&err));
    CHECK(s.running() && s.port() != 0 && !s.tls_enabled());
    CHECK(!s.start(&err));  // already running
    exercise_http(s.port(), false, t);
    exercise_ws(s, s.port(), false, t);

    // CORS allowlist
    Http h = once(s.port(), req("OPTIONS", "/v1/health", "", "", "Origin: https://app.example\r\n"));
    CHECK(h.status == 204 && h.headers["access-control-allow-origin"] == "https://app.example" &&
          h.headers["access-control-allow-headers"].find("Authorization") != std::string::npos);
    CHECK(once(s.port(), req("OPTIONS", "/v1/health", "", "", "Origin: https://evil.example\r\n")).status == 403);
    h = once(s.port(), req("GET", "/v1/health", "", "", "Origin: https://app.example\r\n"));
    CHECK(h.status == 200 && h.headers["access-control-allow-origin"] == "https://app.example" && h.headers["vary"] == "Origin");
    h = once(s.port(), req("GET", "/v1/health", "", "", "Origin: https://evil.example\r\n"));
    CHECK(h.status == 200 && h.headers.count("access-control-allow-origin") == 0);

    // clean shutdown with open connections: idle HTTP, half-sent request, WebSocket
    Conn idle, half, ws;
    CHECK(idle.open(s.port()) && half.open(s.port()));
    CHECK(half.c.send("GET /v1/health HTT"));
    CHECK(ws_connect(ws, s.port(), t.reader));
    auto t0 = std::chrono::steady_clock::now();
    s.stop();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 2000 && !s.running());
    CHECK(idle.fill(2000) < 0 && half.fill(2000) < 0);
    Frame fr;
    CHECK((read_frame(ws, &fr) && fr.op == 8 && close_code(fr) == 1001) || ws.fill(10) < 0);
    s.stop();  // idempotent
    CHECK(s.ws_client_count() == 0);
  }
  // ---- server restart on the same object is not required; but a fresh one binds fine
  // ---- brute force lockout
  {
    ApiConfig c = base_config();
    c.lockout_failures = 3;
    c.lockout_duration_sec = 30;
    ApiServer s(t.store, c);
    add_test_routes(s);
    CHECK(s.start());
    for (int i = 0; i < 2; ++i) CHECK(once(s.port(), req("GET", "/v1/decks/1/state", "dh_bad")).status == 401);
    CHECK(once(s.port(), req("GET", "/v1/decks/1/state", t.reader)).status == 200);  // success resets the count
    for (int i = 0; i < 2; ++i) CHECK(once(s.port(), req("GET", "/v1/decks/1/state", "dh_bad")).status == 401);
    CHECK(once(s.port(), req("GET", "/v1/decks/1/state", "dh_bad")).status == 401);  // third failure triggers it
    Http h = once(s.port(), req("GET", "/v1/decks/1/state", t.reader));  // even a good token is refused now
    CHECK(h.status == 429 && !h.headers["retry-after"].empty());
    CHECK(once(s.port(), req("GET", "/v1/health")).status == 429);
    Conn w;
    CHECK(!ws_connect(w, s.port(), t.reader));
  }
  // ---- per-token and per-IP rate limits
  {
    ApiConfig c = base_config();
    c.token_rate_per_sec = 0.001;
    c.token_burst = 3;
    ApiServer s(t.store, c);
    add_test_routes(s);
    CHECK(s.start());
    int ok = 0, limited = 0;
    for (int i = 0; i < 6; ++i) {
      int st = once(s.port(), req("GET", "/v1/decks/1/state", t.reader)).status;
      ok += st == 200;
      limited += st == 429;
    }
    CHECK(ok == 3 && limited == 3);
    CHECK(once(s.port(), req("GET", "/v1/decks/1/state", t.deck1)).status == 200);  // other token unaffected
    Conn w;
    CHECK(ws_connect(w, s.port(), t.admin));
    for (int i = 0; i < 4; ++i) CHECK(send_cmd(w, "{\"id\":1,\"cmd\":\"ping\"}") && recv_json(w)["ok"].as_bool() == (i < 3));
  }
  {
    ApiConfig c = base_config();
    c.ip_rate_per_sec = 0.001;
    c.ip_burst = 2;
    ApiServer s(t.store, c);
    add_test_routes(s);
    CHECK(s.start());
    CHECK(once(s.port(), req("GET", "/v1/health")).status == 200);
    CHECK(once(s.port(), req("GET", "/v1/health")).status == 200);
    CHECK(once(s.port(), req("GET", "/v1/health")).status == 429);
  }
  // ---- slow WebSocket client is dropped without blocking publish
  {
    ApiConfig c = base_config();
    c.ws_queue_bytes = 200;
    ApiServer s(t.store, c);
    add_test_routes(s);
    CHECK(s.start());
    Conn w;
    CHECK(ws_connect(w, s.port(), t.reader));
    CHECK(send_cmd(w, "{\"cmd\":\"subscribe\",\"args\":{\"topics\":[\"big\"]}}"));
    CHECK(recv_json(w)["ok"].as_bool());
    auto t0 = std::chrono::steady_clock::now();
    s.publish("big", Json(std::string(4000, 'z')));
    for (int i = 0; i < 1000; ++i) s.publish("big", Json(i));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    Frame fr;
    CHECK(read_frame(w, &fr) && fr.op == 8 && close_code(fr) == 1013);
  }
  // ---- header slowloris / idle timeouts
  {
    ApiConfig c = base_config();
    c.header_timeout_ms = 300;
    c.idle_timeout_ms = 300;
    ApiServer s(t.store, c);
    add_test_routes(s);
    CHECK(s.start());
    Conn slow;
    CHECK(slow.open(s.port()) && slow.c.send("GET /v1/health HTTP/1.1\r\nX-A: b\r\n"));
    Http h;
    CHECK(read_http(slow, &h) && h.status == 408);
    Conn idle;
    CHECK(idle.open(s.port()) && idle.c.send(req("GET", "/v1/health")));
    CHECK(read_http(idle, &h) && h.status == 200);
    CHECK(idle.fill(3000) < 0);  // dropped after the idle timeout
  }
  // ---- binding policy: non-loopback without TLS is refused (nothing is bound)
  {
    for (const char* addr : {"0.0.0.0", "::", "192.168.1.10", "example.com"}) {
      ApiConfig c = base_config();
      c.bind_address = addr;
      ApiServer s(t.store, c);
      std::string err;
      CHECK(!s.start(&err) && err.find("non-loopback") != std::string::npos && !s.running());
    }
    ApiConfig c = base_config();
    c.tls_cert = dir + "/only-cert.pem";  // TLS needs both files
    ApiServer s(t.store, c);
    CHECK(!s.start());
    c.tls_key = dir + "/missing.pem";
    ApiServer s2(t.store, c);
    std::string err;
    CHECK(!s2.start(&err) && !err.empty());
    ApiConfig bad = base_config();
    bad.bind_address = "not an address";
    ApiServer s3(t.store, bad);
    CHECK(!s3.start());
  }
  // ---- TLS
  {
    const std::string cert = dir + "/cert.pem", key = dir + "/key.pem";
    std::string err;
    CHECK(!generate_self_signed(cert, key, {"bad host!"}, &err));
    CHECK(generate_self_signed(cert, key, {"deckhand.local", "192.168.9.9"}, &err));
    struct stat st;
    CHECK(stat(key.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    std::string pem = read_all(cert);
    CHECK(pem.find("BEGIN CERTIFICATE") != std::string::npos && read_all(key).find("PRIVATE KEY") != std::string::npos);
    std::string fp;
    CHECK(certificate_fingerprint(cert, &fp, &err) && fp.size() == 64);
    CHECK(!certificate_fingerprint(dir + "/none.pem", &fp, &err));

    ApiConfig c = base_config();
    c.tls_cert = cert;
    c.tls_key = key;
    ApiServer s(t.store, c);
    add_test_routes(s);
    CHECK(s.start(&err));
    CHECK(s.tls_enabled());
    TestClient tc;
    CHECK(tc.connect("127.0.0.1", s.port(), true, &err));
    CHECK(tc.peer_cert_fingerprint() == fp);                       // pinning works
    CHECK(tc.tls_version() == "TLSv1.2" || tc.tls_version() == "TLSv1.3");
    tc.close();
    TestClient old;
    CHECK(!old.connect("127.0.0.1", s.port(), true, &err, 0x0302));  // TLS 1.1 is refused
    TestClient plain;  // plaintext to a TLS port gets nothing useful
    CHECK(plain.connect("127.0.0.1", s.port(), false, &err));
    plain.send("GET /v1/health HTTP/1.1\r\n\r\n");
    char buf[64];
    CHECK(plain.recv(buf, sizeof buf, 3000) <= 0 || std::strncmp(buf, "HTTP/", 5) != 0);
    exercise_http(s.port(), true, t);
    exercise_ws(s, s.port(), true, t);
  }
}

}  // namespace

int main() {
  test_json();
  test_auth_units();
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("dh_api_test_" + random_hex(6));
  std::filesystem::create_directories(dir);
  test_token_store(dir.string());
  test_server(dir.string());
  std::filesystem::remove_all(dir);
  std::printf("api tests ok\n");
  return 0;
}
