#include "deckhand/capture/split.hpp"

#include <algorithm>
#include <cmath>

#include "biquad.hpp"

namespace deckhand::capture {
namespace {

constexpr int kSubPerHop = 5;  // five 2 ms sub-blocks per 10 ms hop; the hop level is their median, so clicks do not count

double percentile(std::vector<float> v, double p) {
  if (v.empty()) return -100;
  const std::size_t k = std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

std::size_t hops_of(const SplitAnalysis& a, double seconds) {
  return static_cast<std::size_t>(std::max(1.0, std::round(seconds / a.hop_seconds())));
}

}  // namespace

SplitAnalysis analyse_splits(SampleSource& src, const SplitSettings& s) {
  SplitAnalysis a;
  a.sample_rate = src.sample_rate();
  a.frames = src.frames();
  const std::uint32_t ch = std::max<std::uint32_t>(1, src.channels());
  const std::uint32_t sub = std::max<std::uint32_t>(8, static_cast<std::uint32_t>(a.sample_rate * 0.002));
  a.hop_frames = sub * kSubPerHop;
  a.env_db.reserve(static_cast<std::size_t>(a.frames / a.hop_frames) + 1);
  HighPass4 hp(std::min(s.rumble_hz, a.sample_rate * 0.2), a.sample_rate);
  std::vector<float> buf(static_cast<std::size_t>(16384) * ch);
  double e[kSubPerHop] = {0};
  int sub_i = 0;
  std::uint32_t in_sub = 0;
  double acc = 0;
  for (std::uint64_t at = 0; at < a.frames;) {
    const std::uint64_t n = src.read(at, 16384, buf.data());
    if (n == 0) break;
    for (std::uint64_t i = 0; i < n; ++i) {
      double m = 0;
      for (std::uint32_t c = 0; c < ch; ++c) m += buf[i * ch + c];
      m = hp.process(m / ch);
      acc += m * m;
      if (++in_sub == sub) {
        e[sub_i++] = acc / sub;
        acc = 0;
        in_sub = 0;
        if (sub_i == kSubPerHop) {
          sub_i = 0;
          double t[kSubPerHop];
          std::copy(e, e + kSubPerHop, t);
          std::nth_element(t, t + kSubPerHop / 2, t + kSubPerHop);
          a.env_db.push_back(static_cast<float>(std::max(-100.0, 10.0 * std::log10(t[kSubPerHop / 2] + 1e-12))));
        }
      }
    }
    at += n;
  }
  return a;
}

SplitLevels split_levels(const SplitAnalysis& a, const SplitSettings& s) {
  SplitLevels L;
  const std::size_t n = a.env_db.size();
  if (n < 20) return L;
  // The noise floor: the quietest half second (minimum statistics), robust to a few clicks and to rare very quiet spots.
  const std::size_t w = std::min<std::size_t>(n, hops_of(a, 0.5));
  std::vector<double> pre(n + 1, 0.0);
  for (std::size_t i = 0; i < n; ++i) pre[i + 1] = pre[i] + a.env_db[i];
  double best = 1e9;
  for (std::size_t i = 0; i + w <= n; ++i) best = std::min(best, (pre[i + w] - pre[i]) / static_cast<double>(w));
  L.floor_db = best;
  L.programme_db = percentile(a.env_db, 0.6);
  L.gap_threshold_db = std::min(L.floor_db + s.adapt_db, L.programme_db - s.min_depth_db);
  L.splittable = L.programme_db - L.floor_db >= s.min_depth_db && L.gap_threshold_db > L.floor_db + 1.0;
  return L;
}

bool audible_range(const SplitAnalysis& a, const SplitSettings& s, std::uint64_t from, std::uint64_t to, std::uint64_t& start, std::uint64_t& end) {
  const SplitLevels L = split_levels(a, s);
  const std::size_t n = a.env_db.size();
  const std::size_t h0 = std::min<std::size_t>(n, from / a.hop_frames), h1 = std::min<std::size_t>(n, (to + a.hop_frames - 1) / a.hop_frames);
  if (h1 <= h0 + 5) return false;
  const std::size_t edge = std::min<std::size_t>(h1 - h0, hops_of(a, 3.0));
  const double noise_in = percentile(std::vector<float>(a.env_db.begin() + h0, a.env_db.begin() + h0 + edge), 0.1);
  const double noise_out = percentile(std::vector<float>(a.env_db.begin() + h1 - edge, a.env_db.begin() + h1), 0.1);
  const double cap = L.splittable ? L.gap_threshold_db : 1e9;
  const double on_in = std::min(noise_in + s.onset_db, cap), on_out = std::min(noise_out + s.onset_db, cap);
  const double fa_in = std::min(noise_in + s.fade_db, cap), fa_out = std::min(noise_out + s.fade_db, cap);
  auto loud = [&](std::size_t i, double thr) { return a.env_db[i] > thr; };
  const std::size_t tol = hops_of(a, s.fade_tolerance_s);
  std::size_t first = h1, last = h0;
  for (std::size_t i = h0; i + 5 <= h1; ++i) {  // onset: at least 4 of 5 hops above the threshold
    int c = 0;
    for (std::size_t k = 0; k < 5; ++k) c += loud(i + k, on_in);
    if (c >= 4) { first = i; break; }
  }
  if (first >= h1) return false;
  for (std::size_t i = h1; i >= h0 + 5; --i) {
    int c = 0;
    for (std::size_t k = 1; k <= 5; ++k) c += loud(i - k, on_out);
    if (c >= 4) { last = i; break; }
  }
  if (last <= first) return false;
  for (std::size_t back = 0; back < tol && first > h0 && loud(first - 1, fa_in); ++back) --first;
  for (std::size_t fwd = 0; fwd < tol && last < h1 && loud(last, fa_out); ++fwd) ++last;
  const std::uint64_t pad_in = static_cast<std::uint64_t>(s.lead_in_pad_s * a.sample_rate), pad_out = static_cast<std::uint64_t>(s.lead_out_pad_s * a.sample_rate);
  start = std::max<std::uint64_t>(from, first * a.hop_frames > pad_in ? first * a.hop_frames - pad_in : 0);
  end = std::min<std::uint64_t>(to, last * a.hop_frames + pad_out);
  return end > start;
}

std::vector<SplitPoint> find_splits(const SplitAnalysis& a, const SplitSettings& s) {
  std::vector<SplitPoint> out;
  const SplitLevels L = split_levels(a, s);
  if (!L.splittable) return out;
  const std::size_t n = a.env_db.size();
  const std::size_t min_len = hops_of(a, s.min_gap_s), bridge = 3;
  struct Run { std::size_t b, e; double noise, conf; };
  std::vector<Run> runs;
  for (std::size_t i = 0; i < n;) {
    if (a.env_db[i] > L.gap_threshold_db) { ++i; continue; }
    std::size_t j = i, last_low = i;
    while (j < n && j - last_low <= bridge) {  // bridge short blips (a loud crackle that got through)
      if (a.env_db[j] <= L.gap_threshold_db) last_low = j;
      ++j;
    }
    const std::size_t b = i, e = last_low + 1;
    i = std::max(e, i + 1);
    if (b == 0 || e >= n) continue;       // touching the start or end: lead-in / run-out groove, not a split
    if (e - b < min_len) continue;
    const std::vector<float> seg(a.env_db.begin() + b, a.env_db.begin() + e);
    const double q1 = percentile(seg, 0.25), q3 = percentile(seg, 0.75);
    if (q3 - q1 > s.max_gap_spread_db) continue;  // music pausing, not groove noise
    const double depth = L.programme_db - q1;
    const double len_s = (e - b) * a.hop_seconds();
    double conf = 0.4 * std::clamp((depth - 10.0) / 30.0, 0.0, 1.0) + 0.3 * std::clamp(len_s / (3.0 * s.min_gap_s), 0.0, 1.0) +
                  0.3 * (1.0 - std::clamp((q3 - q1) / s.max_gap_spread_db, 0.0, 1.0));
    if (conf < s.min_confidence) continue;
    runs.push_back({b, e, q1, conf});
  }
  std::vector<SplitPoint> pts;
  for (const Run& r : runs) {
    SplitPoint p;
    p.frame = (r.b + r.e) / 2 * a.hop_frames;
    p.gap_start = r.b * a.hop_frames;
    p.gap_end = r.e * a.hop_frames;
    p.confidence = r.conf;
    p.noise_db = r.noise;
    pts.push_back(p);
  }
  // Minimum track length, measured on the audible part: merge across the weaker neighbouring gap until nothing is short.
  for (;;) {
    std::vector<std::uint64_t> b;
    b.push_back(0);
    for (const auto& p : pts) b.push_back(p.frame);
    b.push_back(a.frames);
    std::size_t worst = pts.size() + 1;
    double worst_len = 1e18;
    for (std::size_t k = 0; k + 1 < b.size(); ++k) {
      std::uint64_t st, en;
      const double len = audible_range(a, s, b[k], b[k + 1], st, en) ? static_cast<double>(en - st) / a.sample_rate : 0.0;
      if (len < s.min_track_s && len < worst_len) { worst_len = len; worst = k; }
    }
    if (worst > pts.size() || pts.empty()) break;
    std::size_t drop;
    if (worst == 0) drop = 0;
    else if (worst == pts.size()) drop = pts.size() - 1;
    else drop = pts[worst - 1].confidence <= pts[worst].confidence ? worst - 1 : worst;
    pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(drop));
  }
  // Settle each cut midway between the audible end of the piece before it and the audible start of the piece after it.
  std::vector<std::uint64_t> b{0};
  for (const auto& p : pts) b.push_back(p.frame);
  b.push_back(a.frames);
  for (std::size_t k = 0; k < pts.size(); ++k) {
    std::uint64_t s0, e0, s1, e1;
    const std::uint64_t pad_out = static_cast<std::uint64_t>(s.lead_out_pad_s * a.sample_rate), pad_in = static_cast<std::uint64_t>(s.lead_in_pad_s * a.sample_rate);
    if (!audible_range(a, s, b[k], b[k + 1], s0, e0)) continue;
    if (!audible_range(a, s, b[k + 1], b[k + 2], s1, e1)) continue;
    const std::uint64_t ae = e0 > pad_out ? e0 - pad_out : e0, as = s1 + pad_in;  // undo the pads: the bare audible edges
    if (as <= ae) continue;
    pts[k].gap_start = ae;
    pts[k].gap_end = as;
    pts[k].frame = (ae + as) / 2;
  }
  return pts;
}

std::uint64_t nearest_quiet_frame(const SplitAnalysis& a, std::uint64_t frame, double radius_s) {
  const std::size_t n = a.env_db.size();
  if (n == 0) return frame;
  const std::size_t c = std::min<std::size_t>(n - 1, frame / a.hop_frames), r = hops_of(a, radius_s);
  const std::size_t lo = c > r ? c - r : 0, hi = std::min(n, c + r + 1);
  std::size_t best = c;
  double bv = 1e9;
  for (std::size_t i = lo; i + 3 <= hi; ++i) {  // quietest 3-hop mean
    const double v = (a.env_db[i] + a.env_db[i + 1] + a.env_db[i + 2]) / 3.0;
    if (v < bv) { bv = v; best = i + 1; }
  }
  return best * a.hop_frames;
}

std::vector<Piece> plan_pieces(const SplitAnalysis& a, const SplitSettings& s, const std::vector<SplitPoint>& points, bool contiguous) {
  std::vector<std::uint64_t> b{0};
  for (const auto& p : points) b.push_back(std::min(p.frame, a.frames));
  b.push_back(a.frames);
  std::vector<Piece> raw;
  for (std::size_t k = 0; k + 1 < b.size(); ++k) {
    Piece p;
    p.confidence = k == 0 ? 1.0 : points[k - 1].confidence;
    p.empty = !audible_range(a, s, b[k], b[k + 1], p.start, p.end);
    if (p.empty) { p.start = b[k]; p.end = b[k + 1]; }
    raw.push_back(p);
  }
  if (!contiguous) return raw;
  std::vector<std::size_t> live;
  for (std::size_t k = 0; k < raw.size(); ++k) if (!raw[k].empty) live.push_back(k);
  std::vector<Piece> out;
  for (std::size_t m = 0; m < live.size(); ++m) {
    Piece p = raw[live[m]];
    p.start = m == 0 ? raw[live[m]].start : out.back().end;
    p.end = m + 1 == live.size() ? raw[live[m]].end : b[live[m + 1]];
    out.push_back(p);
  }
  return out;
}

void SplitPlan::set_auto(const std::vector<SplitPoint>& found) {
  std::vector<SplitPoint> keep;
  for (const auto& p : points) if (p.origin == SplitPoint::Origin::User) keep.push_back(p);
  const std::uint64_t radius = static_cast<std::uint64_t>(veto_radius_s * sample_rate);
  auto near_any = [&](std::uint64_t f, const std::vector<std::uint64_t>& v) {
    for (auto x : v) if ((f > x ? f - x : x - f) <= radius) return true;
    return false;
  };
  std::vector<std::uint64_t> user_frames;
  for (const auto& p : keep) user_frames.push_back(p.frame);
  for (const auto& p : found) {
    if (near_any(p.frame, vetoes) || near_any(p.frame, user_frames)) continue;
    SplitPoint q = p;
    q.origin = SplitPoint::Origin::Auto;
    keep.push_back(q);
  }
  std::sort(keep.begin(), keep.end(), [](const SplitPoint& x, const SplitPoint& y) { return x.frame < y.frame; });
  points = keep;
}

std::size_t SplitPlan::add(std::uint64_t frame) {
  SplitPoint p;
  p.frame = frame;
  p.gap_start = p.gap_end = frame;
  p.origin = SplitPoint::Origin::User;
  std::size_t at = 0;
  while (at < points.size() && points[at].frame < frame) ++at;
  points.insert(points.begin() + static_cast<std::ptrdiff_t>(at), p);
  return at;
}

bool SplitPlan::move(std::size_t index, std::uint64_t frame) {
  if (index >= points.size()) return false;
  SplitPoint p = points[index];
  if (p.origin == SplitPoint::Origin::Auto) vetoes.push_back(p.frame);  // the old spot must not be re-found
  p.frame = frame;
  p.origin = SplitPoint::Origin::User;
  p.confidence = 1.0;
  points.erase(points.begin() + static_cast<std::ptrdiff_t>(index));
  std::size_t at = 0;
  while (at < points.size() && points[at].frame < frame) ++at;
  points.insert(points.begin() + static_cast<std::ptrdiff_t>(at), p);
  return true;
}

bool SplitPlan::remove(std::size_t index) {
  if (index >= points.size()) return false;
  if (points[index].origin == SplitPoint::Origin::Auto) vetoes.push_back(points[index].frame);
  points.erase(points.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

}  // namespace deckhand::capture
