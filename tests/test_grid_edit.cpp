// Beat grid editing functions (src/audio/grid_edit.cpp).
#include <cmath>
#include <cstdio>

#include "check.hpp"
#include "deckhand/audio/grid_edit.hpp"

using namespace deckhand;
using namespace deckhand::gridedit;

namespace {
const double kSr = 44100;
bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }
double bl(double bpm) { return 60.0 / bpm * kSr; }
}  // namespace

int main() {
  const Grid base = {{1000, 120, 1}};
  // shift by whole frames, ms
  Grid g = shift(base, 44.1, kSr);  // +1 ms
  CHECK(near(g[0].first_beat_frame, 1044.1) && g[0].bpm == 120 && g[0].bar_beat == 1);
  g = shift(base, -44.1 * 100, kSr);  // -100 ms: before the start, so the first beat moves on by whole beats (bar follows)
  CHECK(g[0].first_beat_frame >= 0 && near(std::fmod(g[0].first_beat_frame - (1000 - 4410), bl(120)), 0, 1e-6));
  CHECK(g[0].bar_beat == 2);  // one beat skipped

  // align: beat nearest the playhead lands on it
  g = align(base, 25000, kSr);  // beats at 1000, 23050, 45100: nearest to 25000 is 23050 -> moves to 25000
  CHECK(near(nearest_beat(g, 25000, kSr), 25000, 1e-6));
  CHECK(near(g[0].first_beat_frame, 1000 + 1950, 1e-6));
  // downbeat: the beat nearest the playhead becomes bar beat 1
  g = set_downbeat(base, 23050, kSr);  // beat index 1 is currently bar position 2
  CHECK(bar_position_at(g, 23050, kSr) == 1 && bar_position_at(g, 1000, kSr) == 4);
  g = rotate_downbeat(base, 1);
  CHECK(g[0].bar_beat == 2);
  g = rotate_downbeat(base, -1);
  CHECK(g[0].bar_beat == 4);
  g = set_first_beat(base, 30000, kSr);
  CHECK(near(nearest_beat(g, 30000, kSr), 30000, 1e-6) && bar_position_at(g, 30000, kSr) == 1);

  // BPM: pivots on the beat nearest the playhead
  g = bpm_delta(base, 1000 + bl(120) * 4, 0.5, kSr);  // playhead on beat 4 (frame 89200)
  CHECK(near(g[0].bpm, 120.5, 1e-9));
  CHECK(near(nearest_beat(g, 89200, kSr), 89200, 1e-6));  // the pivot did not move
  CHECK(g[0].first_beat_frame >= 0);
  g = bpm_delta(base, 1000, -0.01, kSr);
  CHECK(near(g[0].bpm, 119.99, 1e-9) && near(g[0].first_beat_frame, 1000, 1e-6));
  g = set_bpm(base, 1000, 500, kSr);
  CHECK(g[0].bpm == kMaxBpm);
  g = scale_bpm(base, 0.5, kSr);
  CHECK(g[0].bpm == 60 && near(g[0].first_beat_frame, 1000));
  g = scale_bpm(base, 2, kSr);
  CHECK(g[0].bpm == 240);
  CHECK(scale_bpm(Grid{{0, 200, 1}}, 2, kSr)[0].bpm == 200);  // 400 would leave the range: unchanged
  // slowing a grid whose pivot is far in must not push the first beat below zero
  g = set_bpm({{100, 150, 1}}, 600000, 100, kSr);
  CHECK(g[0].first_beat_frame >= 0 && near(nearest_beat(g, 599860, kSr), 599860, 1e-3));

  // sections
  g = add_section(base, 441000, kSr);  // 10 s
  CHECK(g.size() == 2 && g[1].first_beat_frame == 441000 && g[1].bpm == 120);
  CHECK(g[1].bar_beat == bar_position_at(base, 441000, kSr));
  g = set_bpm(g, 500000, 100, kSr);  // edits the second section only
  CHECK(g.size() == 2 && g[0].bpm == 120 && g[1].bpm == 100);
  CHECK(add_section(g, 441000.4, kSr).size() == 2);  // same place: nothing
  Grid r = remove_section(g, 500000, kSr);
  CHECK(r.size() == 1 && r[0].bpm == 120);
  r = remove_section(g, 10, kSr);  // removing the first one: the second becomes the grid
  CHECK(r.size() == 1 && r[0].bpm == 100);
  CHECK(remove_section(base, 0, kSr).size() == 1);  // the last section stays
  CHECK(section_at(g, 441000) == 1 && section_at(g, 440999) == 0);
  // the nearest beat never crosses into the next section
  g = {{0, 60, 1}, {50000, 120, 1}};
  CHECK(near(nearest_beat(g, 47000, kSr), 44100) && near(nearest_beat(g, 49900, kSr), 50000));

  // beats_in
  std::vector<BeatMark> marks;
  beats_in(base, 0, 1000 + bl(120) * 4.5, 441000, kSr, marks);
  CHECK(marks.size() == 5 && marks[0].bar_pos == 1 && marks[4].bar_pos == 1 && marks[3].bar_pos == 4);

  // tap tempo
  CHECK(tap_bpm({0, 0.5}) == 0);
  CHECK(near(tap_bpm({0, 0.5, 1.0, 1.5}), 120, 1e-9));
  CHECK(near(tap_bpm({0, 0.4, 0.8, 1.2, 1.6}), 150, 1e-9));
  CHECK(near(tap_bpm({0, 0.5, 1.0, 10.0, 10.4615, 10.923}), 60.0 * 2 / 0.923, 1e-9));  // a pause restarts the count
  CHECK(tap_bpm({0, 5, 10}) == 0);  // 12 BPM is not a tempo

  // normalise drops junk
  g = normalise({{500, 0, 1}, {std::nan(""), 120, 1}, {-10, 120, 9}}, kSr);
  CHECK(g.size() == 1 && g[0].first_beat_frame >= 0 && g[0].bar_beat >= 1 && g[0].bar_beat <= 4);
  CHECK(same(base, base) && !same(base, shift(base, 1, kSr)));
  CHECK(shift({}, 100, kSr).empty() && align({}, 5, kSr).empty());
  std::puts("test_grid_edit: ok");
  return 0;
}
