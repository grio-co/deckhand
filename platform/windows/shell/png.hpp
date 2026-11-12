// Writes an RGBA image as a PNG (stored deflate blocks, no compression). Used by --screenshot.
#pragma once
#include <cstdint>

namespace deckhand::shell {
bool write_png(const wchar_t* path, int width, int height, const std::uint8_t* rgba);
}  // namespace deckhand::shell
