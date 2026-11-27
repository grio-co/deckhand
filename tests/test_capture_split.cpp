// Track splitting on synthetic vinyl sides: known gaps, surface noise, crackle, clicks and rumble.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "capture_synth.hpp"
#include "check.hpp"
#include "deckhand/capture/source.hpp"
#include "deckhand/capture/split.hpp"

using namespace deckhand::capture;
using namespace synth;

namespace {

SplitAnalysis analyse(const Side& s, const SplitSettings& ss) {
  MemorySource src(s.pcm.data(), s.frames(), 2, s.rate);
  return analyse_splits(src, ss);
}

double secs(std::uint64_t f, std::uint32_t rate) { return static_cast<double>(f) / rate; }

// Truth: the cut sits in the middle of the silence between tracks k and k+1.
double truth_cut(const Side& s, std::size_t k) { return secs((s.ends[k] + s.starts[k + 1]) / 2, s.rate); }

SideSpec three_tracks() {
  SideSpec sp;
  sp.tracks = {{40, 0.2, 0, 0, false, 220, 0.05}, {35, 0.25, 0, 0, false, 330, 0.05}, {45, 0.15, 0, 0, false, 247, 0.05}};
  return sp;
}

}  // namespace

int main() {
  const SplitSettings ss;

  // ---- clean side: the baseline ----------------------------------------------------------------------------------
  {
    SideSpec sp = three_tracks();
    const Side s = make_side(sp);
    const SplitAnalysis a = analyse(s, ss);
    const auto pts = find_splits(a, ss);
    CHECK(pts.size() == 2);
    for (std::size_t k = 0; k < 2; ++k) {
      CHECK(std::fabs(secs(pts[k].frame, s.rate) - truth_cut(s, k)) < 0.05);
      CHECK(pts[k].confidence > 0.6);
    }
  }

  // ---- dirty side: groove noise, 12 clicks per second, 30 Hz rumble that is louder than the noise ---------------------------
  for (std::uint32_t rate : {44100u, 48000u}) {
    SideSpec sp = three_tracks();
    sp.rate = rate;
    sp.crackle_per_s = 12;
    sp.click_amp = 0.3;
    sp.rumble = 0.02;
    sp.noise = 0.003;
    const Side s = make_side(sp);
    const SplitAnalysis a = analyse(s, ss);
    const SplitLevels lv = split_levels(a, ss);
    CHECK(lv.splittable);
    const auto pts = find_splits(a, ss);
    CHECK(pts.size() == 2);
    for (std::size_t k = 0; k < pts.size(); ++k) {
      CHECK(std::fabs(secs(pts[k].frame, rate) - truth_cut(s, k)) < 0.05);
      CHECK(pts[k].gap_start < pts[k].frame && pts[k].frame < pts[k].gap_end);
      CHECK(pts[k].origin == SplitPoint::Origin::Auto);
    }
    // Pieces: lead-in and run-out are trimmed in the trimmed plan, the tracks come out whole.
    const auto pieces = plan_pieces(a, ss, pts, false);
    CHECK(pieces.size() == 3);
    for (std::size_t k = 0; k < 3; ++k) {
      CHECK(!pieces[k].empty);
      CHECK(std::fabs(secs(pieces[k].start, rate) - secs(s.starts[k], rate)) < 0.15);
      CHECK(std::fabs(secs(pieces[k].end, rate) - secs(s.ends[k], rate)) < 0.5);
      CHECK(pieces[k].start <= s.starts[k] + 2205 && pieces[k].end + 2205 >= s.ends[k]);  // never cuts into the music
    }
    // Contiguous plan: tiles the audible range without a lost frame.
    const auto tiled = plan_pieces(a, ss, pts, true);
    CHECK(tiled.size() == 3);
    CHECK(tiled[0].end == tiled[1].start && tiled[1].end == tiled[2].start);
    CHECK(tiled[0].end == pts[0].frame && tiled[1].end == pts[1].frame);
  }

  // ---- quiet intro: a soft tone, and a fade-in out of the noise; neither is cut or taken for a gap ------------------------
  {
    SideSpec sp = three_tracks();
    sp.tracks[1].intro_s = 4.0;     // 4 s at -38 dBFS before the track opens up
    sp.tracks[1].intro_level = 0.0125;
    sp.noise = 0.002;
    sp.crackle_per_s = 6;
    const Side s = make_side(sp);
    const SplitAnalysis a = analyse(s, ss);
    const auto pts = find_splits(a, ss);
    CHECK(pts.size() == 2);
    const auto pieces = plan_pieces(a, ss, pts, false);
    CHECK(pieces.size() == 3);
    CHECK(pieces[1].start <= s.starts[1] + 4410);  // the intro is kept from its first sample
    CHECK(std::fabs(secs(pts[0].frame, s.rate) - truth_cut(s, 0)) < 0.05);
  }
  {
    SideSpec sp = three_tracks();
    sp.tracks[2].intro_s = 3.0;
    sp.tracks[2].intro_fade = true;  // fades in from -35 dB below the track level, through the noise
    const Side s = make_side(sp);
    const SplitAnalysis a = analyse(s, ss);
    const auto pts = find_splits(a, ss);
    CHECK(pts.size() == 2);
    const auto pieces = plan_pieces(a, ss, pts, false);
    CHECK(pieces.size() == 3);
    CHECK(secs(pieces[2].start, s.rate) <= secs(s.starts[2], s.rate) + 1.0);  // fade tolerance keeps the start of the fade
    CHECK(secs(pieces[2].start, s.rate) >= secs(pts[1].frame, s.rate));
  }

  // ---- live album: no gaps, dynamics and crowd noise -> no splits at all -------------------------------------------------
  {
    SideSpec sp;
    sp.tracks = {{200, 0.2, 0, 0, false, 196, 0.05}};
    sp.noise = 0.004;  // crowd
    sp.crackle_per_s = 4;
    Side s = make_side(sp);
    // Slow swells down to -12 dB on top of what the swell already does.
    for (std::uint64_t i = 0; i < s.frames(); ++i) {
      const double g = 0.65 + 0.35 * std::sin(2 * kPi * 0.05 * static_cast<double>(i) / s.rate);
      s.pcm[i * 2] = static_cast<float>(s.pcm[i * 2] * g);
      s.pcm[i * 2 + 1] = static_cast<float>(s.pcm[i * 2 + 1] * g);
    }
    const SplitAnalysis a = analyse(s, ss);
    CHECK(find_splits(a, ss).empty());
    // Even with the thresholds loosened a lot, steady music with a pause of a few notes is not a groove gap.
    SplitSettings loose = ss;
    loose.min_depth_db = 8;
    loose.min_gap_s = 0.5;
    CHECK(find_splits(a, loose).empty() || find_splits(a, loose).size() <= 0);
  }

  // ---- minimum track length: a 6 s "track" between two gaps is merged, the weaker gap goes ---------------------------------
  {
    SideSpec sp = three_tracks();
    sp.tracks[1].seconds = 6;
    sp.gap_s = 2.0;
    const Side s = make_side(sp);
    const SplitAnalysis a = analyse(s, ss);
    CHECK(find_splits(a, ss).size() == 1);
    SplitSettings short_ok = ss;
    short_ok.min_track_s = 3;
    CHECK(find_splits(a, short_ok).size() == 2);
    SplitSettings long_gap = ss;
    long_gap.min_gap_s = 3.0;  // the gaps here are 2 s
    CHECK(find_splits(a, long_gap).empty());
  }

  // ---- editing: add, move, remove, re-run ----------------------------------------------------------------------------------
  {
    SideSpec sp = three_tracks();
    const Side s = make_side(sp);
    const SplitAnalysis a = analyse(s, ss);
    SplitPlan plan;
    plan.sample_rate = s.rate;
    plan.set_auto(find_splits(a, ss));
    CHECK(plan.points.size() == 2);
    const std::uint64_t first = plan.points[0].frame;
    CHECK(plan.remove(0));
    CHECK(plan.points.size() == 1 && plan.vetoes.size() == 1);
    plan.set_auto(find_splits(a, ss));  // a re-run does not bring back what was removed
    CHECK(plan.points.size() == 1 && plan.points[0].frame != first);
    const std::uint64_t spot = nearest_quiet_frame(a, s.starts[1] - 22050 + 40000, 2.0);  // near the (removed) gap
    CHECK(std::fabs(secs(spot, s.rate) - truth_cut(s, 0)) < 1.3);
    const std::size_t at = plan.add(spot);
    CHECK(at == 0 && plan.points[0].origin == SplitPoint::Origin::User && plan.points[0].confidence == 1.0);
    plan.set_auto(find_splits(a, ss));  // the user's point survives and the automatic twin is not duplicated
    CHECK(plan.points.size() == 2);
    CHECK(plan.move(0, spot + 4410));
    CHECK(plan.points[0].frame == spot + 4410 && plan.points[0].origin == SplitPoint::Origin::User);
    CHECK(!plan.move(9, 0) && !plan.remove(9));
    const auto pieces = plan_pieces(a, ss, plan.points, true);
    CHECK(pieces.size() == 3 && pieces[0].end == spot + 4410);
    plan.clear_user_edits();
    plan.set_auto(find_splits(a, ss));
    CHECK(plan.points.size() == 2);  // the moved user point stays and still stands in for the automatic one near it
    CHECK(plan.points[0].origin == SplitPoint::Origin::User);
  }

  std::printf("capture split ok\n");
  return 0;
}
