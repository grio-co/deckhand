// api: authentication primitives (scopes, token store, rate limiting). The HTTP/WebSocket
// transport lives in server.cpp.
#include <arpa/inet.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "deckhand/api/auth.hpp"
#include "deckhand/api/json.hpp"

namespace deckhand::api {

double monotonic_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

namespace {
std::string hex(const unsigned char* d, std::size_t n) {
  static const char* k = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    s += k[d[i] >> 4];
    s += k[d[i] & 15];
  }
  return s;
}

std::int64_t unix_now() { return static_cast<std::int64_t>(std::time(nullptr)); }

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (;;) {
    auto p = s.find(sep, start);
    if (p == std::string_view::npos) { out.push_back(s.substr(start)); return out; }
    out.push_back(s.substr(start, p - start));
    start = p + 1;
  }
}

// Parses an address into 16 bytes (v4 as v4-mapped) and reports whether it was IPv4.
bool parse_ip(std::string_view ip, unsigned char out[16], bool* is_v4) {
  std::string s(ip);
  in_addr a4;
  in6_addr a6;
  if (inet_pton(AF_INET, s.c_str(), &a4) == 1) {
    std::memset(out, 0, 10);
    out[10] = out[11] = 0xff;
    std::memcpy(out + 12, &a4, 4);
    *is_v4 = true;
    return true;
  }
  if (inet_pton(AF_INET6, s.c_str(), &a6) == 1) {
    std::memcpy(out, &a6, 16);
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    *is_v4 = std::memcmp(out, mapped, 12) == 0;
    return true;
  }
  return false;
}

bool parse_cidr(std::string_view c, unsigned char addr[16], int* bits) {
  auto slash = c.find('/');
  std::string_view host = c.substr(0, slash);
  bool v4 = false;
  if (!parse_ip(host, addr, &v4)) return false;
  // Was it written as IPv4? (parse_ip also reports mapped v6 as v4; distinguish by the text.)
  bool written_v4 = host.find(':') == std::string_view::npos;
  int max = written_v4 ? 32 : 128;
  if (slash == std::string_view::npos) { *bits = written_v4 ? 128 : max; return true; }
  std::string_view n = c.substr(slash + 1);
  if (n.empty() || n.size() > 3) return false;
  int v = 0;
  for (char ch : n) {
    if (ch < '0' || ch > '9') return false;
    v = v * 10 + (ch - '0');
  }
  if (v > max) return false;
  *bits = written_v4 ? v + 96 : v;  // v4 prefixes live in the mapped /96
  return true;
}
}  // namespace

std::string sha256_hex(std::string_view data) {
  unsigned char md[32];
  unsigned int n = 0;
  if (EVP_Digest(data.data(), data.size(), md, &n, EVP_sha256(), nullptr) != 1) return {};
  return hex(md, n);
}

bool random_bytes(void* out, std::size_t n) { return RAND_bytes(static_cast<unsigned char*>(out), static_cast<int>(n)) == 1; }

std::string random_hex(std::size_t nbytes) {
  std::vector<unsigned char> b(nbytes);
  if (!random_bytes(b.data(), nbytes)) return {};
  return hex(b.data(), nbytes);
}

bool valid_scope(std::string_view s) {
  if (s.empty() || s.size() > 96) return false;
  for (auto seg : split(s, ':')) {
    if (seg.empty()) return false;
    if (seg == "*") continue;
    for (char c : seg)
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
  }
  return s != "*";
}

bool scope_allows(std::string_view granted, std::string_view required) {
  if (granted == "admin") return true;
  if (!valid_scope(granted) || required.empty()) return false;
  auto g = split(granted, ':');
  auto r = split(required, ':');
  if (g.size() > r.size()) return false;
  for (std::size_t i = 0; i < g.size(); ++i)
    if (g[i] != "*" && g[i] != r[i]) return false;
  return true;
}

bool scopes_allow(const std::vector<std::string>& granted, std::string_view required) {
  for (const auto& g : granted)
    if (scope_allows(g, required)) return true;
  return false;
}

bool valid_cidr(std::string_view cidr) {
  unsigned char a[16];
  int bits;
  return parse_cidr(cidr, a, &bits);
}

bool cidr_contains(std::string_view cidr, std::string_view ip) {
  unsigned char net[16], addr[16];
  int bits;
  bool v4 = false;
  if (!parse_cidr(cidr, net, &bits) || !parse_ip(ip, addr, &v4)) return false;
  bool net_v4 = cidr.substr(0, cidr.find('/')).find(':') == std::string_view::npos;
  if (net_v4 && !v4) return false;
  if (!net_v4 && v4 && bits <= 96) {
    // A v6 rule written as a mapped address is handled above; plain v6 rules never match v4 peers.
    return false;
  }
  int full = bits / 8;
  if (std::memcmp(net, addr, static_cast<std::size_t>(full)) != 0) return false;
  int rem = bits % 8;
  if (rem == 0) return true;
  unsigned char mask = static_cast<unsigned char>(0xFF << (8 - rem));
  return (net[full] & mask) == (addr[full] & mask);
}

