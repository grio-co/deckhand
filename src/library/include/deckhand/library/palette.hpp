#pragma once
// DeckHand's own colour palette for track colour tags and cue colours: 16 colours, 0xRRGGBB. The first eight follow the
// hue order pink, red, orange, yellow, green, aqua, blue, purple that the USB export's colour table uses (id = index + 1).
#include <cstdint>

namespace deckhand {

constexpr int kPaletteSize = 16;
constexpr std::uint32_t kPalette[kPaletteSize] = {
    0xFF5FA8, 0xFF5C5C, 0xFF8A3D, 0xFADC46, 0x3DDC84, 0x3CD2C8, 0x3EA6FF, 0xA678FF,
    0xA0E63C, 0x1EAAA0, 0x78C8FF, 0x5A64E6, 0xDC50DC, 0xC88C3C, 0x96A0AF, 0xEBEEF2};
constexpr const char* const kPaletteIds[kPaletteSize] = {"pink",   "red",    "orange", "yellow", "green",  "aqua",
                                                         "blue",   "purple", "lime",   "teal",   "sky",    "indigo",
                                                         "magenta", "amber",  "grey",   "white"};

// Index of the palette colour nearest to rgb, or -1 for "no colour" (rgb < 0).
inline int palette_nearest(std::int64_t rgb, int first = 0, int count = kPaletteSize) {
  if (rgb < 0) return -1;
  const int r = static_cast<int>((rgb >> 16) & 255), g = static_cast<int>((rgb >> 8) & 255), b = static_cast<int>(rgb & 255);
  int best = first, best_d = 1 << 30;
  for (int i = first; i < first + count && i < kPaletteSize; ++i) {
    const int dr = r - static_cast<int>((kPalette[i] >> 16) & 255), dg = g - static_cast<int>((kPalette[i] >> 8) & 255),
              db = b - static_cast<int>(kPalette[i] & 255);
    const int d = dr * dr + dg * dg + db * db;
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

}  // namespace deckhand
