#pragma once
// Primitive byte IO and UTF conversion shared by the writer and the reader (nothing format specific).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace deckhand::usbexport::detail {

using Bytes = std::vector<std::uint8_t>;

inline void put_u8(Bytes& b, std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v)); }
inline void put_le16(Bytes& b, std::uint32_t v) { put_u8(b, v); put_u8(b, v >> 8); }
inline void put_le32(Bytes& b, std::uint32_t v) { put_le16(b, v); put_le16(b, v >> 16); }
inline void put_be16(Bytes& b, std::uint32_t v) { put_u8(b, v >> 8); put_u8(b, v); }
inline void put_be32(Bytes& b, std::uint32_t v) { put_be16(b, v >> 16); put_be16(b, v); }
inline void set_le16(Bytes& b, std::size_t at, std::uint32_t v) {
  b[at] = static_cast<std::uint8_t>(v);
  b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
inline void set_le32(Bytes& b, std::size_t at, std::uint32_t v) {
  set_le16(b, at, v);
  set_le16(b, at + 2, v >> 16);
}
inline void set_be32(Bytes& b, std::size_t at, std::uint32_t v) {
  b[at] = static_cast<std::uint8_t>(v >> 24);
  b[at + 1] = static_cast<std::uint8_t>(v >> 16);
  b[at + 2] = static_cast<std::uint8_t>(v >> 8);
  b[at + 3] = static_cast<std::uint8_t>(v);
}

inline std::u16string utf8_to_utf16(const std::string& s) {
  std::u16string out;
  std::size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::uint32_t cp = 0xFFFD;
    std::size_t n = 1;
    if (c < 0x80) {
      cp = c;
    } else if (c >= 0xC2 && c < 0xE0) {
      n = 2;
    } else if (c >= 0xE0 && c < 0xF0) {
      n = 3;
    } else if (c >= 0xF0 && c < 0xF5) {
      n = 4;
    }
    if (n > 1) {
      if (i + n <= s.size()) {
        cp = n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
        for (std::size_t k = 1; k < n; ++k) {
          const unsigned char cc = static_cast<unsigned char>(s[i + k]);
          if ((cc & 0xC0) != 0x80) {
            cp = 0xFFFD;
            n = k;
            break;
          }
          cp = (cp << 6) | (cc & 0x3Fu);
        }
      } else {
        n = s.size() - i;
      }
    }
    i += n;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return out;
}

inline std::string utf16_to_utf8(const std::u16string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    std::uint32_t cp = s[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp < 0xE000) {
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

// Truncate to at most max bytes without splitting a UTF-8 sequence.
inline std::string cap_utf8(const std::string& s, std::size_t max) {
  if (s.size() <= max) return s;
  std::size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return s.substr(0, n);
}

}  // namespace deckhand::usbexport::detail