// ---- TokenStore ---------------------------------------------------------------------------

TokenStore::TokenStore(std::string path) : path_(std::move(path)) {}

bool TokenStore::load(std::string* error) {
  std::lock_guard<std::mutex> lk(mu_);
  entries_.clear();
  load_failed_ = false;
  if (path_.empty()) return true;
  std::ifstream f(path_, std::ios::binary);
  if (!f) {
    if (access(path_.c_str(), F_OK) != 0) return true;  // first run
    load_failed_ = true;
    if (error) *error = "cannot read " + path_;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  std::string perr;
  JsonLimits lim;
  lim.max_bytes = 16u << 20;
  Json j;
  if (!Json::try_parse(ss.str(), &j, &perr, lim) || !j.is_object() || !j["tokens"].is_array()) {
    load_failed_ = true;
    if (error) *error = "corrupt token file: " + (perr.empty() ? std::string("bad structure") : perr);
    return false;
  }
  const Json& arr = j["tokens"];
  for (std::size_t i = 0; i < arr.size(); ++i) {
    const Json& t = arr.at(i);
    Entry e;
    e.info.id = t["id"].as_string();
    e.info.name = t["name"].as_string();
    e.hash = t["hash"].as_string();
    e.info.created = t["created"].as_int();
    e.info.expires = t["expires"].as_int();
    for (std::size_t k = 0; k < t["scopes"].size(); ++k) e.info.scopes.push_back(t["scopes"].at(k).as_string());
    for (std::size_t k = 0; k < t["cidrs"].size(); ++k) e.info.cidrs.push_back(t["cidrs"].at(k).as_string());
    bool ok = !e.info.id.empty() && e.hash.size() == 64;
    for (const auto& s : e.info.scopes) ok = ok && valid_scope(s);
    for (const auto& c : e.info.cidrs) ok = ok && valid_cidr(c);
    if (!ok) {
      entries_.clear();
      load_failed_ = true;
      if (error) *error = "corrupt token file: invalid entry " + std::to_string(i);
      return false;
    }
    entries_.push_back(std::move(e));
  }
  return true;
}

bool TokenStore::save_locked(std::string* error) const {
  if (path_.empty()) return true;
  if (load_failed_) {
    if (error) *error = "token file failed to load; refusing to overwrite it";
    return false;
  }
  Json root = Json::object();
  root.set("version", 1);
  Json arr = Json::array();
  for (const auto& e : entries_) {
    Json t = Json::object();
    t.set("id", e.info.id);
    t.set("name", e.info.name);
    t.set("hash", e.hash);
    Json sc = Json::array();
    for (const auto& s : e.info.scopes) sc.push_back(s);
    t.set("scopes", std::move(sc));
    t.set("created", e.info.created);
    t.set("expires", e.info.expires);
    Json cd = Json::array();
    for (const auto& c : e.info.cidrs) cd.push_back(c);
    t.set("cidrs", std::move(cd));
    arr.push_back(std::move(t));
  }
  root.set("tokens", std::move(arr));
  const std::string data = root.dump();
  const std::string tmp = path_ + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    if (error) *error = "cannot write " + tmp;
    return false;
  }
  fchmod(fd, 0600);
  bool ok = true;
  const char* p = data.data();
  std::size_t left = data.size();
  while (left > 0) {
    ssize_t n = ::write(fd, p, left);
    if (n <= 0) { ok = false; break; }
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  ok = ok && fsync(fd) == 0;
  ::close(fd);
  if (!ok || std::rename(tmp.c_str(), path_.c_str()) != 0) {
    ::unlink(tmp.c_str());
    if (error) *error = "cannot save " + path_;
    return false;
  }
  return true;
}

bool TokenStore::create(const std::string& name, const std::vector<std::string>& scopes, std::int64_t expires_unix,
                        const std::vector<std::string>& cidrs, Created* out, std::string* error) {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return false;
  };
  if (name.empty() || name.size() > 64 || !valid_utf8(name)) return fail("invalid token name");
  if (scopes.empty()) return fail("at least one scope is required");
  for (const auto& s : scopes)
    if (!valid_scope(s)) return fail("invalid scope");
  for (const auto& c : cidrs)
    if (!valid_cidr(c)) return fail("invalid CIDR");
  std::string raw = random_hex(32), id = random_hex(8);
  if (raw.empty() || id.empty()) return fail("random source failed");
  Entry e;
  e.info.id = id;
  e.info.name = name;
  e.info.scopes = scopes;
  e.info.created = unix_now();
  e.info.expires = expires_unix;
  e.info.cidrs = cidrs;
  const std::string secret = "dh_" + raw;
  e.hash = sha256_hex(secret);
  std::lock_guard<std::mutex> lk(mu_);
  entries_.push_back(e);
  if (!save_locked(error)) {
    entries_.pop_back();
    return false;
  }
  if (out) {
    out->info = e.info;
    out->secret = secret;
  }
  return true;
}

