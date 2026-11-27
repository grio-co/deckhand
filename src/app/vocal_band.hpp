#pragma once
// Where the vocal band of a track is drawn: the stored vocal regions mapped onto a strip, clipped to it, with regions that
// would be thinner than a pixel and sit next to each other merged. The regions belong to the loaded track (DeckInfo), so
// this is the per-track cache; the mapping is a few multiplications per region.
#include <algorithm>
#include <vector>

#include "deckhand/library/library.hpp"

namespace deckhand {

struct VocalSpan {
  float x0 = 0, x1 = 0;
};

// Spans of the regions on a strip from `x` of width `w` that shows [view_start_ms, view_end_ms]. Regions outside the view
// are skipped; a gap under `min_gap` pixels between two spans is closed so a track with hundreds of tiny regions does not
// become hundreds of rectangles.
inline std::vector<VocalSpan> vocal_spans(const std::vector<StoredVocalRegion>& regions, double view_start_ms, double view_end_ms, float x, float w,
                                          float min_gap = 1.0f) {
  std::vector<VocalSpan> out;
  if (!(view_end_ms > view_start_ms) || w <= 0) return out;
  const double per_ms = static_cast<double>(w) / (view_end_ms - view_start_ms);
  const float xl = x, xr = x + w;
  for (const StoredVocalRegion& g : regions) {
    if (g.end_ms <= view_start_ms || g.start_ms >= view_end_ms) continue;
    const float a = std::max(xl, x + static_cast<float>((g.start_ms - view_start_ms) * per_ms));
    const float b = std::min(xr, x + static_cast<float>((g.end_ms - view_start_ms) * per_ms));
    if (!(b > a)) continue;
    if (!out.empty() && a - out.back().x1 < min_gap) out.back().x1 = std::max(out.back().x1, b);
    else out.push_back({a, b});
  }
  return out;
}

}  // namespace deckhand
