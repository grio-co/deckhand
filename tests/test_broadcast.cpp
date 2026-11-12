#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>

#include "check.hpp"
#include "deckhand/broadcast/automation.hpp"
#include "deckhand/broadcast/crypto.hpp"
#include "deckhand/broadcast/json.hpp"
#include "deckhand/broadcast/nowplaying.hpp"
#include "deckhand/broadcast/obs.hpp"
#include "deckhand/broadcast/websocket.hpp"

using namespace deckhand::broadcast;
namespace fs = std::filesystem;

namespace {

// Vector for the OBS auth string, computed independently with Python hashlib / openssl.
const char* kPassword = "supersecretpassword";
const char* kSalt = "lM1GncleQOaCu9lT1yeUZhFYnqhsLLP1G5lAGo3ixaI=";
const char* kChallenge = "+IxH4CnCiqpX1rM9scsNynZzbOe4KhkdAdypRL0S9ug=";
const char* kAuth = "Gau6oHeMms/AJPPG5+MWXUP3imS2vr1BWmUbQFZzpKk=";

bool wait_for(const std::function<bool()>& f, double sec) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(sec);
  while (std::chrono::steady_clock::now() < end) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

// ---------- crypto / json / websocket ----------

void test_crypto() {
  CHECK(to_hex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(to_hex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(to_hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(to_hex(sha256(std::string(1000000, 'a'))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  CHECK(to_hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(to_hex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  const char* in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
  const char* out[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
  for (int i = 0; i < 7; ++i) {
    CHECK(base64_encode(in[i]) == out[i]);
    std::string back;
    CHECK(base64_decode(out[i], &back) && back == in[i]);
  }
  std::string tmp;
  CHECK(!base64_decode("Zm9v!", &tmp));
  CHECK(obs_auth_string(kPassword, kSalt, kChallenge) == kAuth);
  // RFC 6455 section 1.3 example.
  CHECK(websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

void test_json() {
  auto j = Json::parse(R"({"a":1,"b":[true,null,"x\n\u00e9\ud83d\ude00"],"c":{"d":-2.5e1}})");
  CHECK(j);
  CHECK((*j)["a"].as_number() == 1);
  CHECK((*j)["b"].size() == 3 && (*j)["b"].at(0).as_bool());
  CHECK((*j)["b"].at(2).as_string() == "x\n\xc3\xa9\xf0\x9f\x98\x80");
  CHECK((*j)["c"]["d"].as_number() == -25);
  CHECK((*j)["missing"]["deeper"].is_null());
  auto again = Json::parse(j->dump());
  CHECK(again && again->dump() == j->dump());
  CHECK(!Json::parse("{\"a\":}") && !Json::parse("[1,2") && !Json::parse("{} x") && !Json::parse("\"\\q\""));
  CHECK(!Json::parse(std::string(200, '[') + std::string(200, ']')));  // depth limit
  CHECK(Json::quote("a\"b\\c\x01") == "\"a\\\"b\\\\c\\u0001\"");
  CHECK(Json::quote("ok\xff") == "\"ok\xef\xbf\xbd\"");  // invalid UTF-8 never leaks into the output
  CHECK(Json(3).dump() == "3" && Json(2.5).dump() == "2.5");
}

void test_websocket() {
  for (std::size_t n : {0u, 5u, 125u, 126u, 70000u}) {
    const std::string payload(n, 'x');
    const std::string frame = ws_encode(WsOpcode::Text, payload, true);
    WsParser p;
    WsMessage m;
    for (std::size_t i = 0; i + 1 < frame.size(); ++i) {  // byte by byte: no message until complete
      p.feed(&frame[i], 1);
      CHECK(!p.next(&m));
    }
    p.feed(&frame.back(), 1);
    CHECK(p.next(&m) && m.opcode == WsOpcode::Text && m.payload == payload);
  }
  // Fragmented message with an interleaved ping.
  std::string wire;
  wire += std::string("\x01\x03", 2) + "abc";
  wire += ws_encode(WsOpcode::Ping, "p", false);
  wire += std::string("\x80\x02", 2) + "de";
  WsParser p;
  p.feed(wire.data(), wire.size());
  WsMessage m;
  CHECK(p.next(&m) && m.opcode == WsOpcode::Ping);
  CHECK(p.next(&m) && m.opcode == WsOpcode::Text && m.payload == "abcde");
  WsParser bad;
  bad.feed("\xf1\x00", 2);  // reserved bits set
  CHECK(!bad.next(&m) && bad.failed());
}

// ---------- now playing ----------

DeckState deck(const std::string& title, bool playing, double audible, const std::string& artist = "Artist") {
  DeckState d;
  d.loaded = true;
  d.playing = playing;
  d.audible = audible;
  d.title = title;
  d.artist = artist;
  d.bpm = 128;
  d.key = "8A";
  return d;
}
NowPlayingState two(DeckState a, DeckState b) {
  NowPlayingState s;
  s.decks = {std::move(a), std::move(b)};
  return s;
}

void test_onair() {
  {  // simple A -> B mix
    NowPlaying np;
    int changes = 0;
    np.on_track_change([&](const TrackInfo&) { ++changes; });
    for (double t = 0; t < 7.5; t += 0.5) np.update(two(deck("A", true, 1), deck("B", false, 0)), t);
    CHECK(!np.current() && changes == 0);  // not on air before the hold time
    np.update(two(deck("A", true, 1), deck("B", false, 0)), 8);
    CHECK(np.current() && np.current()->title == "A" && np.current()->deck == 1 && changes == 1);
    for (double t = 10; t < 30; t += 1) np.update(two(deck("A", true, 1), deck("B", true, 0.3)), t);
    CHECK(np.current()->title == "A");
    // B equal to A (full blend) is not enough; B clearly louder starting at t=30.
    for (double t = 30; t < 37.5; t += 0.5) np.update(two(deck("A", true, 0.4), deck("B", true, 0.8)), t);
    CHECK(np.current()->title == "A" && changes == 1);
    np.update(two(deck("A", true, 0.2), deck("B", true, 1)), 38);
    CHECK(np.current()->title == "B" && np.current()->deck == 2 && changes == 2);
    CHECK(np.tracklist_text() == "00:00 Artist - A\n00:30 Artist - B\n");
  }
  {  // quick back-and-forth cut never changes the on-air track
    NowPlaying np;
    int changes = 0;
    np.on_track_change([&](const TrackInfo&) { ++changes; });
    np.update(two(deck("A", true, 1), deck("B", false, 0)), 0);
    np.update(two(deck("A", true, 1), deck("B", false, 0)), 9);
    for (int i = 0; i < 40; ++i) {  // B and A alternate every second for 40 s
      const bool b = i % 2 == 0;
      np.update(two(deck("A", true, b ? 0 : 1), deck("B", true, b ? 1 : 0)), 10 + i);
    }
    CHECK(changes == 1 && np.current()->title == "A");
  }
  {  // both paused: the last track stays; a new one needs its own hold time
    NowPlaying np;
    np.update(two(deck("A", true, 1), deck("B", false, 0)), 0);
    np.update(two(deck("A", true, 1), deck("B", false, 0)), 8);
    np.update(two(deck("A", false, 1), deck("B", false, 1)), 20);
    CHECK(np.current() && np.current()->title == "A");
    np.update(two(deck("A", false, 1), deck("B", true, 1)), 30);
    np.update(two(deck("A", false, 1), deck("B", true, 1)), 37);
    CHECK(np.current()->title == "A");
    np.update(two(deck("A", false, 1), deck("B", true, 1)), 38);
    CHECK(np.current()->title == "B");
  }
  {  // same track reloaded on the other deck: no new tracklist entry, deck follows
    NowPlaying np;
    int changes = 0;
    np.on_track_change([&](const TrackInfo&) { ++changes; });
    np.update(two(deck("X", true, 1), deck("", false, 0)), 0);
    np.update(two(deck("X", true, 1), deck("", false, 0)), 8);
    for (double t = 10; t < 40; t += 1) np.update(two(deck("X", true, 0), deck("X", true, 1)), t);
    CHECK(changes == 1 && np.tracklist().size() == 1);
    CHECK(np.current()->deck == 2);
  }
  {  // hold 0 = immediate; fader-down deck is not "audible"
    NowPlayingConfig c;
    c.hold_seconds = 0;
    NowPlaying np(c);
    np.update(two(deck("A", true, 0.01), deck("B", false, 0)), 0);
    CHECK(!np.current());
    np.update(two(deck("A", true, 0.5), deck("B", false, 0)), 1);
    CHECK(np.current()->title == "A");
  }
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void test_outputs() {
  TrackInfo t;
  t.deck = 2;
  t.title = "Jóga";
  t.artist = "Björk";
  t.album = "Homogenic";
  t.key = "5A";
  t.bpm = 128;
  CHECK(NowPlaying::render("{artist} - {title} [{key} {bpm}] d{deck} {nope} {", t) ==
        "Björk - Jóga [5A 128.0] d2 {nope} {");

  const fs::path dir = fs::temp_directory_path() / ("dh-broadcast-test-" + std::to_string(::getpid()));
  fs::remove_all(dir);
  NowPlayingConfig c;
  c.hold_seconds = 0;
  c.text_template = "{artist} \xe2\x80\x93 {title} ({bpm})";
  c.files.enabled = true;
  c.files.folder = (dir / "out").string();
  NowPlaying np(c);
  CHECK(fs::exists(dir / "out" / "now-playing.txt"));  // exists (empty) before the first track
  NowPlayingState s;
  s.decks.push_back(deck("Jóga 日本", true, 1, "Björk"));
  s.decks[0].artwork = "/tmp/art.png";
  np.update(s, 0);
  CHECK(slurp(dir / "out" / "title.txt") == "Jóga 日本");
  CHECK(slurp(dir / "out" / "artist.txt") == "Björk");
  CHECK(slurp(dir / "out" / "now-playing.txt") == "Björk \xe2\x80\x93 Jóga 日本 (128.0)");
  auto j = Json::parse(slurp(dir / "out" / "now-playing.json"));
  CHECK(j && (*j)["title"].as_string() == "Jóga 日本" && (*j)["on_air"].as_bool() && (*j)["bpm"].as_number() == 128);
  CHECK(slurp(dir / "out" / "tracklist.txt") == "00:00 Björk - Jóga 日本\n");
  for (auto& e : fs::directory_iterator(dir / "out")) CHECK(e.path().extension() != ".tmp");  // atomic: no leftovers

  // Tracklist stamps: mm:ss, then h:mm:ss after an hour.
  s.decks[0] = deck("B", true, 1);
  np.update(s, 125);
  s.decks[0] = deck("C", true, 1);
  np.update(s, 3725);
  CHECK(slurp(dir / "out" / "tracklist.txt") == "00:00 Björk - Jóga 日本\n02:05 Artist - B\n1:02:05 Artist - C\n");
  np.reset();
  CHECK(slurp(dir / "out" / "now-playing.txt").empty() && slurp(dir / "out" / "tracklist.txt").empty());
  fs::remove_all(dir);
}

// ---------- overlay HTTP ----------

int dial(std::uint16_t port, int rcv_ms = 3000) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
  timeval tv{rcv_ms / 1000, (rcv_ms % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  return fd;
}

void send_raw(int fd, const std::string& s) {
#ifdef MSG_NOSIGNAL
  ::send(fd, s.data(), s.size(), MSG_NOSIGNAL);
#else
  ::send(fd, s.data(), s.size(), 0);
#endif
}

std::string http_exchange(std::uint16_t port, const std::string& raw) {
  const int fd = dial(port);
  send_raw(fd, raw);
  std::string resp;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    resp.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return resp;
}

bool read_until(int fd, const std::string& needle, std::string& acc, double sec) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(sec);
  char buf[4096];
  while (acc.find(needle) == std::string::npos && std::chrono::steady_clock::now() < end) {
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n == 0) break;
    if (n > 0) acc.append(buf, static_cast<std::size_t>(n));
  }
  return acc.find(needle) != std::string::npos;
}

void test_http() {
  NowPlayingConfig c;
  c.hold_seconds = 0;
  c.http.enabled = true;
  c.http.port = 0;
  NowPlaying np(c);
  const std::uint16_t port = np.http_port();
  CHECK(port != 0);
  NowPlayingState s;
  s.decks.push_back(deck("First <b>", true, 1));
  np.update(s, 0);

  const std::string page = http_exchange(port, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
  CHECK(page.rfind("HTTP/1.1 200", 0) == 0);
  CHECK(page.find("text/html") != std::string::npos && page.find("EventSource") != std::string::npos);
  CHECK(page.find("Content-Security-Policy") != std::string::npos && page.find("background:transparent") != std::string::npos);
  CHECK(http_exchange(port, "GET /?theme=light&size=40&align=center HTTP/1.0\r\n\r\n").rfind("HTTP/1.1 200", 0) == 0);

  const std::string js = http_exchange(port, "GET /now-playing.json HTTP/1.1\r\n\r\n");
  const auto body = js.substr(js.find("\r\n\r\n") + 4);
  auto j = Json::parse(body);
  CHECK(j && (*j)["title"].as_string() == "First <b>" && (*j)["on_air"].as_bool());

  // One server-sent event after the initial one.
  const int sse = dial(port, 200);
  send_raw(sse, "GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n");
  std::string acc;
  CHECK(read_until(sse, "First", acc, 3));
  CHECK(acc.find("text/event-stream") != std::string::npos);
  s.decks[0] = deck("Second", true, 1);
  np.update(s, 1);
  CHECK(read_until(sse, "Second", acc, 3));
  ::close(sse);

  // Rejections.
  CHECK(http_exchange(port, "BOGUS\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
  CHECK(http_exchange(port, "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n").rfind("HTTP/1.1 405", 0) == 0);
  CHECK(http_exchange(port, "GET / HTTP/2\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
  CHECK(http_exchange(port, "GET / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello").rfind("HTTP/1.1 400", 0) == 0);
  CHECK(http_exchange(port, "GET / HTTP/1.1\r\nbroken header\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
  CHECK(http_exchange(port, "GET /" + std::string(3000, 'a') + " HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 414", 0) == 0);
  CHECK(http_exchange(port, "GET / HTTP/1.1\r\nX: " + std::string(9000, 'a')).rfind("HTTP/1.1 431", 0) == 0);
  CHECK(http_exchange(port, "GET /../etc/passwd HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 404", 0) == 0);
  CHECK(http_exchange(port, "GET /artwork HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 404", 0) == 0);
  // Still serving afterwards.
  CHECK(http_exchange(port, "GET /now-playing.json HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200", 0) == 0);
}

// ---------- mock OBS v5 server ----------

class MockObs {
 public:
  explicit MockObs(bool with_password) : with_password_(with_password) {
    lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::listen(lfd_, 4) == 0);
    socklen_t len = sizeof a;
    getsockname(lfd_, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    th_ = std::thread([this] { loop(); });
  }
  ~MockObs() {
    stop_ = true;
    th_.join();
    ::close(lfd_);
  }
  std::uint16_t port = 0;
  std::mutex m;
  int connections = 0;
  int auth_ok = 0, auth_bad = 0;
  std::vector<std::string> log;  // "Type|requestData"

  std::vector<std::string> snapshot() {
    std::lock_guard<std::mutex> lk(m);
    return log;
  }

 private:
  bool send_ws(int fd, WsOpcode op, const std::string& p) {
    const std::string f = ws_encode(op, p, false);
    std::size_t off = 0;
    while (off < f.size()) {
#ifdef MSG_NOSIGNAL
      const ssize_t n = ::send(fd, f.data() + off, f.size() - off, MSG_NOSIGNAL);
#else
      const ssize_t n = ::send(fd, f.data() + off, f.size() - off, 0);
#endif
      if (n <= 0) return false;
      off += static_cast<std::size_t>(n);
    }
    return true;
  }
  bool recv_msg(int fd, WsParser& p, WsMessage* m) {
    char buf[4096];
    while (!stop_) {
      if (p.next(m)) return true;
      if (p.failed()) return false;
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n == 0) return false;
      if (n > 0) p.feed(buf, static_cast<std::size_t>(n));
    }
    return false;
  }

  void loop() {
    while (!stop_) {
      pollfd p{lfd_, POLLIN, 0};
      if (::poll(&p, 1, 50) <= 0) continue;
      const int fd = ::accept(lfd_, nullptr, nullptr);
      if (fd < 0) continue;
      timeval tv{0, 100000};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
      const int one = 1;
      setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
      serve(fd);
      ::close(fd);
    }
  }

  void serve(int fd) {
    std::string head;
    char buf[2048];
    while (head.find("\r\n\r\n") == std::string::npos && !stop_) {
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n == 0) return;
      if (n > 0) head.append(buf, static_cast<std::size_t>(n));
    }
    const auto k = head.find("Sec-WebSocket-Key: ");
    if (k == std::string::npos || head.find("obswebsocket.json") == std::string::npos) return;
    const std::string key = head.substr(k + 19, head.find("\r\n", k) - k - 19);
    const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " + websocket_accept_key(key) +
                             "\r\nSec-WebSocket-Protocol: obswebsocket.json\r\n\r\n";
    ::send(fd, resp.data(), resp.size(), 0);
    {
      std::lock_guard<std::mutex> lk(m);
      ++connections;
    }
    Json hd = Json::object();
    hd.set("obsWebSocketVersion", Json("30.0.0"));
    hd.set("rpcVersion", Json(1));
    if (with_password_) {
      Json a = Json::object();
      a.set("challenge", Json(kChallenge));
      a.set("salt", Json(kSalt));
      hd.set("authentication", std::move(a));
    }
    Json hello = Json::object();
    hello.set("op", Json(0));
    hello.set("d", std::move(hd));
    if (!send_ws(fd, WsOpcode::Text, hello.dump())) return;

    WsParser parser;
    WsMessage m;
    if (!recv_msg(fd, parser, &m)) return;
    auto ident = Json::parse(m.payload);
    if (!ident || (*ident)["op"].as_number() != 1 || (*ident)["d"]["rpcVersion"].as_number() != 1) return;
    const std::string auth = (*ident)["d"]["authentication"].as_string();
    if (with_password_ && auth != kAuth) {
      {
        std::lock_guard<std::mutex> lk(this->m);
        ++auth_bad;
      }
      send_ws(fd, WsOpcode::Close, std::string("\x0f\xa9", 2) + "Authentication failed.");
      return;
    }
    {
      std::lock_guard<std::mutex> lk(this->m);
      ++auth_ok;
    }
    send_ws(fd, WsOpcode::Text, R"({"op":2,"d":{"negotiatedRpcVersion":1}})");

    while (recv_msg(fd, parser, &m)) {
      if (m.opcode == WsOpcode::Ping) { send_ws(fd, WsOpcode::Pong, m.payload); continue; }
      if (m.opcode == WsOpcode::Close) return;
      auto j = Json::parse(m.payload);
      if (!j || (*j)["op"].as_number() != 6) continue;
      const Json& d = (*j)["d"];
      const std::string type = d["requestType"].as_string();
      {
        std::lock_guard<std::mutex> lk(this->m);
        log.push_back(type + "|" + d["requestData"].dump());
      }
      if (type == "NeverAnswer") continue;
      if (type == "DropMe") return;
      Json data = Json::object();
      if (type == "GetVersion") data.set("obsWebSocketVersion", Json("30.0.0"));
      if (type == "GetSceneList") {
        Json arr = Json::array();
        for (const char* n : {"Intro", "Live"}) {
          Json sc = Json::object();
          sc.set("sceneName", Json(n));
          arr.push(std::move(sc));
        }
        data.set("scenes", std::move(arr));
        data.set("currentProgramSceneName", Json("Intro"));
      }
      if (type == "GetSceneItemId") data.set("sceneItemId", Json(7));
      Json st = Json::object();
      st.set("result", Json(true));
      st.set("code", Json(100));
      Json rd = Json::object();
      rd.set("requestType", Json(type));
      rd.set("requestId", Json(d["requestId"].as_string()));
      rd.set("requestStatus", std::move(st));
      rd.set("responseData", std::move(data));
      Json out = Json::object();
      out.set("op", Json(7));
      out.set("d", std::move(rd));
      if (!send_ws(fd, WsOpcode::Text, out.dump())) return;
      if (type == "TriggerEvent")
        send_ws(fd, WsOpcode::Text,
                R"({"op":5,"d":{"eventType":"CurrentProgramSceneChanged","eventIntent":4,"eventData":{"sceneName":"Live"}}})");
    }
  }

  bool with_password_;
  int lfd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread th_;
};

ObsResponse call(ObsClient& c, const std::string& type, const std::string& data = "") {
  auto pr = std::make_shared<std::promise<ObsResponse>>();
  auto fut = pr->get_future();
  c.request(type, data, [pr](const ObsResponse& r) { pr->set_value(r); });
  CHECK(fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  return fut.get();
}

ObsConfig cfg_for(const MockObs& mock, const std::string& password) {
  ObsConfig c;
  c.port = mock.port;
  c.password = password;
  c.reconnect_min = 0.05;
  c.reconnect_max = 0.2;
  c.request_timeout = 0.6;
  return c;
}

void test_obs_plain() {
  MockObs mock(false);
  ObsClient obs(cfg_for(mock, ""));
  std::atomic<int> ups{0}, downs{0};
  std::mutex em;
  std::vector<std::string> events;
  obs.on_connection([&](bool up) { ++(up ? ups : downs); });
  obs.on_event([&](const std::string& type, const Json& data) {
    std::lock_guard<std::mutex> lk(em);
    events.push_back(type + ":" + data["sceneName"].as_string());
  });
  CHECK(call(obs, "GetVersion").error == "not started");  // before start(): immediate, never hangs
  obs.start();
  CHECK(wait_for([&] { return obs.connected(); }, 3));
  CHECK(mock.auth_ok == 1 && ups == 1);

  auto v = call(obs, "GetVersion");
  CHECK(v.ok && v.code == 100 && v.data["obsWebSocketVersion"].as_string() == "30.0.0");

  // Scene switch request/response through the typed helper.
  auto pr = std::make_shared<std::promise<ObsResponse>>();
  obs.set_current_scene("Live", [pr](const ObsResponse& r) { pr->set_value(r); });
  CHECK(pr->get_future().get().ok);
  CHECK(mock.snapshot().back() == "SetCurrentProgramScene|{\"sceneName\":\"Live\"}");

  auto sc = std::make_shared<std::promise<std::vector<std::string>>>();
  obs.list_scenes([sc](const ObsResponse&, const std::vector<std::string>& names, const std::string& cur) {
    auto n = names;
    n.push_back("current=" + cur);
    sc->set_value(n);
  });
  auto names = sc->get_future().get();
  CHECK(names.size() == 3 && names[0] == "Intro" && names[1] == "Live" && names[2] == "current=Intro");

  obs.set_text("Now Playing", "A \"quoted\" \xc3\xa9");
  auto bad = call(obs, "SetCurrentProgramScene", "{not json");
  CHECK(!bad.ok && bad.error == "bad request json");
  CHECK(wait_for([&] { return mock.snapshot().back().rfind("SetInputSettings|", 0) == 0; }, 2));
  auto set = Json::parse(mock.snapshot().back().substr(mock.snapshot().back().find('|') + 1));
  CHECK(set && (*set)["inputName"].as_string() == "Now Playing" &&
        (*set)["inputSettings"]["text"].as_string() == "A \"quoted\" \xc3\xa9");

  auto vis = std::make_shared<std::promise<ObsResponse>>();
  obs.set_source_visible("Live", "Banner", true, [vis](const ObsResponse& r) { vis->set_value(r); });
  CHECK(vis->get_future().get().ok);
  CHECK(mock.snapshot().back() == "SetSceneItemEnabled|{\"sceneName\":\"Live\",\"sceneItemId\":7,\"sceneItemEnabled\":true}");

  // Event delivery.
  CHECK(call(obs, "TriggerEvent").ok);
  CHECK(wait_for([&] { std::lock_guard<std::mutex> lk(em); return !events.empty(); }, 2));
  {
    std::lock_guard<std::mutex> lk(em);
    CHECK(events[0] == "CurrentProgramSceneChanged:Live");
  }

  // Timeout: the answer never comes.
  const auto t0 = std::chrono::steady_clock::now();
  auto to = call(obs, "NeverAnswer");
  CHECK(!to.ok && to.error == "timeout");
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
  CHECK(obs.connected());

  // Reconnect after the server drops the link.
  auto dropped = call(obs, "DropMe");
  CHECK(!dropped.ok && dropped.error == "disconnected");
  CHECK(wait_for([&] { return mock.connections >= 2 && obs.connected(); }, 3));
  CHECK(downs >= 1 && ups >= 2);
  CHECK(call(obs, "GetVersion").ok);

  obs.stop();
  CHECK(obs.state() == ObsState::Stopped);
  CHECK(call(obs, "GetVersion").error == "not started");
}

void test_obs_auth() {
  {
    MockObs mock(true);
    ObsClient obs(cfg_for(mock, kPassword));
    obs.start();
    CHECK(wait_for([&] { return obs.connected(); }, 3));
    CHECK(mock.auth_ok == 1 && mock.auth_bad == 0);
    CHECK(call(obs, "GetVersion").ok);
  }
  {
    MockObs mock(true);
    ObsClient obs(cfg_for(mock, "wrong"));
    obs.start();
    CHECK(wait_for([&] { return mock.auth_bad >= 2; }, 3));  // keeps retrying with backoff
    CHECK(!obs.connected() && obs.last_error() == "authentication failed");
    auto r = call(obs, "GetVersion");
    CHECK(!r.ok && (r.error == "not connected" || r.error == "timeout"));
  }
  {
    MockObs mock(true);
    ObsClient obs(cfg_for(mock, ""));
    obs.start();
    CHECK(wait_for([&] { return obs.last_error() == "OBS requires a password"; }, 3));
  }
  {  // nothing listening: fails and keeps trying, stop() returns promptly
    ObsConfig c;
    c.port = 1;
    c.reconnect_min = 0.05;
    ObsClient obs(c);
    obs.start();
    CHECK(wait_for([&] { return !obs.last_error().empty(); }, 3));
    const auto t0 = std::chrono::steady_clock::now();
    obs.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
  }
}

// ---------- automation ----------

void test_automation() {
  std::vector<std::pair<std::string, std::string>> sent;
  Automation a([&](const std::string& type, const std::string& json, ObsCallback cb) {
    sent.push_back({type, json});
    if (cb) {
      ObsResponse r;
      r.ok = true;
      r.data.set("sceneItemId", Json(7));
      cb(r);
    }
  });
  AutomationRule text;
  text.action = AutomationRule::Action::SetText;
  text.source = "Track";
  text.text = "{artist}: {title}";
  AutomationRule scene;
  scene.trigger = AutomationRule::Trigger::DeckStart;
  scene.deck = 2;
  scene.action = AutomationRule::Action::SwitchScene;
  scene.scene = "Deck B cam";
  AutomationRule stop;
  stop.trigger = AutomationRule::Trigger::DeckStop;
  stop.action = AutomationRule::Action::SwitchScene;
  stop.scene = "Idle";
  AutomationRule banner;
  banner.action = AutomationRule::Action::ShowSource;
  banner.scene = "Live";
  banner.source = "Banner";
  banner.seconds = 3;
  a.set_rules({text, scene, stop, banner});

  TrackInfo t;
  t.deck = 1;
  t.title = "T";
  t.artist = "Ar";
  a.update(two(deck("T", false, 0), deck("U", false, 0)), 0);  // baseline: no edges
  CHECK(sent.empty());
  a.on_track_change(t);
  CHECK(sent.size() == 3);
  CHECK(sent[0].first == "SetInputSettings" && Json::parse(sent[0].second)->operator[]("inputSettings")["text"].as_string() == "Ar: T");
  CHECK(sent[1].first == "GetSceneItemId" && sent[2].first == "SetSceneItemEnabled");
  CHECK(sent[2].second.find("\"sceneItemEnabled\":true") != std::string::npos);
  sent.clear();
  a.update(two(deck("T", true, 1), deck("U", false, 0)), 1);  // deck 1 starts: the rule is for deck 2
  CHECK(sent.empty());
  a.update(two(deck("T", true, 1), deck("U", true, 1)), 2);
  CHECK(sent.size() == 1 && sent[0].second.find("Deck B cam") != std::string::npos);
  sent.clear();
  a.update(two(deck("T", true, 1), deck("U", true, 1)), 2.9);  // banner shown at clock 0 for 3 s: still visible
  CHECK(sent.empty());
  a.update(two(deck("T", true, 1), deck("U", true, 1)), 3.5);  // time is up: hidden again
  CHECK(sent.size() == 1 && sent[0].second.find("\"sceneItemEnabled\":false") != std::string::npos);
  sent.clear();
  a.update(two(deck("T", false, 1), deck("U", true, 1)), 4.0);  // deck 1 stops
  CHECK(sent.size() == 1 && sent[0].second.find("Idle") != std::string::npos);
  sent.clear();
  a.tick(200);
  CHECK(sent.empty());
}

}  // namespace

int main() {
  test_crypto();
  test_json();
  test_websocket();
  test_onair();
  test_outputs();
  test_http();
  test_obs_plain();
  test_obs_auth();
  test_automation();
  std::puts("test_broadcast: ok");
  return 0;
}