std::vector<TokenInfo> TokenStore::list() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<TokenInfo> v;
  for (const auto& e : entries_) v.push_back(e.info);
  return v;
}

bool TokenStore::revoke(const std::string& id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.info.id == id; });
  if (it == entries_.end()) return false;
  entries_.erase(it);
  save_locked(nullptr);  // stays revoked in memory even if the disk write fails (fail closed)
  return true;
}

TokenStore::Result TokenStore::authenticate(std::string_view secret, std::string_view remote_ip, TokenInfo* out,
                                            std::int64_t now_unix) const {
  if (secret.empty() || secret.size() > 128) return Result::Unknown;
  const std::string h = sha256_hex(secret);
  if (h.size() != 64) return Result::Unknown;
  if (now_unix == 0) now_unix = unix_now();
  std::lock_guard<std::mutex> lk(mu_);
  const Entry* found = nullptr;
  for (const auto& e : entries_) {
    // No early exit: every stored hash is compared.
    if (e.hash.size() == 64 && CRYPTO_memcmp(e.hash.data(), h.data(), 64) == 0) found = &e;
  }
  if (!found) return Result::Unknown;
  if (found->info.expires != 0 && now_unix >= found->info.expires) return Result::Expired;
  if (!found->info.cidrs.empty()) {
    bool ok = false;
    for (const auto& c : found->info.cidrs) ok = cidr_contains(c, remote_ip) || ok;
    if (!ok) return Result::SourceDenied;
  }
  if (out) *out = found->info;
  return Result::Ok;
}

bool TokenStore::still_valid(const std::string& id, std::int64_t now_unix) const {
  if (now_unix == 0) now_unix = unix_now();
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& e : entries_)
    if (e.info.id == id) return e.info.expires == 0 || now_unix < e.info.expires;
  return false;
}

// ---- RateLimiter / FailureTracker -----------------------------------------------------------

void RateLimiter::reconfigure(double rate, double burst) {
  std::lock_guard<std::mutex> lk(mu_);
  rate_ = rate;
  burst_ = burst;
}

bool RateLimiter::allow(const std::string& key, double now) {
  std::lock_guard<std::mutex> lk(mu_);
  if (rate_ <= 0) return true;  // disabled
  if (buckets_.size() >= max_keys_ && !buckets_.count(key)) {
    // Drop idle (full) buckets; if everything is busy, drop the oldest half of them.
    for (auto it = buckets_.begin(); it != buckets_.end();)
      it = (it->second.tokens + (now - it->second.last) * rate_ >= burst_) ? buckets_.erase(it) : std::next(it);
    if (buckets_.size() >= max_keys_) buckets_.erase(buckets_.begin());
  }
  auto it = buckets_.find(key);
  if (it == buckets_.end()) it = buckets_.emplace(key, Bucket{burst_, now}).first;
  Bucket& b = it->second;
  b.tokens = std::min(burst_, b.tokens + std::max(0.0, now - b.last) * rate_);
  b.last = now;
  if (b.tokens < 1.0) return false;
  b.tokens -= 1.0;
  return true;
}

bool FailureTracker::locked(const std::string& key, double now, double* retry_after) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = recs_.find(key);
  if (it == recs_.end() || it->second.locked_until <= now) return false;
  if (retry_after) *retry_after = it->second.locked_until - now;
  return true;
}

void FailureTracker::failure(const std::string& key, double now) {
  std::lock_guard<std::mutex> lk(mu_);
  if (max_ <= 0) return;
  if (recs_.size() >= max_keys_ && !recs_.count(key)) {
    for (auto it = recs_.begin(); it != recs_.end();)
      it = (it->second.locked_until <= now && now - it->second.first > window_) ? recs_.erase(it) : std::next(it);
    if (recs_.size() >= max_keys_) recs_.erase(recs_.begin());
  }
  Rec& r = recs_[key];
  if (r.count == 0 || now - r.first > window_) {
    r.count = 0;
    r.first = now;
  }
  if (++r.count >= max_) {
    r.locked_until = now + lockout_;
    r.count = 0;
  }
}

void FailureTracker::success(const std::string& key) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = recs_.find(key);
  if (it != recs_.end() && it->second.locked_until == 0) recs_.erase(it);
}

}  // namespace deckhand::api
