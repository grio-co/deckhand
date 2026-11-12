#include "overlay_server.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "deckhand/log/log.hpp"

namespace deckhand::broadcast {
namespace {

constexpr std::size_t kMaxRequestLine = 2048;
constexpr std::size_t kMaxHead = 8192;
constexpr std::size_t kMaxHeaders = 64;
constexpr std::size_t kMaxClients = 32;
constexpr std::size_t kMaxOut = 512 * 1024;
constexpr std::size_t kMaxArtwork = 8u << 20;
constexpr double kRequestTimeout = 5.0;
constexpr double kHeartbeat = 15.0;

std::string response(int code, const char* reason, const char* ctype, const std::string& body, bool head_only = false) {
  std::string r = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + ctype +
                  "\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n";
  if (code == 405) r += "Allow: GET\r\n";
  r += "\r\n";
  if (!head_only) r += body;
  return r;
}

std::string error_response(int code, const char* reason) {
  return response(code, reason, "text/plain; charset=utf-8", std::string(reason) + "\n");
}

const char* image_type(const std::string& path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return nullptr;
  std::string e = path.substr(dot + 1);
  for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (e == "png") return "image/png";
  if (e == "jpg" || e == "jpeg") return "image/jpeg";
  if (e == "gif") return "image/gif";
  if (e == "webp") return "image/webp";
  return nullptr;
}

}  // namespace

struct OverlayServer::Client {
  int fd = -1;
  std::string in;
  std::string out;
  bool sse = false;
  bool close_after = false;
  bool dead = false;
  double born = 0;
  double last_write = 0;
  std::size_t sse_rx = 0;
};

OverlayServer::OverlayServer() = default;
OverlayServer::~OverlayServer() { stop(); }

bool OverlayServer::start(std::uint16_t port) {
  if (thread_.joinable()) return true;
  listen_fd_ = net::listen_loopback(port, &port_);
  if (listen_fd_ < 0) {
    port_ = 0;
    DH_LOG(Warn, "overlay server: cannot listen on 127.0.0.1:%u", unsigned(port));
    return false;
  }
  stop_ = false;
  thread_ = std::thread([this] { run(); });
  return true;
}

void OverlayServer::stop() {
  if (!thread_.joinable()) return;
  stop_ = true;
  wake_.poke();
  thread_.join();
  net::close_fd(listen_fd_);
  listen_fd_ = -1;
  port_ = 0;
}

void OverlayServer::publish(const std::string& json, const std::string& artwork_path) {
  {
    std::lock_guard<std::mutex> lk(m_);
    json_ = json;
    artwork_ = artwork_path;
    ++version_;
  }
  wake_.poke();
}

void OverlayServer::run() {
  std::vector<Client> clients;
  std::uint64_t seen = 0;

  auto handle_request = [&](Client& c, const std::string& head) {
    const std::size_t eol = head.find("\r\n");
    const std::string line = head.substr(0, eol);
    if (line.size() > kMaxRequestLine) { c.out = error_response(414, "URI Too Long"); return; }
    // Request line: METHOD SP target SP HTTP/1.x
    const std::size_t s1 = line.find(' ');
    const std::size_t s2 = s1 == std::string::npos ? s1 : line.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos || line.find(' ', s2 + 1) != std::string::npos) {
      c.out = error_response(400, "Bad Request");
      return;
    }
    const std::string method = line.substr(0, s1), target = line.substr(s1 + 1, s2 - s1 - 1),
                      version = line.substr(s2 + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") { c.out = error_response(400, "Bad Request"); return; }
    for (char ch : target)
      if (static_cast<unsigned char>(ch) <= 0x20 || ch == 0x7f) { c.out = error_response(400, "Bad Request"); return; }
    if (target.empty() || target[0] != '/') { c.out = error_response(400, "Bad Request"); return; }
    // Headers: well formed, bounded, and no request body.
    std::size_t pos = eol == std::string::npos ? head.size() : eol + 2, count = 0;
    while (pos < head.size()) {
      std::size_t e = head.find("\r\n", pos);
      if (e == std::string::npos) e = head.size();
      const std::string h = head.substr(pos, e - pos);
      pos = e + 2;
      if (h.empty()) break;
      if (++count > kMaxHeaders) { c.out = error_response(431, "Request Header Fields Too Large"); return; }
      const std::size_t colon = h.find(':');
      if (colon == std::string::npos || colon == 0 || h[0] == ' ' || h[0] == '\t') {
        c.out = error_response(400, "Bad Request");
        return;
      }
      std::string name = h.substr(0, colon);
      for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (name == "transfer-encoding") { c.out = error_response(400, "Bad Request"); return; }
      if (name == "content-length") {
        const std::size_t v = h.find_first_of("123456789", colon);
        if (v != std::string::npos) { c.out = error_response(400, "Bad Request"); return; }
      }
    }
    if (method != "GET") { c.out = error_response(405, "Method Not Allowed"); return; }
    const std::string path = target.substr(0, target.find('?'));

    std::string json, artwork;
    std::uint64_t ver;
    {
      std::lock_guard<std::mutex> lk(m_);
      json = json_;
      artwork = artwork_;
      ver = version_;
    }
    if (path == "/" || path == "/overlay") {
      c.out = response(200, "OK", "text/html; charset=utf-8", overlay_html());
      // The page only talks to this origin.
      c.out.insert(c.out.find("\r\n\r\n"),
                   "\r\nContent-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
                   "script-src 'unsafe-inline'; connect-src 'self'; img-src 'self'");
    } else if (path == "/now-playing.json") {
      c.out = response(200, "OK", "application/json; charset=utf-8", json);
    } else if (path == "/events") {
      c.out = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
              "X-Content-Type-Options: nosniff\r\nConnection: keep-alive\r\n\r\nretry: 2000\n\ndata: " +
              json + "\n\n";
      c.sse = true;
      seen = ver;
    } else if (path == "/artwork") {
      const char* type = artwork.empty() ? nullptr : image_type(artwork);
      std::string body;
      if (type) {
        std::ifstream f(artwork, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        body = ss.str();
      }
      if (!type || body.empty() || body.size() > kMaxArtwork) c.out = error_response(404, "Not Found");
      else c.out = response(200, "OK", type, body);
    } else {
      c.out = error_response(404, "Not Found");
    }
  };

  while (!stop_) {
    std::vector<net::PollItem> items;
    items.push_back({wake_.fd(), true, false});
    items.push_back({listen_fd_, true, false});
    for (auto& c : clients) items.push_back({c.fd, true, !c.out.empty()});
    if (net::poll(items, 1000) < 0) break;
    if (stop_) break;
    if (items[0].readable) wake_.drain();
    const double now = net::now_seconds();

    // Push a changed payload to event streams.
    std::string payload;
    bool changed = false;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (version_ != seen) {
        seen = version_;
        payload = json_;
        changed = true;
      }
    }
    if (changed) {
      const std::string ev = "data: " + payload + "\n\n";
      for (auto& c : clients) {
        if (!c.sse) continue;
        if (c.out.size() + ev.size() > kMaxOut) c.dead = true;  // reader too slow
        else { c.out += ev; c.last_write = now; }
      }
    }

    const std::size_t existing = clients.size();
    for (std::size_t i = 0; i < existing; ++i) {
      Client& c = clients[i];
      if (items[i + 2].readable && !c.dead) {
        char buf[4096];
        const long n = net::recv_some(c.fd, buf, sizeof buf);
        if (n < 0) {
          c.dead = true;
        } else if (n > 0 && c.sse) {
          c.sse_rx += static_cast<std::size_t>(n);  // clients of a stream have nothing to say
          if (c.sse_rx > 4096) c.dead = true;
        } else if (n > 0 && c.out.empty() && !c.close_after) {
          c.in.append(buf, static_cast<std::size_t>(n));
          const std::size_t end = c.in.find("\r\n\r\n");
          if (end != std::string::npos) {
            if (end > kMaxHead) c.out = error_response(431, "Request Header Fields Too Large");
            else handle_request(c, c.in.substr(0, end + 2));
            c.close_after = !c.sse;
            c.in.clear();
            c.last_write = now;
          } else if (c.in.size() > kMaxHead) {
            c.out = error_response(431, "Request Header Fields Too Large");
            c.close_after = true;
            c.in.clear();
          }
        }
      }
      if (!c.dead && c.sse && now - c.last_write > kHeartbeat) {
        c.out += ": ping\n\n";
        c.last_write = now;
      }
      if (!c.dead && !c.out.empty()) {
        const long n = net::send_some(c.fd, c.out.data(), c.out.size());
        if (n < 0) c.dead = true;
        else c.out.erase(0, static_cast<std::size_t>(n));
      }
      if (!c.dead && c.close_after && c.out.empty()) c.dead = true;
      if (!c.dead && !c.sse && now - c.born > kRequestTimeout) c.dead = true;  // slow or idle request
    }

    if (items[1].readable) {
      for (int i = 0; i < 16; ++i) {
        const int fd = net::accept_client(listen_fd_);
        if (fd < 0) break;
        if (clients.size() >= kMaxClients) { net::close_fd(fd); continue; }
        Client c;
        c.fd = fd;
        c.born = c.last_write = now;
        clients.push_back(std::move(c));
      }
    }

    for (std::size_t i = 0; i < clients.size();) {
      if (clients[i].dead) {
        net::close_fd(clients[i].fd);
        clients.erase(clients.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
  }
  for (auto& c : clients) net::close_fd(c.fd);
}

}  // namespace deckhand::broadcast
