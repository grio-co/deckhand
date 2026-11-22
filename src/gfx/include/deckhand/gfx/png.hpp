#pragma once
// A small PNG reader for skin images: non-interlaced, bit depth 8, colour types RGBA, RGB, grey, grey+alpha and
// palette (with tRNS). No third-party code; the inflate decoder is in png.cpp. Never throws, never reads out of
// bounds, and refuses images that would need more than kMaxPixels.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace deckhand::gfx {

struct Image {
  int width = 0, height = 0;
  std::vector<std::uint8_t> rgba;  // width * height * 4, straight (non-premultiplied) alpha
};

constexpr std::size_t kMaxPixels = 16384u * 16384u / 4;  // 64 Mpixel

// Decodes `size` bytes of a PNG file. On failure returns false and sets `error`.
bool decode_png(const std::uint8_t* data, std::size_t size, Image& out, std::string& error);
// Same, reading a file (at most `max_bytes` long).
bool load_png_file(const std::string& path, Image& out, std::string& error, std::size_t max_bytes = 8u << 20);

// Raw zlib/deflate stream (RFC 1950/1951) decoder, exposed for tests. `limit` caps the output size.
bool zlib_inflate(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out, std::size_t limit,
                  std::string& error);

}  // namespace deckhand::gfx
