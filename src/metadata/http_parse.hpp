#pragma once
// Pure HTTP/1.1 pieces of the HTTPS client, testable without a socket.
#include <cstddef>
#include <map>
#include <string>

namespace deckhand::metadata::http {

struct Url {
  std::string host, target;  // target = path and query, starts with '/'
  int port = 443;
};
// https URLs only; no user info; host letters, digits, '.' and '-'.
bool parse_https_url(const std::string& url, Url* out);
std::string build_request(const Url& u, const std::string& user_agent);

struct Head {
  int status = 0;
  std::map<std::string, std::string> headers;  // lower-case names
};
// `raw` is everything before the blank line.
bool parse_head(const std::string& raw, Head* out);
// Decodes a complete chunked body. False when malformed or longer than `cap`; *complete tells whether the last chunk was seen.
bool decode_chunked(const std::string& in, std::string* out, std::size_t cap, bool* complete);
// Absolute https URL on the same host that `location` points to, or "" (other host, other scheme, malformed).
std::string resolve_redirect(const Url& base, const std::string& location);

}  // namespace deckhand::metadata::http
