// The DirectWrite rasteriser: metrics conventions and that every script produces ink (font fallback works).
// With an argument, writes a sheet of samples to that PNG for a visual check.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>

#include "../../../tests/check.hpp"
#include "../shell/png.hpp"
#include "../shell/text.hpp"

using deckhand::shell::rasterise_text;

int main(int argc, char** argv) {
  const std::vector<std::string> samples = {
      "DeckHand  Collection 128.00 BPM", "Tr\xC3\xA4""ume \xC3\xA9\xC3\xA8 \xC3\xB1 \xC3\x9F",
      "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x88\xE3\x83\xA9\xE3\x83\x83\xE3\x82\xAF",  // Japanese
      "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4",                                                              // Korean
      "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85",  // Arabic
      "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D",                                  // Hebrew
      "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87",                           // Devanagari
      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xCE\x93\xCE\xB5\xCE\xB9\xCE\xAC"};                 // Cyrillic, Greek

  dh_bitmap empty{};
  CHECK(!rasterise_text(nullptr, "", 14, 0, &empty));
  CHECK(!rasterise_text(nullptr, "x", 0, 0, &empty));

  // Metrics: height = ceil(ascent) + ceil(descent) + 2, bigger text is bigger, bold is wider.
  dh_bitmap small{}, big{}, bold{};
  CHECK(rasterise_text(nullptr, "Hamburgefonts", 12, 0, &small));
  CHECK(rasterise_text(nullptr, "Hamburgefonts", 24, 0, &big));
  CHECK(rasterise_text(nullptr, "Hamburgefonts", 12, 1, &bold));
  CHECK(big.w > small.w * 3 / 2 && big.h > small.h && small.ascent > 5 && small.ascent < small.h);
  CHECK(bold.w >= small.w);
  std::free(small.alpha); std::free(big.alpha); std::free(bold.alpha);

  std::vector<dh_bitmap> bitmaps;
  for (const auto& s : samples) {
    dh_bitmap b{};
    CHECK(rasterise_text(nullptr, s.c_str(), 20, 0, &b));
    long ink = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(b.w) * b.h; ++i) ink += b.alpha[i] > 64;
    std::printf("%2ux%-3u ink=%ld  %s\n", b.w, b.h, ink, s.c_str());
    // A blank bitmap means no font produced any glyph. Where the system lacks fonts for a script (Wine in a minimal
    // container) DECKHAND_TEST_LENIENT=1 turns that into a reported skip for the non-Latin samples.
    if (ink <= 20 && std::getenv("DECKHAND_TEST_LENIENT") && &s != &samples.front() && &s != &samples[1]) {
      std::printf("   skip: no font for this script on this system\n");
    } else {
      CHECK(ink > 20);
    }
    bitmaps.push_back(b);
  }

  if (argc > 1) {
    int sheet_w = 0, sheet_h = 8;
    for (const auto& b : bitmaps) { sheet_w = std::max<int>(sheet_w, b.w + 16); sheet_h += b.h + 6; }
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(sheet_w) * sheet_h * 4, 255);
    int y = 4;
    for (const auto& b : bitmaps) {
      for (std::uint32_t j = 0; j < b.h; ++j)
        for (std::uint32_t i = 0; i < b.w; ++i) {
          const std::uint8_t a = b.alpha[j * b.w + i];
          std::uint8_t* p = &rgba[(static_cast<std::size_t>(y + j) * sheet_w + 8 + i) * 4];
          p[0] = p[1] = p[2] = static_cast<std::uint8_t>(255 - a);
        }
      y += static_cast<int>(b.h) + 6;
    }
    int wn = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
    std::wstring wpath(static_cast<std::size_t>(wn > 0 ? wn : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, wpath.data(), wn);
    CHECK(deckhand::shell::write_png(wpath.c_str(), sheet_w, sheet_h, rgba.data()));
  }
  for (auto& b : bitmaps) std::free(b.alpha);
  std::puts("test_windows_text ok");
  return 0;
}
