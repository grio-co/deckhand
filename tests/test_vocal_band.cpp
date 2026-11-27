// The vocal band drawn on the overview waveform: regions mapped onto the strip, clipped, tiny neighbours merged.
#include <cstdio>
#include <vector>

#include "check.hpp"
#include "vocal_band.hpp"

using namespace deckhand;

int main() {
  const std::vector<StoredVocalRegion> r = {{1000, 2000}, {5000, 5001}, {5002, 5003}, {9000, 12000}};
  // a 10 s track on a 1000 px strip starting at x = 50: 1 px = 10 ms
  auto s = vocal_spans(r, 0, 10000, 50, 1000);
  CHECK(s.size() == 3);
  CHECK(s[0].x0 > 149.9f && s[0].x0 < 150.1f && s[0].x1 > 249.9f && s[0].x1 < 250.1f);
  CHECK(s[1].x0 > 549.9f && s[1].x1 > 550.2f && s[1].x1 < 550.4f);  // the two 1 px regions 1 px apart became one span
  CHECK(s[2].x0 > 949.9f && s[2].x1 > 1049.9f && s[2].x1 < 1050.1f);  // clipped at the strip's right edge
  CHECK(vocal_spans(r, 0, 10000, 50, 1000, 0.0f).size() == 4);       // without merging
  // a window of the track: only what shows, clipped at both edges
  s = vocal_spans(r, 1500, 2500, 0, 100);
  CHECK(s.size() == 1 && s[0].x0 == 0.0f && s[0].x1 > 49.9f && s[0].x1 < 50.1f);
  // nothing: no regions, an empty or reversed view, no width
  CHECK(vocal_spans({}, 0, 10000, 0, 100).empty());
  CHECK(vocal_spans(r, 5, 5, 0, 100).empty() && vocal_spans(r, 10, 5, 0, 100).empty() && vocal_spans(r, 0, 10, 0, 0).empty());
  CHECK(vocal_spans(r, 20000, 30000, 0, 100).empty());
  // hundreds of tiny regions collapse to a handful of rectangles
  std::vector<StoredVocalRegion> many;
  for (int i = 0; i < 500; ++i) many.push_back({i * 20.0, i * 20.0 + 15.0});
  CHECK(vocal_spans(many, 0, 10000, 0, 200).size() == 1);
  std::printf("vocal band ok\n");
  return 0;
}
