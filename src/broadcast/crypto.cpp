#include "deckhand/broadcast/crypto.hpp"

#include <cstdint>
#include <cstring>

namespace deckhand::broadcast {
namespace {

constexpr std::uint32_t kK256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline std::uint32_t rotl(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// Pads per FIPS 180: 0x80, zeros, 64-bit big-endian bit length.
std::string pad_message(std::string_view data) {
  std::string msg(data);
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back('\0');
  const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
  return msg;
}

inline std::uint32_t load_be(const unsigned char* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

void store_be(std::string& out, std::uint32_t v) {
  for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
}

}  // namespace

std::string sha256(std::string_view data) {
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const std::string msg = pad_message(data);
  for (std::size_t off = 0; off < msg.size(); off += 64) {
    const auto* p = reinterpret_cast<const unsigned char*>(msg.data()) + off;
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = load_be(p + i * 4);
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t t1 = hh + S1 + ch + kK256[i] + w[i];
      const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = S0 + maj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  std::string out;
  for (std::uint32_t v : h) store_be(out, v);
  return out;
}

std::string sha1(std::string_view data) {
  std::uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  const std::string msg = pad_message(data);
  for (std::size_t off = 0; off < msg.size(); off += 64) {
    const auto* p = reinterpret_cast<const unsigned char*>(msg.data()) + off;
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = load_be(p + i * 4);
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
      else { f = b ^ c ^ d; k = 0xca62c1d6; }
      const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::string out;
  for (std::uint32_t v : h) store_be(out, v);
  return out;
}

std::string to_hex(std::string_view bytes) {
  static const char* d = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) {
    out.push_back(d[c >> 4]);
    out.push_back(d[c & 15]);
  }
  return out;
}

std::string base64_encode(std::string_view bytes) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const std::uint32_t v = (std::uint32_t(static_cast<unsigned char>(bytes[i])) << 16) |
                            (std::uint32_t(static_cast<unsigned char>(bytes[i + 1])) << 8) |
                            static_cast<unsigned char>(bytes[i + 2]);
    out.push_back(t[(v >> 18) & 63]);
    out.push_back(t[(v >> 12) & 63]);
    out.push_back(t[(v >> 6) & 63]);
    out.push_back(t[v & 63]);
  }
  const std::size_t rest = bytes.size() - i;
  if (rest == 1) {
    const std::uint32_t v = std::uint32_t(static_cast<unsigned char>(bytes[i])) << 16;
    out.push_back(t[(v >> 18) & 63]);
    out.push_back(t[(v >> 12) & 63]);
    out += "==";
  } else if (rest == 2) {
    const std::uint32_t v = (std::uint32_t(static_cast<unsigned char>(bytes[i])) << 16) |
                            (std::uint32_t(static_cast<unsigned char>(bytes[i + 1])) << 8);
    out.push_back(t[(v >> 18) & 63]);
    out.push_back(t[(v >> 12) & 63]);
    out.push_back(t[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

bool base64_decode(std::string_view text, std::string* out) {
  std::string res;
  std::uint32_t acc = 0;
  int bits = 0, pad = 0;
  for (char c : text) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else if (c == '=') { ++pad; continue; }
    else return false;
    if (pad) return false;  // data after padding
    acc = (acc << 6) | std::uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      res.push_back(static_cast<char>((acc >> bits) & 0xff));
    }
  }
  if (pad > 2) return false;
  if (out) *out = std::move(res);
  return true;
}

std::string obs_auth_string(const std::string& password, const std::string& salt, const std::string& challenge) {
  const std::string secret = base64_encode(sha256(password + salt));
  return base64_encode(sha256(secret + challenge));
}

std::string websocket_accept_key(const std::string& client_key) {
  return base64_encode(sha1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

}  // namespace deckhand::broadcast
