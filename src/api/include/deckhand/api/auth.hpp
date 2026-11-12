#pragma once
// Authentication building blocks: scopes, tokens, rate limiting, lockout.
//
// Scope grammar: colon-separated segments of [a-z0-9_.*-], e.g. "read", "control",
// "control:deck:1", "control:deck:*".
//   * A granted scope covers a required scope when its segments are a prefix of the required
//     segments and each granted segment equals the required one or is "*".
//     So "control" covers "control" and "control:deck:1"; "control:deck:1" covers only itself
//     and "control:deck:1:..."; "control:deck:*" covers "control:deck:<anything>".
//   * "admin" (exactly) covers every scope. Scopes never imply each other otherwise
//     ("control" does not include "read").
//   * "*" as the whole granted scope is NOT special (not valid); use "admin".
//
// Threading: TokenStore, RateLimiter and FailureTracker are internally synchronized.
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace deckhand::api {

std::string sha256_hex(std::string_view data);
bool random_bytes(void* out, std::size_t n);   // cryptographically secure
std::string random_hex(std::size_t nbytes);    // empty on failure

bool valid_scope(std::string_view scope);
bool scope_allows(std::string_view granted, std::string_view required);
bool scopes_allow(const std::vector<std::string>& granted, std::string_view required);

// "10.0.0.0/8", "::1/128", or a bare address (host route). Peer addresses in the
// IPv4-mapped IPv6 form (::ffff:a.b.c.d) are treated as IPv4.
bool valid_cidr(std::string_view cidr);
bool cidr_contains(std::string_view cidr, std::string_view ip);

struct TokenInfo {
  std::string id;                      // public identifier (not secret), 16 hex chars
  std::string name;
  std::vector<std::string> scopes;
  std::int64_t created = 0;            // unix seconds
  std::int64_t expires = 0;            // unix seconds, 0 = never
  std::vector<std::string> cidrs;      // empty = any source address
};

// Tokens are "dh_" + 64 hex chars (256 random bits). Only the SHA-256 hash is stored, in a
// JSON file written atomically with mode 0600.
class TokenStore {
 public:
  explicit TokenStore(std::string path = {});  // empty path = memory only (tests)
  // Loads the file; a missing file is fine. False (and *error) if it is unreadable or corrupt;
  // the store is then empty and will not overwrite the file until a successful load or create.
  bool load(std::string* error = nullptr);

  struct Created {
    TokenInfo info;
    std::string secret;  // shown once, never stored
  };
  bool create(const std::string& name, const std::vector<std::string>& scopes, std::int64_t expires_unix,
              const std::vector<std::string>& cidrs, Created* out, std::string* error = nullptr);
  std::vector<TokenInfo> list() const;
  bool revoke(const std::string& id);  // false if unknown

  enum class Result { Ok, Unknown, Expired, SourceDenied };
  // `now_unix` 0 = current time. Constant-time with respect to the stored hashes.
  Result authenticate(std::string_view secret, std::string_view remote_ip, TokenInfo* out,
                      std::int64_t now_unix = 0) const;
  // Still present, unexpired? (used to drop live connections when a token is revoked.)
  bool still_valid(const std::string& id, std::int64_t now_unix = 0) const;

 private:
  struct Entry {
    TokenInfo info;
    std::string hash;
  };
  bool save_locked(std::string* error) const;

  std::string path_;
  mutable std::mutex mu_;
  std::vector<Entry> entries_;
  bool load_failed_ = false;
};

// Token bucket per key. `rate` tokens per second, up to `burst`.
class RateLimiter {
 public:
  RateLimiter(double rate, double burst, std::size_t max_keys = 20000) : rate_(rate), burst_(burst), max_keys_(max_keys) {}
  bool allow(const std::string& key, double now_sec);
  void reconfigure(double rate, double burst);

 private:
  struct Bucket {
    double tokens;
    double last;
  };
  double rate_, burst_;
  std::size_t max_keys_;
  std::mutex mu_;
  std::map<std::string, Bucket> buckets_;
};

// Locks a key out for `lockout` seconds after `max_failures` failures within `window` seconds.
class FailureTracker {
 public:
  FailureTracker(int max_failures, double window, double lockout, std::size_t max_keys = 20000)
      : max_(max_failures), window_(window), lockout_(lockout), max_keys_(max_keys) {}
  bool locked(const std::string& key, double now_sec, double* retry_after = nullptr);
  void failure(const std::string& key, double now_sec);
  void success(const std::string& key);

 private:
  struct Rec {
    int count = 0;
    double first = 0;
    double locked_until = 0;
  };
  int max_;
  double window_, lockout_;
  std::size_t max_keys_;
  std::mutex mu_;
  std::map<std::string, Rec> recs_;
};

double monotonic_seconds();

}  // namespace deckhand::api
