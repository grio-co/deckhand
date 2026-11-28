#include "http_parse.hpp"

#include <cctype>
#include <cstdlib>

namespace deckhand::metadata::http {

bool parse_https_url(const std::string& url, Url* out) {
  static const std::string scheme = "https://";
  if (url.compare(0, scheme.size(), scheme) != 0) return false;
  const std::size_t hs = scheme.size();
  const std::size_t he = url.find_first_of("/?#", hs);
  std::string hostport = url.substr(hs, he == std::string::npos ? std::string::npos : he - hs);
  Url u;
  const std::size_t colon = hostport.find(':');
  if (colon != std::string::npos) {
    const std::string p = hostport.substr(colon + 1);
    if (p.empty() || p.size() > 5) return false;
    for (char c : p)
      if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    u.port = std::atoi(p.c_str());
    if (u.port < 1 || u.port > 65535) return false;
    hostport.erase(colon);
  }
  if (hostport.empty() || hostport.size() > 253) return false;
  for (char c : hostport)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return false;
  u.host = hostport;
  std::string t = he == std::string::npos ? "/" : url.substr(he);
  const std::size_t frag = t.find('#');
  if (frag != std::string::npos) t.erase(frag);
  if (t.empty() || t[0] != '/') t.insert(0, "/");
  for (char c : t)
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return false;  // no CR/LF/space injection
  u.target = t;
  *out = u;
  return true;
}

std::string build_request(const Url& u, const std::string& user_agent) {
  std::string ua;
  for (char c : user_agent)
    if (c != '\r' && c != '\n') ua += c;
  return "GET " + u.target + " HTTP/1.1\r\nHost: " + u.host + (u.port == 443 ? "" : ":" + std::to_string(u.port)) +
         "\r\nUser-Agent: " + ua + "\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
}

bool parse_head(const std::string& raw, Head* out) {
  Head h;
  const std::size_t eol = raw.find("\r\n");
  const std::string line = raw.substr(0, eol);
  if (line.compare(0, 5, "HTTP/") != 0) return false;
  const std::size_t sp = line.find(' ');
  if (sp == std::string::npos || line.size() < sp + 4) return false;
  h.status = std::atoi(line.substr(sp + 1, 3).c_str());
  if (h.status < 100 || h.status > 599) return false;
  std::size_t pos = eol == std::string::npos ? raw.size() : eol + 2;
  while (pos < raw.size()) {
    std::size_t e = raw.find("\r\n", pos);
    if (e == std::string::npos) e = raw.size();
    const std::string l = raw.substr(pos, e - pos);
    const std::size_t c = l.find(':');
    if (c != std::string::npos && c > 0) {
      std::string k = l.substr(0, c), v = l.substr(c + 1);
      for (char& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      const std::size_t a = v.find_first_not_of(" \t");
      v = a == std::string::npos ? std::string() : v.substr(a);
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
      h.headers[k] = v;
    }
    pos = e + 2;
  }
  *out = h;
  return true;
}

bool decode_chunked(const std::string& in, std::string* out, std::size_t cap, bool* complete) {
  std::string body;
  std::size_t pos = 0;
  if (complete) *complete = false;
  while (pos < in.size()) {
    const std::size_t e = in.find("\r\n", pos);
    if (e == std::string::npos) break;
    std::string sz = in.substr(pos, e - pos);
    const std::size_t semi = sz.find(';');
    if (semi != std::string::npos) sz.erase(semi);
    if (sz.empty() || sz.size() > 8) return false;
    for (char c : sz)
      if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    const std::size_t n = std::strtoul(sz.c_str(), nullptr, 16);
    pos = e + 2;
    if (n == 0) {
      if (complete) *complete = true;
      *out = std::move(body);
      return true;
    }
    if (n > cap || body.size() + n > cap) return false;
    if (pos + n + 2 > in.size()) break;  // truncated
    body.append(in, pos, n);
    if (in.compare(pos + n, 2, "\r\n") != 0) return false;
    pos += n + 2;
  }
  *out = std::move(body);
  return true;
}

std::string resolve_redirect(const Url& base, const std::string& location) {
  if (location.empty()) return {};
  std::string abs;
  if (location[0] == '/' && (location.size() < 2 || location[1] != '/')) {
    abs = "https://" + base.host + (base.port == 443 ? "" : ":" + std::to_string(base.port)) + location;
  } else {
    abs = location;
  }
  Url u;
  if (!parse_https_url(abs, &u) || u.host != base.host) return {};
  return abs;
}

}  // namespace deckhand::metadata::http
