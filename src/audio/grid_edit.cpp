#include "deckhand/audio/grid_edit.hpp"

#include <algorithm>
#include <cmath>

namespace deckhand::gridedit {
namespace {

double beat_len(const GridSection& s, double sr) { return 60.0 / s.bpm * sr; }
int wrap_bar(long v) { return static_cast<int>(((v % 4) + 4) % 4) + 1; }

}  // namespace

Grid normalise(Grid g, double sr) {
  Grid out;
  std::sort(g.begin(), g.end(), [](const GridSection& a, const GridSection& b) { return a.first_beat_frame < b.first_beat_frame; });
  for (GridSection s : g) {
    if (!std::isfinite(s.first_beat_frame) || !std::isfinite(s.bpm) || s.bpm <= 0) continue;
    s.bpm = std::clamp(s.bpm, kMinBpm, kMaxBpm);
    s.bar_beat = std::clamp(s.bar_beat, 1, 4);
    if (out.empty() && s.first_beat_frame < 0) {
      const double n = std::ceil(-s.first_beat_frame / beat_len(s, sr));
      s.first_beat_frame += n * beat_len(s, sr);
      s.bar_beat = wrap_bar(static_cast<long>(s.bar_beat - 1) + static_cast<long>(n));
      if (s.first_beat_frame < 0) s.first_beat_frame = 0;
    }
    if (s.first_beat_frame < 0) continue;
    if (!out.empty() && s.first_beat_frame - out.back().first_beat_frame < 1.0) continue;
    if (static_cast<int>(out.size()) >= kMaxSections) break;
    out.push_back(s);
  }
  return out;
}

int section_at(const Grid& g, double at) {
  int idx = 0;
  for (std::size_t i = 0; i < g.size(); ++i)
    if (g[i].first_beat_frame <= at) idx = static_cast<int>(i);
  return idx;
}

double nearest_beat(const Grid& g, double at, double sr) {
  if (g.empty()) return -1;
  const int i = section_at(g, at);
  const GridSection& s = g[static_cast<std::size_t>(i)];
  const double bl = beat_len(s, sr);
  double k = std::round((at - s.first_beat_frame) / bl);
  if (k < 0) k = 0;
  double f = s.first_beat_frame + k * bl;
  if (static_cast<std::size_t>(i) + 1 < g.size() && f >= g[static_cast<std::size_t>(i) + 1].first_beat_frame)
    f = g[static_cast<std::size_t>(i) + 1].first_beat_frame;
  if (static_cast<std::size_t>(i) + 1 < g.size()) {  // the next section's first beat may be the closer one
    const double nx = g[static_cast<std::size_t>(i) + 1].first_beat_frame;
    if (std::fabs(nx - at) < std::fabs(f - at)) f = nx;
  }
  return f;
}

int bar_position_at(const Grid& g, double at, double sr) {
  if (g.empty()) return 0;
  const int i = section_at(g, at);
  const GridSection& s = g[static_cast<std::size_t>(i)];
  const double k = std::max(0.0, std::round((at - s.first_beat_frame) / beat_len(s, sr)));
  return wrap_bar(static_cast<long>(s.bar_beat - 1) + static_cast<long>(k));
}

void beats_in(const Grid& g, double from, double to, double length, double sr, std::vector<BeatMark>& out) {
  out.clear();
  for (std::size_t i = 0; i < g.size(); ++i) {
    const GridSection& s = g[i];
    const double bl = beat_len(s, sr);
    const double end = i + 1 < g.size() ? g[i + 1].first_beat_frame : length;
    if (end < from || s.first_beat_frame > to) continue;
    double k = std::max(0.0, std::ceil((from - s.first_beat_frame) / bl));
    for (double f = s.first_beat_frame + k * bl; f <= to && f < end - bl * 0.5; f = s.first_beat_frame + ++k * bl)
      out.push_back({f, wrap_bar(static_cast<long>(s.bar_beat - 1) + static_cast<long>(k))});
    if (out.size() > 4000) break;  // zoomed far out: drawing code decimates, this only bounds the work
  }
}

Grid shift(const Grid& g, double frames, double sr) {
  if (g.empty() || !std::isfinite(frames)) return g;
  Grid out = g;
  for (GridSection& s : out) s.first_beat_frame += frames;
  return normalise(std::move(out), sr);
}

Grid align(const Grid& g, double at, double sr) {
  const double nb = nearest_beat(g, at, sr);
  if (nb < 0 || !std::isfinite(at)) return g;
  return shift(g, at - nb, sr);
}

Grid set_downbeat(const Grid& g, double at, double sr) {
  if (g.empty()) return g;
  const int pos = bar_position_at(g, at, sr);
  return rotate_downbeat(g, 1 - pos);  // the beat now at `pos` moves to 1
}

Grid rotate_downbeat(const Grid& g, int steps) {
  Grid out = g;
  for (GridSection& s : out) s.bar_beat = wrap_bar(static_cast<long>(s.bar_beat - 1) + steps);
  return out;
}

Grid set_first_beat(const Grid& g, double at, double sr) { return set_downbeat(align(g, at, sr), at, sr); }

Grid set_bpm(const Grid& g, double at, double bpm, double sr) {
  if (g.empty() || !std::isfinite(bpm) || bpm <= 0) return g;
  bpm = std::clamp(bpm, kMinBpm, kMaxBpm);
  Grid out = g;
  const std::size_t i = static_cast<std::size_t>(section_at(g, at));
  GridSection& s = out[i];
  // Pivot: the beat nearest `at` (inside this section) keeps its frame and its bar position.
  const double old_len = beat_len(s, sr);
  double k = std::max(0.0, std::round((at - s.first_beat_frame) / old_len));
  const double anchor = s.first_beat_frame + k * old_len;
  const int anchor_bar = wrap_bar(static_cast<long>(s.bar_beat - 1) + static_cast<long>(k));
  s.bpm = bpm;
  if (i > 0) return normalise(std::move(out), sr);  // a later section keeps its start: it is where the tempo changes
  const double new_len = beat_len(s, sr);
  const double floor_at = i == 0 ? 0.0 : g[i - 1].first_beat_frame + 1.0;
  // Same number of beats before the anchor as before if they fit (the section keeps its beat count up to the pivot),
  // otherwise as many as fit above the floor.
  const double m = std::min(k, std::floor((anchor - floor_at) / new_len + 1e-9));
  s.first_beat_frame = anchor - m * new_len;
  s.bar_beat = wrap_bar(static_cast<long>(anchor_bar - 1) - static_cast<long>(m));
  return normalise(std::move(out), sr);
}

Grid bpm_delta(const Grid& g, double at, double delta, double sr) {
  if (g.empty()) return g;
  const double cur = g[static_cast<std::size_t>(section_at(g, at))].bpm;
  return set_bpm(g, at, std::round((cur + delta) * 1000.0) / 1000.0, sr);
}

Grid scale_bpm(const Grid& g, double factor, double sr) {
  if (g.empty() || !(factor > 0)) return g;
  Grid out = g;
  for (GridSection& s : out) s.bpm *= factor;
  if (!out.empty() && (out.front().bpm < kMinBpm || out.front().bpm > kMaxBpm)) return g;  // would leave the usable range
  return normalise(std::move(out), sr);
}

Grid add_section(const Grid& g, double at, double sr) {
  if (g.empty() || !std::isfinite(at) || at < 0 || static_cast<int>(g.size()) >= kMaxSections) return g;
  for (const GridSection& s : g)
    if (std::fabs(s.first_beat_frame - at) < 1.0) return g;
  const int i = section_at(g, at);
  const GridSection& cur = g[static_cast<std::size_t>(i)];
  GridSection n;
  n.first_beat_frame = at;
  n.bpm = cur.bpm;
  // continue the bar count: the position `at` would have had in the current section
  const double k = std::round((at - cur.first_beat_frame) / beat_len(cur, sr));
  n.bar_beat = wrap_bar(static_cast<long>(cur.bar_beat - 1) + static_cast<long>(k));
  Grid out = g;
  out.push_back(n);
  return normalise(std::move(out), sr);
}

Grid remove_section(const Grid& g, double at, double sr) {
  if (g.size() < 2) return g;
  Grid out = g;
  out.erase(out.begin() + section_at(g, at));
  return normalise(std::move(out), sr);
}

double tap_bpm(const std::vector<double>& times) {
  std::vector<double> t;
  for (const double x : times) {
    if (!t.empty() && x - t.back() > 2.5) t.clear();  // a pause starts a new count
    if (!t.empty() && x <= t.back()) continue;
    t.push_back(x);
  }
  if (t.size() > 8) t.erase(t.begin(), t.end() - 8);
  if (t.size() < 3) return 0;
  const double bpm = 60.0 * static_cast<double>(t.size() - 1) / (t.back() - t.front());
  return std::isfinite(bpm) && bpm >= kMinBpm && bpm <= kMaxBpm ? bpm : 0;
}

bool same(const Grid& a, const Grid& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::fabs(a[i].first_beat_frame - b[i].first_beat_frame) > 1e-3 || std::fabs(a[i].bpm - b[i].bpm) > 1e-3 ||
        a[i].bar_beat != b[i].bar_beat)
      return false;
  return true;
}

}  // namespace deckhand::gridedit
