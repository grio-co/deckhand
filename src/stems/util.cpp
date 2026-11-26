#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "deckhand/stems/cache.hpp"
#include "internal.hpp"

namespace deckhand::stems {
namespace detail {

// ---- SHA-256 -------------------------------------------------------------------------------------------------
namespace {
const std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

Sha256::Sha256() {
  static const std::uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(h_, init, sizeof h_);
}
void Sha256::block(const std::uint8_t* p) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) | (std::uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    std::uint32_t ch = (e & f) ^ (~e & g);
    std::uint32_t t1 = h + S1 + ch + K[i] + w[i];
    std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    std::uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}
void Sha256::update(const void* data, std::size_t n) {
  auto p = static_cast<const std::uint8_t*>(data);
  total_ += n;
  while (n) {
    if (fill_ == 0 && n >= 64) { block(p); p += 64; n -= 64; continue; }
    std::size_t k = std::min(n, 64 - fill_);
    std::memcpy(buf_ + fill_, p, k);
    fill_ += k; p += k; n -= k;
    if (fill_ == 64) { block(buf_); fill_ = 0; }
  }
}
std::string Sha256::finish_hex() {
  std::uint64_t bits = total_ * 8;
  std::uint8_t pad[72] = {0x80};
  std::size_t padlen = (fill_ < 56) ? 56 - fill_ : 120 - fill_;
  for (int i = 0; i < 8; ++i) pad[padlen + i] = std::uint8_t(bits >> (56 - 8 * i));
  update(pad, padlen + 8);
  static const char* hx = "0123456789abcdef";
  std::string s;
  for (int i = 0; i < 8; ++i)
    for (int sh = 28; sh >= 0; sh -= 4) s += hx[(h_[i] >> sh) & 15];
  return s;
}

std::uint32_t crc32_update(std::uint32_t crc, const void* data, std::size_t n) {
  static std::uint32_t table[256];
  static bool init = [] {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    return true;
  }();
  (void)init;
  crc = ~crc;
  auto p = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// ---- JSON ------------------------------------------------------------------------------------------------------
namespace {
struct Parser {
  const std::string& s;
  std::size_t i = 0;
  std::string err;
  explicit Parser(const std::string& t) : s(t) {}
  void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
  bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i); return false; }
  bool str(std::string& out) {
    if (i >= s.size() || s[i] != '"') return fail("expected string");
    ++i;
    out.clear();
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c == '\\') {
        if (i >= s.size()) return fail("bad escape");
        char e = s[i++];
        switch (e) {
          case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
          case 'b': out += '\b'; break; case 'f': out += '\f'; break;
          case '"': case '\\': case '/': out += e; break;
          case 'u': {
            if (i + 4 > s.size()) return fail("bad \\u escape");
            unsigned cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = s[i++];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= h - '0'; else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
              else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10; else return fail("bad \\u escape");
            }
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
            else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
            break;
          }
          default: return fail("bad escape");
        }
      } else {
        out += c;
      }
    }
    if (i >= s.size()) return fail("unterminated string");
    ++i;
    return true;
  }
  bool value(Json& v, int depth) {
    if (depth > 32) return fail("nesting too deep");
    ws();
    if (i >= s.size()) return fail("unexpected end");
    char c = s[i];
    if (c == '{') {
      ++i; v.t = Json::T::Obj; ws();
      if (i < s.size() && s[i] == '}') { ++i; return true; }
      while (true) {
        ws();
        std::string k;
        if (!str(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return fail("expected ':'");
        ++i;
        Json x;
        if (!value(x, depth + 1)) return false;
        v.o.emplace_back(std::move(k), std::move(x));
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++i; v.t = Json::T::Arr; ws();
      if (i < s.size() && s[i] == ']') { ++i; return true; }
      while (true) {
        Json x;
        if (!value(x, depth + 1)) return false;
        v.a.push_back(std::move(x));
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') { v.t = Json::T::Str; return str(v.s); }
    if (s.compare(i, 4, "true") == 0) { i += 4; v.t = Json::T::Bool; v.b = true; return true; }
    if (s.compare(i, 5, "false") == 0) { i += 5; v.t = Json::T::Bool; return true; }
    if (s.compare(i, 4, "null") == 0) { i += 4; return true; }
    char* end = nullptr;
    v.n = std::strtod(s.c_str() + i, &end);
    if (end == s.c_str() + i) return fail("unexpected character");
    i = std::size_t(end - s.c_str());
    v.t = Json::T::Num;
    return true;
  }
};
}  // namespace

bool parse_json(const std::string& text, Json& out, std::string* error) {
  if (text.size() > (8u << 20)) { if (error) *error = "JSON too large"; return false; }
  Parser p(text);
  out = Json();
  bool ok = p.value(out, 0);
  if (ok) { p.ws(); if (p.i != text.size()) { p.fail("trailing data"); ok = false; } }
  if (!ok && error) *error = p.err;
  return ok;
}

LayoutIndex make_layout(const std::string& layout, const std::vector<std::int64_t>& dims, const std::string& allowed) {
  LayoutIndex L;
  if (layout.size() != dims.size()) { L.error = "layout '" + layout + "' does not match tensor rank " + std::to_string(dims.size()); return L; }
  std::size_t acc = 1;
  for (int k = int(layout.size()) - 1; k >= 0; --k) {
    unsigned char ch = static_cast<unsigned char>(layout[std::size_t(k)]);
    if (allowed.find(char(ch)) == std::string::npos || L.stride[ch] != 0) { L.error = "bad or repeated letter in layout '" + layout + "'"; return L; }
    if (dims[std::size_t(k)] <= 0) { L.error = "non-positive tensor dimension"; return L; }
    L.stride[ch] = acc;
    acc *= std::size_t(dims[std::size_t(k)]);
  }
  L.total = acc;
  L.ok = true;
  return L;
}

}  // namespace detail

std::string sha256_hex(const void* data, std::size_t n) {
  detail::Sha256 h;
  h.update(data, n);
  return h.finish_hex();
}
bool sha256_file(const std::filesystem::path& p, std::string& hex) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  detail::Sha256 h;
  std::vector<char> buf(1 << 20);
  while (f) {
    f.read(buf.data(), std::streamsize(buf.size()));
    h.update(buf.data(), std::size_t(f.gcount()));
  }
  if (f.bad()) return false;
  hex = h.finish_hex();
  return true;
}

}  // namespace deckhand::stems
