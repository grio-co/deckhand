// The pure HTTP pieces of the HTTPS client (URL checks, request, response head, chunked bodies, redirects). No sockets.
#include <cstdio>

#include "check.hpp"
#include "http_parse.hpp"

using namespace deckhand::metadata::http;

int main() {
  Url u;
  CHECK(parse_https_url("https://musicbrainz.org/ws/2/recording?query=a%20b&fmt=json#x", &u));
  CHECK(u.host == "musicbrainz.org" && u.port == 443 && u.target == "/ws/2/recording?query=a%20b&fmt=json");
  CHECK(parse_https_url("https://example.test:8443", &u) && u.port == 8443 && u.target == "/");
  CHECK(!parse_https_url("http://musicbrainz.org/", &u));
  CHECK(!parse_https_url("https://user@host/", &u));
  CHECK(!parse_https_url("https://host:99999/", &u));
  CHECK(!parse_https_url("https:///x", &u));
  CHECK(!parse_https_url("https://host/a b", &u));
  CHECK(!parse_https_url("https://host/a\r\nX: y", &u));
  CHECK(parse_https_url("https://musicbrainz.org/ws/2/x", &u));
  const std::string req = build_request(u, "DeckHand/0.1\r\nInjected: 1");
  CHECK(req.rfind("GET /ws/2/x HTTP/1.1\r\nHost: musicbrainz.org\r\n", 0) == 0);
  CHECK(req.find("\r\nInjected") == std::string::npos);
  CHECK(req.find("Connection: close") != std::string::npos);

  Head h;
  CHECK(parse_head("HTTP/1.1 503 Service Unavailable\r\nRetry-After:  7 \r\nContent-Type: application/json", &h));
  CHECK(h.status == 503 && h.headers["retry-after"] == "7" && h.headers["content-type"] == "application/json");
  CHECK(!parse_head("garbage", &h));
  CHECK(!parse_head("HTTP/1.1 99 x", &h));

  std::string body;
  bool complete = false;
  CHECK(decode_chunked("4\r\nWiki\r\n5;ext=1\r\npedia\r\n0\r\n\r\n", &body, 100, &complete) && complete && body == "Wikipedia");
  CHECK(decode_chunked("4\r\nWiki\r\n", &body, 100, &complete) && !complete);       // truncated
  CHECK(!decode_chunked("zz\r\nabc\r\n", &body, 100, &complete));                   // not hex
  CHECK(!decode_chunked("10\r\n0123456789abcdef\r\n0\r\n\r\n", &body, 8, &complete));  // over the cap
  CHECK(!decode_chunked("3\r\nabcXX0\r\n\r\n", &body, 100, &complete));             // missing CRLF after data

  Url base;
  CHECK(parse_https_url("https://musicbrainz.org/ws/2/a", &base));
  CHECK(resolve_redirect(base, "/ws/2/b") == "https://musicbrainz.org/ws/2/b");
  CHECK(resolve_redirect(base, "https://musicbrainz.org/x") == "https://musicbrainz.org/x");
  CHECK(resolve_redirect(base, "https://evil.example/x").empty());
  CHECK(resolve_redirect(base, "http://musicbrainz.org/x").empty());
  CHECK(resolve_redirect(base, "//evil.example/x").empty());
  CHECK(resolve_redirect(base, "").empty());
  std::printf("metadata http tests passed\n");
  return 0;
}
