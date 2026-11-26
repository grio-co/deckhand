#include "deckhand/app/automix.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <random>
#include <set>

#include "deckhand/log/log.hpp"

namespace deckhand {

// ---- settings ------------------------------------------------------------------------------------------------------

bool AutomixSettings::valid_type(const std::string& t) {
  return t == "crossfade" || t == "cut" || t == "fade" || t == "echo_out";
}

void AutomixSettings::load(Library& lib) {
  const auto num = [&](const char* key, double fallback) {
    const std::string v = lib.setting(key, "");
    return v.empty() ? fallback : std::atof(v.c_str());
  };
  const std::string type = lib.setting("automix.transition_type", transition_type);
  if (valid_type(type)) transition_type = type;
  const std::string fallback = lib.setting("automix.unsynced_type", unsynced_type);
  if (valid_type(fallback)) unsynced_type = fallback;
  transition_beats = std::clamp(num("automix.transition_beats", transition_beats), 1.0, 64.0);
  mix_out_bars = std::clamp(num("automix.mix_out_bars", mix_out_bars), 0.0, 128.0);
  max_sync_gap_pct = std::clamp(num("automix.max_sync_gap_pct", max_sync_gap_pct), 0.0, 50.0);
  bass_swap = lib.setting("automix.bass_swap", bass_swap ? "1" : "0") != "0";
  const std::string mode = lib.setting("automix.override_mode", override_mode);
  if (mode == "yield" || mode == "hands_off") override_mode = mode;
  const std::string empty = lib.setting("automix.on_empty", on_empty);
  if (empty == "finish" || empty == "fade") on_empty = empty;
  fade_seconds = std::clamp(num("automix.fade_seconds", fade_seconds), 1.0, 60.0);
  endless = lib.setting("queue.endless", endless ? "1" : "0") != "0";
  endless_source = lib.setting("queue.endless_source", endless_source);
  endless_keep = static_cast<int>(std::clamp(num("queue.endless_keep", endless_keep), 1.0, 100.0));
  const std::string order = lib.setting("queue.endless_order", endless_order);
  if (order == "sequential" || order == "shuffle") endless_order = order;
}

void AutomixSettings::save(Library& lib) const {
  const auto n = [](double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return std::string(b);
  };
  lib.set_setting("automix.transition_type", transition_type);
  lib.set_setting("automix.unsynced_type", unsynced_type);
  lib.set_setting("automix.transition_beats", n(transition_beats));
  lib.set_setting("automix.mix_out_bars", n(mix_out_bars));
  lib.set_setting("automix.max_sync_gap_pct", n(max_sync_gap_pct));
  lib.set_setting("automix.bass_swap", bass_swap ? "1" : "0");
  lib.set_setting("automix.override_mode", override_mode);
  lib.set_setting("automix.on_empty", on_empty);
  lib.set_setting("automix.fade_seconds", n(fade_seconds));
  lib.set_setting("queue.endless", endless ? "1" : "0");
  lib.set_setting("queue.endless_source", endless_source);
  lib.set_setting("queue.endless_keep", std::to_string(endless_keep));
  lib.set_setting("queue.endless_order", endless_order);
}

std::int64_t AutomixHost::now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

TrackFacts read_track_facts(Library& lib, std::int64_t track_id) {
  TrackFacts f;
  const auto row = lib.track(track_id);
  if (!row) return f;
  f.ok = true;
  f.id = track_id;
  f.title = row->title;
  f.bpm = row->bpm;
  f.grid = lib.beatgrid(track_id);
  f.duration_ms = static_cast<double>(row->duration_ms);
  f.cues = lib.track_cues(track_id);
  f.points = lib.automix_points(track_id);
  if (auto sp = lib.phrases(track_id)) f.phrases = std::move(*sp);
  return f;
}

std::vector<std::int64_t> resolve_track_source(Library& lib, const std::string& source) {
  const auto colon = source.find(':');
  if (colon == std::string::npos) return {};
  const std::string kind = source.substr(0, colon), arg = source.substr(colon + 1);
  if (kind == "playlist") return lib.playlist_track_ids(std::atoll(arg.c_str()));
  if (kind == "smart") return lib.smart_track_ids(std::atoll(arg.c_str()));
  if (kind == "search") return lib.track_ids(0, arg);
  return {};
}

// ---- beat grid ------------------------------------------------------------------------------------------------------

namespace automix {

BeatGrid BeatGrid::make(const std::vector<BeatSection>& grid_ms, double fallback_bpm, double sample_rate) {
  BeatGrid g;
  g.sr = sample_rate > 0 ? sample_rate : 44100;
  if (grid_ms.empty()) {
    if (fallback_bpm <= 0) return g;
    g.sections.push_back(GridSection{0, fallback_bpm, 1});
  } else {
    for (std::size_t i = 0; i < grid_ms.size() && i < 64; ++i) {
      if (grid_ms[i].bpm <= 0) return g;
      g.sections.push_back(GridSection{grid_ms[i].first_beat_ms / 1000.0 * g.sr, grid_ms[i].bpm, grid_ms[i].bar_beat});
    }
  }
  g.base.assign(g.sections.size(), 0.0);
  for (std::size_t i = 1; i < g.sections.size(); ++i) {
    const double bf = 60.0 / g.sections[i - 1].bpm * g.sr;
    g.base[i] = g.base[i - 1] + std::ceil((g.sections[i].first_beat_frame - g.sections[i - 1].first_beat_frame) / bf - 1e-6);
  }
  g.ok = true;
  return g;
}

double BeatGrid::beat_frames_at(double frame) const {
  std::size_t i = 0;
  while (i + 1 < sections.size() && sections[i + 1].first_beat_frame <= frame) ++i;
  return 60.0 / sections[i].bpm * sr;
}

double BeatGrid::bpm_at_frame(double frame) const {
  std::size_t i = 0;
  while (i + 1 < sections.size() && sections[i + 1].first_beat_frame <= frame) ++i;
  return sections[i].bpm;
}

double BeatGrid::frame_to_beat(double p) const {
  std::size_t i = 0;
  while (i + 1 < sections.size() && sections[i + 1].first_beat_frame <= p) ++i;
  return base[i] + (p - sections[i].first_beat_frame) / (60.0 / sections[i].bpm * sr);
}

double BeatGrid::beat_to_frame(double b) const {
  std::size_t i = 0;
  while (i + 1 < sections.size() && base[i + 1] <= b) ++i;
  return sections[i].first_beat_frame + (b - base[i]) * (60.0 / sections[i].bpm * sr);
}

int BeatGrid::bar_beat(double b) const {
  std::size_t i = 0;
  while (i + 1 < sections.size() && base[i + 1] <= b) ++i;
  const long k = static_cast<long>(sections[i].bar_beat - 1) + std::lround(b - base[i]);
  return static_cast<int>(((k % 4) + 4) % 4) + 1;
}

double BeatGrid::bar_floor(double beat) const {
  double n = std::floor(beat + 1e-9);
  for (int k = 0; k < 4 && bar_beat(n) != 1; ++k) n -= 1;
  return n;
}

double BeatGrid::bar_ceil(double beat) const {
  double n = std::ceil(beat - 1e-9);
  for (int k = 0; k < 4 && bar_beat(n) != 1; ++k) n += 1;
  return n;
}

ResolvedTrack resolve_track(const TrackFacts& f, const AutomixPoints& entry, const AutomixSettings& st) {
  ResolvedTrack r;
  r.duration_ms = f.duration_ms;
  double last = f.cues.last_sound_ms.value_or(0);
  if (last <= 0 || (f.duration_ms > 0 && last > f.duration_ms)) last = f.duration_ms;  // not analysed: the file length
  r.last_sound_ms = last;
  const auto pick = [&](const std::optional<double>& a, const std::optional<double>& b) { return a ? a : b; };
  // Phrase analysis gives the automatic mix-in and mix-out when nothing was set by hand (the user's own points and main
  // cue always win; without phrases it is the load cue and "N bars before the last sound").
  std::optional<double> phrase_in, phrase_out;
  const bool want_in = !pick(pick(entry.mix_in_ms, f.points.mix_in_ms), f.cues.main_cue_ms);
  const bool want_out = !pick(entry.mix_out_ms, f.points.mix_out_ms);
  if (!f.phrases.sections.empty() && (want_in || want_out)) {
    const PhrasePoints pp = phrase_mix_points(f.phrases, f.cues.first_sound_ms.value_or(0), last, static_cast<int>(std::lround(st.mix_out_bars)));
    phrase_in = pp.mix_in_ms;
    phrase_out = pp.mix_out_ms;
  }
  r.mix_in_ms = pick(pick(entry.mix_in_ms, f.points.mix_in_ms),
                     pick(f.cues.main_cue_ms, pick(phrase_in, pick(f.cues.load_cue_ms, f.cues.first_sound_ms)))).value_or(0);
  if (f.duration_ms > 0) r.mix_in_ms = std::clamp(r.mix_in_ms, 0.0, std::max(0.0, f.duration_ms - 1000.0));
  double bpm = f.bpm;
  if (bpm <= 0 && !f.grid.empty()) bpm = f.grid.back().bpm;
  const double bar_ms = bpm > 0 ? 4 * 60000.0 / bpm : 2000.0;
  const std::optional<double> explicit_out = pick(entry.mix_out_ms, f.points.mix_out_ms);
  double out;
  if (explicit_out) {
    out = *explicit_out;
    r.mix_out_automatic = false;
  } else {
    out = phrase_out ? *phrase_out : last - st.mix_out_bars * bar_ms;
    const double playable = last - r.mix_in_ms;
    if (out < r.mix_in_ms + 0.35 * playable) out = r.mix_in_ms + 0.65 * playable;  // short track: do not give it away at once
  }
  if (const auto mp = pick(entry.max_play_ms, f.points.max_play_ms)) {
    if (r.mix_in_ms + *mp < out) {
      out = r.mix_in_ms + *mp;
      r.mix_out_limited_by_max_play = true;
    }
  }
  const double end = f.duration_ms > 0 ? f.duration_ms : std::max(out, last);
  out = std::min(out, end);
  out = std::max(out, std::min(r.mix_in_ms + 1000.0, end));
  r.mix_out_ms = out;
  r.beats = pick(entry.transition_beats, f.points.transition_beats);
  r.type = entry.transition_type ? *entry.transition_type : (f.points.transition_type ? *f.points.transition_type : std::string());
  if (!AutomixSettings::valid_type(r.type)) r.type.clear();
  return r;
}

}  // namespace automix

// ---- the engine room -------------------------------------------------------------------------------------------------

namespace {

using automix::BeatGrid;
using automix::ResolvedTrack;

constexpr double kLeadSeconds = 4.0;       // the transition is handed to the engine this long before it starts
constexpr double kSkipMarginSeconds = 0.8; // skip: the earliest bar line this far ahead
constexpr double kLateMarginSeconds = 1.5; // a transition that is committed late starts at least this far ahead
constexpr double kPlaySlewSeconds = 0.003; // the engine ramps a starting deck's speed with this time constant; the
                                           // incoming deck is cued that much ahead so its beat lands on time
constexpr double kLoadTimeoutSeconds = 90;

enum Tag { kFaderA, kFaderB, kEqLowA, kEqLowB, kTempoB, kPlayB, kStopA, kOther, kTagCount };

struct Step {
  int tag = kOther;
  double at = 0;  // offset from the start in plan units
  Command cmd;
  std::uint64_t id = 0;
  bool fired = false;
};

// A command that is not handed to the engine's scheduler but sent by tick() some time after the transition began. Used
// where the outgoing deck stops early, so its playhead can no longer trigger anything.
struct Deferred {
  int tag = kOther;
  std::uint64_t delay = 0;  // engine frames after the start
  Command cmd;
  bool done = false;
};

struct Plan {
  std::string type;  // crossfade | cut | fade | echo_out | fade_out
  bool synced = false, beat_timeline = false, has_b = true;
  int a = 0, b = 1;
  double n0 = 0, f0 = 0, length = 0, sr_a = 44100;
  double ratio0 = 1, old_range = 0.06, new_range = 0.06;
  double b_cue_frame = 0;  // where the incoming deck is parked
  double b_start_frame = 0;
  double beats = 0;        // transition length shown to the user
  std::string reason;
  std::vector<Command> pre;    // sent at once when the plan is committed
  std::vector<Step> steps;     // scheduled
  std::vector<Deferred> deferred;
  double a_stop_at = 0;        // offset at which the outgoing deck stops
  double b_play_at = 0;        // offset at which the incoming deck starts
  std::uint64_t t0_clock = 0;  // engine frame of offset 0 once it happened
  bool ramps = false;          // the incoming tempo is ramped
  double baseline[kTagCount] = {};
  bool committed = false, started = false, rescued = false, rescued_b = false;
  std::uint64_t commit_clock = 0;
};

struct PlanInput {
  const AutomixSettings* st = nullptr;
  int a = 0, b = 1;
  DeckView da, db;
  BeatGrid ga, gb;
  ResolvedTrack ra, rb;
  double sr_out = 44100;
  double earliest_frame_a = 0;  // the start may not be earlier
  bool has_b = true;
  bool immediate = false;       // skip: start at the first bar line after earliest_frame_a
};

Command make_cmd(Op op, int deck, float f = 0, int i = 0, double d = 0) {
  Command c;
  c.op = op;
  c.deck = static_cast<std::uint8_t>(deck);
  c.f = f;
  c.i = i;
  c.d = d;
  return c;
}

float fader_for_gain(double g) { return static_cast<float>(std::sqrt(std::clamp(g, 0.0, 1.0))); }  // the engine's gain is fader^2

double ms_to_frame(double ms, double sr) { return ms / 1000.0 * sr; }

// Builds the transition from deck a (playing) to deck b (loaded, cued). Nothing is sent here.
Plan build_plan(const PlanInput& in) {
  const AutomixSettings& st = *in.st;
  Plan p;
  p.a = in.a;
  p.b = in.b;
  p.has_b = in.has_b;
  p.sr_a = in.da.sample_rate;
  const double sr_a = p.sr_a;
  const double end_f = ms_to_frame(in.ra.mix_out_ms, sr_a);                        // where A is done
  const double frames_a = std::max(1.0, in.da.length);
  const double safe_end_f = std::max(0.0, frames_a - 0.25 * sr_a);
  p.old_range = in.db.tempo_range > 0 ? in.db.tempo_range : 0.06;
  p.new_range = p.old_range;

  // ---- which transition ----------------------------------------------------------------------------------------------
  std::string type = in.ra.type.empty() ? (in.rb.type.empty() ? st.transition_type : in.rb.type) : in.ra.type;
  if (!AutomixSettings::valid_type(type)) type = "crossfade";
  double beats = in.ra.beats ? *in.ra.beats : (in.rb.beats ? *in.rb.beats : st.transition_beats);
  beats = std::clamp(beats, 1.0, 64.0);
  if (!in.has_b) {
    type = "fade_out";
  } else if (!in.ga.ok || !in.gb.ok) {  // no tempo to follow on one side: a plain fade, timed in seconds
    p.reason = "no beat grid";
    type = "fade";
  }
  double bpm_a = 0, bpm_b = 0, mb_beat = 0;
  double mb_frame = ms_to_frame(in.rb.mix_in_ms, in.db.sample_rate > 0 ? in.db.sample_rate : 44100.0);
  if (in.has_b && in.gb.ok) {  // the incoming track starts on one of its beats
    mb_beat = std::round(in.gb.frame_to_beat(mb_frame));
    mb_frame = std::max(0.0, in.gb.beat_to_frame(mb_beat));
    bpm_b = in.gb.bpm_at_frame(mb_frame);
  }
  p.beat_timeline = in.ga.ok;
  if (in.ga.ok) bpm_a = in.ga.bpm_at_frame(end_f) * (1.0 + in.da.tempo);
  if (in.has_b && in.ga.ok && in.gb.ok && bpm_b > 0 && bpm_a > 0) {
    p.ratio0 = bpm_a / bpm_b;
    p.synced = std::fabs(p.ratio0 - 1.0) <= st.max_sync_gap_pct / 100.0;
    if (!p.synced) {
      p.reason = "tempo gap " + std::to_string(static_cast<int>(std::lround(std::fabs(p.ratio0 - 1.0) * 100))) + "%";
      if (type == "crossfade") type = AutomixSettings::valid_type(st.unsynced_type) && st.unsynced_type != "crossfade" ? st.unsynced_type : "cut";
      p.ratio0 = 1.0;
    }
  }
  if (type == "crossfade" && !p.synced) type = "fade";
  if (type == "echo_out" && (in.da.fx[kFxSlots - 1].kind != EffectKind::None)) type = "cut";  // the slot is in use
  p.type = type;

  // ---- when it starts ---------------------------------------------------------------------------------------------------
  double len = beats;  // plan units: beats (beat timeline) or seconds
  if (!in.ga.ok) len = st.fade_seconds;
  const bool short_form = type == "cut" || type == "echo_out";
  if (in.ga.ok) {
    const double bf_end = in.ga.beat_frames_at(end_f);
    double start_f = short_form ? end_f : end_f - len * bf_end;
    start_f = in.immediate ? in.earliest_frame_a : std::max(start_f, in.earliest_frame_a);
    const double start_beat = in.ga.frame_to_beat(start_f);
    double n0 = in.ga.bar_floor(start_beat);
    if (in.immediate) n0 = in.ga.bar_ceil(start_beat);
    else if (!in.ra.mix_out_limited_by_max_play) {  // the nearest bar line; a max play time is never overrun, so it rounds down
      const double up = in.ga.bar_ceil(start_beat);
      if (up - start_beat < start_beat - n0) n0 = up;
    }
    if (in.ga.beat_to_frame(n0) < in.earliest_frame_a - 1) n0 = in.ga.bar_ceil(in.ga.frame_to_beat(in.earliest_frame_a));
    p.n0 = n0;
    p.f0 = in.ga.beat_to_frame(n0);
    const double bf = in.ga.beat_frames_at(p.f0);
    const double room = std::floor((safe_end_f - p.f0) / bf);  // beats left before the end of A's file
    if (!short_form) {
      len = std::min(len, room);
      if (len < 2) {  // no room for a blend: cut on the bar
        type = "cut";
        p.type = type;
        len = std::min(beats, std::max(1.0, room));
      }
    } else {
      len = std::clamp(std::min(type == "echo_out" ? std::max(beats, 8.0) : beats, room), 1.0, 64.0);
    }
    if (type == "fade") len = std::max(len, 4.0);
  } else {
    double start_f = in.immediate ? in.earliest_frame_a : std::max(end_f - len * sr_a, in.earliest_frame_a);
    p.f0 = std::min(start_f, safe_end_f);
    len = std::min(len, std::max(1.0, (safe_end_f - p.f0) / sr_a));
  }
  p.length = len;
  p.beats = len;
  p.b_start_frame = mb_frame;

  // ---- the commands ---------------------------------------------------------------------------------------------------------
  const int a = in.a, b = in.b;
  const auto add = [&](int tag, double at, const Command& c) {
    Step s;
    s.tag = tag;
    s.at = at;
    s.cmd = c;
    p.steps.push_back(s);
  };
  const double out_beat_frames = in.ga.ok ? 60.0 / std::max(1.0, bpm_a) * in.sr_out : in.sr_out;  // output frames per beat of A
  const auto defer = [&](int tag, double at, const Command& c) {
    Deferred d;
    d.tag = tag;
    d.delay = static_cast<std::uint64_t>(std::max(0.0, at) * (p.beat_timeline ? out_beat_frames : in.sr_out));
    d.cmd = c;
    p.deferred.push_back(d);
  };
  const auto gain_steps = [&](double from, double to, bool down_a, bool up_b) {  // equal-power over [from, to]
    const double span = to - from;
    const int n = std::clamp(static_cast<int>(span * (p.beat_timeline ? 4 : 8)), 12, 128);
    for (int k = 1; k <= n; ++k) {
      const double t = static_cast<double>(k) / n, at = from + span * t;
      if (down_a) add(kFaderA, at, make_cmd(Op::Fader, a, fader_for_gain(std::cos(t * 1.5707963267948966))));
      if (up_b) add(kFaderB, at, make_cmd(Op::Fader, b, fader_for_gain(std::sin(t * 1.5707963267948966))));
    }
  };
  p.baseline[kFaderA] = in.da.fader;
  p.baseline[kFaderB] = 0.0;
  p.baseline[kEqLowA] = in.da.eq_low;
  p.baseline[kEqLowB] = 0.5;
  p.baseline[kTempoB] = 0.0;
  double ramp_from = 0;
  if (in.has_b) {
    p.pre.push_back(make_cmd(Op::Fader, b, 0.0f));
    p.pre.push_back(make_cmd(Op::EqLow, b, 0.5f));
    p.pre.push_back(make_cmd(Op::Crossfader, 0, 0.5f));
  }
  if (type == "crossfade") {
    gain_steps(0, p.length, true, true);
    if (st.bass_swap) {
      double mid = p.length >= 8 ? std::round(p.length / 2 / 4) * 4 : std::round(p.length / 2);
      mid = std::clamp(mid, 1.0, std::max(1.0, p.length - 1));
      p.pre.push_back(make_cmd(Op::EqLow, b, 0.0f));
      p.baseline[kEqLowB] = 0.0;
      add(kEqLowA, mid, make_cmd(Op::EqLow, a, 0.0f));
      add(kEqLowB, mid, make_cmd(Op::EqLow, b, 0.5f));
    }
    add(kPlayB, 0, make_cmd(Op::PlayToggle, b));
    ramp_from = 0;
  } else if (type == "cut") {
    add(kPlayB, 0, make_cmd(Op::PlayToggle, b));
    add(kFaderB, 0, make_cmd(Op::Fader, b, 1.0f));
    add(kFaderA, 0, make_cmd(Op::Fader, a, 0.0f));
    ramp_from = 0;
  } else if (type == "fade") {  // A fades out, then B starts and fades in
    const double half = p.beat_timeline ? std::max(2.0, std::ceil(p.length / 2)) : p.length / 2;
    gain_steps(0, half, true, false);
    add(kPlayB, half, make_cmd(Op::PlayToggle, b));
    const double rest = std::max(1.0, p.length - half);
    const int n = std::clamp(static_cast<int>(rest * (p.beat_timeline ? 4 : 8)), 12, 128);
    for (int k = 1; k <= n; ++k) {
      const double t = static_cast<double>(k) / n;
      add(kFaderB, half + rest * t, make_cmd(Op::Fader, b, fader_for_gain(t)));
    }
    p.length = std::max(p.length, half + rest);
    ramp_from = half;
  } else if (type == "echo_out") {
    const int slot = kFxSlots - 1;
    p.pre.push_back(make_cmd(Op::FxSelect, a, 0, slot, static_cast<double>(static_cast<int>(EffectKind::Echo))));
    p.pre.push_back(make_cmd(Op::FxWet, a, 0.65f, slot));
    p.pre.push_back(make_cmd(Op::FxTime, a, 1.0f, slot));
    add(kOther, -(p.beat_timeline ? 1.0 : 0.5), make_cmd(Op::FxEnable, a, 1.0f, slot));  // the last beat goes into the echo
    add(kPlayB, 0, make_cmd(Op::PlayToggle, b));
    add(kFaderB, 0, make_cmd(Op::Fader, b, 1.0f));
    add(kStopA, 0, make_cmd(Op::PlayToggle, a));  // A is cut dead; its echo rings out (A's playhead is still from now on)
    defer(kOther, std::max(1.0, p.length - 1), make_cmd(Op::FxEnable, a, 0.0f, slot));
    defer(kFaderA, p.length, make_cmd(Op::Fader, a, 0.0f));
    ramp_from = 0;
  } else {  // fade_out: nothing comes in
    gain_steps(0, p.length, true, false);
  }

  // ---- tempo: the incoming deck starts at the outgoing tempo and returns to its own ------------------------------------------
  if (in.has_b && p.synced && std::fabs(p.ratio0 - 1.0) > 0.0005) {
    const double need = std::fabs(p.ratio0 - 1.0);
    p.new_range = p.old_range;
    for (const double r : {0.06, 0.08, 0.10, 0.16, 0.25, 0.5, 1.0})
      if (p.new_range + 1e-9 < need * 1.02 && r > p.new_range) p.new_range = r;
    if (p.new_range < need * 1.02) p.new_range = std::min(1.0, need * 1.1);
    const double f0 = std::clamp((p.ratio0 - 1.0) / p.new_range, -1.0, 1.0);
    p.pre.push_back(make_cmd(Op::TempoRange, b, static_cast<float>(p.new_range)));
    p.pre.push_back(make_cmd(Op::Tempo, b, static_cast<float>(f0)));
    p.baseline[kTempoB] = f0;
    p.ramps = true;
    const double span = std::max(1.0, p.length - ramp_from);
    const int m = std::clamp(static_cast<int>(span * 2), 8, 64);
    for (int k = 1; k <= m; ++k) {
      const double t = static_cast<double>(k) / m;
      if (type == "echo_out") defer(kTempoB, ramp_from + span * t, make_cmd(Op::Tempo, b, static_cast<float>(f0 * (1.0 - t))));
      else add(kTempoB, ramp_from + span * t, make_cmd(Op::Tempo, b, static_cast<float>(f0 * (1.0 - t))));
    }
  } else if (in.has_b) {
    p.pre.push_back(make_cmd(Op::Tempo, b, 0.0f));
  }
  if (in.has_b) {
    // park the incoming deck a little ahead of its start: its speed ramps up after the play command, so it trails by this much
    const double target = p.synced ? p.ratio0 : 1.0;
    const double lag = kPlaySlewSeconds * in.sr_out * target * (in.db.sample_rate > 0 ? in.db.sample_rate / in.sr_out : 1.0);
    p.b_cue_frame = mb_frame + lag;
    p.pre.push_back(make_cmd(Op::Seek, b, 0, 0, p.b_cue_frame));
  }
  if (type != "echo_out") add(kStopA, p.length, make_cmd(Op::PlayToggle, a));  // last: A has been silent for a while
  p.a_stop_at = type == "echo_out" ? 0 : p.length;
  p.b_play_at = 0;
  for (const Step& s : p.steps)
    if (s.tag == kPlayB) p.b_play_at = s.at;
  std::stable_sort(p.steps.begin(), p.steps.end(), [](const Step& x, const Step& y) { return x.at < y.at; });
  return p;
}

}  // namespace

// ---- the service ----------------------------------------------------------------------------------------------------

struct AutomixService::Impl {
  AutomixHost& host;
  PlayQueue& q;
  Library* lib;
  mutable std::mutex m;
  std::mutex evm;
  std::vector<AutomixEvent> out;
  std::function<void(const AutomixEvent&)> listener;
  AutomixSettings st;

  bool running = false, paused = false, stop_after = false, skip_req = false, resume_hint = false;
  enum class Phase { Idle, Playing, Transitioning };
  Phase phase = Phase::Idle;
  std::string note;

  int cur = -1;
  QueueEntry cur_entry;
  TrackFacts cur_facts;
  ResolvedTrack cur_res;

  struct Prep {
    enum S { None, Loading, Ready } s = None;
    int deck = -1;
    QueueEntry entry;
    TrackFacts facts;
    ResolvedTrack res;
    std::uint64_t since = 0;
  } prep;
  Prep incoming;  // the entry whose transition is running

  Plan plan;
  bool have_plan = false;
  std::set<int> yielded;
  std::uint64_t grace_until = 0, last_plan_check = 0, last_endless = 0;
  double last_pos[kMaxDecks] = {};
  std::uint64_t last_clock = 0;
  bool have_last = false;
  double eta_s = -1;
  std::size_t endless_cursor = 0;
  std::mt19937 rng{std::random_device{}()};
  std::string last_state;

  Impl(AutomixHost& h, PlayQueue& queue, Library* l) : host(h), q(queue), lib(l) {}

  // ---- events ----------------------------------------------------------------------------------------------------------
  void emit(const std::string& type, std::map<std::string, std::string> text = {}, std::map<std::string, double> num = {}) {
    AutomixEvent e;
    e.type = type;
    e.text = std::move(text);
    e.num = std::move(num);
    std::lock_guard<std::mutex> lock(evm);
    out.push_back(std::move(e));
  }
  void flush() {
    std::vector<AutomixEvent> list;
    std::function<void(const AutomixEvent&)> cb;
    {
      std::lock_guard<std::mutex> lock(evm);
      list.swap(out);
    }
    {
      std::lock_guard<std::mutex> lock(m_listener());
      cb = listener;
    }
    if (cb)
      for (const AutomixEvent& e : list) cb(e);
  }
  std::mutex& m_listener() { return evm; }

  std::string state_name() const {
    if (!running) return "stopped";
    if (phase == Phase::Transitioning) return "transition";
    if (paused) return "paused";
    if (phase == Phase::Idle) return "idle";
    if (have_plan && plan.committed) return "armed";
    if (!q.peek() && !st.endless && prep.s == Prep::None) return "finishing";
    return "playing";
  }
  void state_changed() {
    const std::string s = state_name();
    if (s == last_state) return;
    last_state = s;
    emit("automix.state", {{"state", s}, {"note", note}});
  }

  IEngine& eng() { return host.engine(); }
  void send(const Command& c) { eng().send(c); }

  void note_problem(const std::string& s) {
    note = s;
    DH_LOG(Warn, "automix: %s", s.c_str());
  }

  // ---- plan bookkeeping --------------------------------------------------------------------------------------------------
  Schedule when(const Plan& p, double at) const {
    if (p.beat_timeline) return Schedule::at_deck_beat(p.a, p.n0 + at, false, true);
    return Schedule::at_deck_frame(p.a, p.f0 + at * p.sr_a, true);
  }

  void cancel_steps(Plan& p, int only_tag = -1) {
    for (Step& s : p.steps) {
      if (only_tag >= 0 && s.tag != only_tag) continue;
      if (s.id && !s.fired) eng().cancel(s.id);
      if (only_tag >= 0 || true) s.id = 0;
    }
  }

  bool commit_plan(Plan& p, std::uint64_t clock) {
    for (const Command& c : p.pre) send(c);
    for (Step& s : p.steps) {
      s.id = eng().schedule(s.cmd, when(p, s.at));
      if (s.id == 0) {
        cancel_steps(p);
        return false;
      }
    }
    p.committed = true;
    p.commit_clock = clock;
    grace_until = clock + static_cast<std::uint64_t>(0.35 * eng().sample_rate());
    return true;
  }

  // ---- loading the next entry ------------------------------------------------------------------------------------------------
  void fail_entry(const QueueEntry& e, const std::string& why) {
    note_problem("skipped queue entry " + std::to_string(e.id) + " (track " + std::to_string(e.track_id) + "): " + why);
    q.remove(e.id);
    emit("automix.entry_failed", {{"reason", why}}, {{"entry_id", static_cast<double>(e.id)}, {"track_id", static_cast<double>(e.track_id)}});
  }

  // Returns true when the next entry is loaded on `deck` and cued. Needs the queue's first entry; reloads when the queue changed.
  bool prepare(int deck, std::uint64_t clock) {
    if (prep.s == Prep::None) {
      const auto e = q.peek();
      if (!e) return false;
      const TrackFacts f = host.facts(e->track_id);
      if (!f.ok) {
        fail_entry(*e, "track not found");
        return false;
      }
      const DeckView held = eng().deck(deck);
      if (!(held.loaded && held.track_id == e->track_id) && !host.request_load(deck, e->track_id)) {
        fail_entry(*e, "cannot load");
        return false;
      }
      prep = Prep{};
      prep.s = Prep::Loading;
      prep.deck = deck;
      prep.entry = *e;
      prep.facts = f;
      prep.since = clock;
      return false;
    }
    const auto top = q.peek();
    if (!top || top->id != prep.entry.id || prep.deck != deck) {  // the queue changed under us
      prep = Prep{};
      return false;
    }
    prep.entry.overrides = top->overrides;
    if (prep.s == Prep::Loading) {
      const LoadState ls = host.load_state(deck, prep.entry.track_id);
      if (ls == LoadState::Failed || (ls == LoadState::Pending && clock > prep.since + static_cast<std::uint64_t>(kLoadTimeoutSeconds * eng().sample_rate()))) {
        const QueueEntry bad = prep.entry;
        prep = Prep{};
        fail_entry(bad, ls == LoadState::Failed ? "the file could not be loaded" : "loading took too long");
        return false;
      }
      if (ls == LoadState::Pending) return false;
      prep.facts = host.facts(prep.entry.track_id);  // analysis on load may have added cues and a grid
      prep.s = Prep::Ready;
    }
    const DeckView dv = eng().deck(deck);
    if (!dv.loaded || dv.track_id != prep.entry.track_id) {  // somebody loaded something else on it
      prep.s = Prep::Loading;
      prep.since = clock;
      host.request_load(deck, prep.entry.track_id);
      return false;
    }
    if (dv.length > 0 && dv.sample_rate > 0) prep.facts.duration_ms = dv.length / dv.sample_rate * 1000.0;  // the decoded length is exact
    prep.res = automix::resolve_track(prep.facts, prep.entry.overrides, st);
    return true;
  }

  // ---- starting, adopting, finishing ------------------------------------------------------------------------------------------------
  void start_first(int deck, std::uint64_t clock) {
    const auto e = q.begin_pending();
    if (!e || e->id != prep.entry.id) {
      if (e) q.abort_pending();
      prep = Prep{};
      return;
    }
    const DeckView dv = eng().deck(deck);
    const double sr = dv.sample_rate > 0 ? dv.sample_rate : 44100.0;
    send(make_cmd(Op::Crossfader, 0, 0.5f));
    send(make_cmd(Op::Fader, deck, 1.0f));
    send(make_cmd(Op::EqLow, deck, 0.5f));
    send(make_cmd(Op::Tempo, deck, 0.0f));
    send(make_cmd(Op::Seek, deck, 0, 0, ms_to_frame(prep.res.mix_in_ms, sr)));
    send(make_cmd(Op::PlayToggle, deck));
    q.commit_pending(host.now_ms());
    cur = deck;
    cur_entry = *e;
    cur_facts = prep.facts;
    cur_res = prep.res;
    prep = Prep{};
    phase = Phase::Playing;
    note.clear();
    have_last = false;
    (void)clock;
  }

  bool adopt_playing(const DeckView dv[]) {
    for (int d = 0; d < kMaxDecks; ++d) {
      if (!dv[d].loaded || !dv[d].playing || dv[d].fader <= 0.05f) continue;
      const auto e = q.adopt_current(dv[d].track_id, host.now_ms());
      if (!e) continue;
      cur = d;
      cur_entry = *e;
      cur_facts = host.facts(dv[d].track_id);
      if (dv[d].length > 0 && dv[d].sample_rate > 0) cur_facts.duration_ms = dv[d].length / dv[d].sample_rate * 1000.0;
      cur_res = automix::resolve_track(cur_facts, AutomixPoints{}, st);
      phase = Phase::Playing;
      prep = Prep{};
      have_last = false;
      return true;
    }
    return false;
  }

  void finish_all(const char* why) {
    q.clear_current();
    cur = -1;
    phase = Phase::Idle;
    running = false;
    paused = false;
    prep = Prep{};
    have_plan = false;
    yielded.clear();
    note = why;
    emit("automix.finished", {{"reason", why}});
    persist_hint(false);
  }

  void persist_hint(bool on) {
    if (!lib) return;
    try {
      lib->set_setting("automix.was_running", on ? "1" : "0");
    } catch (...) {
    }
  }

  // ---- endless mode ---------------------------------------------------------------------------------------------------------
  void fill_endless(std::uint64_t clock) {
    if (!st.endless || st.endless_source.empty()) return;
    const int have = static_cast<int>(q.size()) + (prep.s != Prep::None ? 0 : 0);
    if (have >= st.endless_keep) return;
    if (clock < last_endless + static_cast<std::uint64_t>(1.0 * eng().sample_rate()) && last_endless != 0) return;
    last_endless = clock;
    const std::vector<std::int64_t> src = host.source_tracks(st.endless_source);
    if (src.empty()) return;
    int want = st.endless_keep - have;
    for (std::size_t tries = 0; want > 0 && tries < src.size() * 2; ++tries) {
      std::int64_t pick;
      if (st.endless_order == "shuffle") {
        pick = src[rng() % src.size()];
      } else {
        pick = src[endless_cursor % src.size()];
        ++endless_cursor;
      }
      if (!q.add_end({pick}).entry_ids.empty()) --want;
    }
  }

  // ---- watching what the user does -------------------------------------------------------------------------------------------------
  double progress(const DeckView& a) const {  // plan units since the start, from A's playhead
    if (plan.beat_timeline && plan_grid.ok) return plan_grid.frame_to_beat(a.position) - plan.n0;
    return (a.position - plan.f0) / plan.sr_a;
  }
  BeatGrid plan_grid;  // the outgoing track's grid, kept while a plan is committed

  double value_at(int tag, double x) const {
    double v = plan.baseline[tag];
    for (const Step& s : plan.steps)
      if (s.tag == tag && s.at <= x + 1e-9) v = s.cmd.f;
    return v;
  }
  bool in_band(int tag, double observed, double p, double tol) const {
    const double d = plan.beat_timeline ? 0.5 : 0.25;
    double lo = value_at(tag, p - d), hi = lo;
    for (const Step& s : plan.steps) {
      if (s.tag != tag || s.at < p - d || s.at > p + d) continue;
      lo = std::min<double>(lo, s.cmd.f);
      hi = std::max<double>(hi, s.cmd.f);
    }
    return observed >= lo - tol && observed <= hi + tol;
  }

  void abort_plan(const std::string& why, const DeckView dv[], std::uint64_t clock) {
    note_problem("transition abandoned: " + why);
    cancel_steps(plan);
    const bool started = plan.started;
    const Plan p = plan;
    have_plan = false;
    plan = Plan{};
    yielded.clear();
    if (p.has_b) send(make_cmd(Op::EqLow, p.b, 0.5f));
    send(make_cmd(Op::EqLow, p.a, 0.5f));
    if (!started) {
      q.abort_pending();
      if (phase == Phase::Transitioning) phase = Phase::Playing;
      return;
    }
    // The incoming track already plays: whichever deck still plays carries on as the current one.
    int carry = -1;
    if (p.has_b && dv[p.b].playing) carry = p.b;
    else if (dv[p.a].playing) carry = p.a;
    if (carry < 0) {
      cur = -1;
      phase = Phase::Idle;
      paused = true;
      state_changed();
      return;
    }
    if (carry == p.b) {
      cur = p.b;
      cur_entry = incoming.entry;
      cur_facts = incoming.facts;
      cur_res = incoming.res;
    }
    incoming = Prep{};
    phase = Phase::Playing;
    have_last = false;
    (void)clock;
  }

  // Compares what the engine reports with what the plan says it should be.
  void check_user(const DeckView dv[], const MixerView& mx, std::uint64_t clock) {
    if (!have_plan || !plan.committed || st.override_mode == "hands_off" || clock < grace_until) return;
    const DeckView& A = dv[plan.a];
    const DeckView& B = dv[plan.b];
    const double p = progress(A);
    // transport: pause, jump, loop, jog
    const auto jumped = [&](const DeckView& d, int idx) {
      if (!have_last || clock <= last_clock) return false;
      const double dt = static_cast<double>(clock - last_clock);
      const double expect = d.speed * dt * (d.sample_rate / std::max(1.0, eng().sample_rate() * 1.0));
      return std::fabs((d.position - last_pos[idx]) - expect) > 0.30 * d.sample_rate + 0.2 * std::fabs(expect);
    };
    const double unit = plan.beat_timeline ? 1.0 : 0.5;  // a beat, or half a second
    const bool a_ending = p >= plan.a_stop_at - 0.25 * unit;
    bool transport = false;
    std::string why;
    if (!A.loaded || A.track_id != cur_entry.track_id) { transport = true; why = "the outgoing deck was unloaded"; }
    else if (!A.playing && !a_ending) { transport = true; why = "the outgoing deck was stopped"; }
    else if (A.loop_active || A.touching || A.reverse) { transport = true; why = "the outgoing deck was used"; }
    else if (A.playing && jumped(A, plan.a)) { transport = true; why = "the outgoing deck was moved"; }
    if (plan.has_b && !transport) {
      const std::int64_t want = plan.started ? incoming.entry.track_id : prep.entry.track_id;
      if (!B.loaded || B.track_id != want) { transport = true; why = "the incoming deck was changed"; }
      else if (!plan.started && B.playing && p < plan.b_play_at - 0.75 * unit) { transport = true; why = "the incoming deck was started by hand"; }
      else if (plan.started && !B.playing && p > plan.b_play_at + 0.75 * unit && B.position < B.length - 2) { transport = true; why = "the incoming deck was stopped"; }
      else if (B.loop_active || B.touching || B.reverse) { transport = true; why = "the incoming deck was used"; }
      else if (plan.started && B.playing && jumped(B, plan.b)) { transport = true; why = "the incoming deck was moved"; }
    }
    if (transport) {
      abort_plan(why, dv, clock);
      emit("automix.state", {{"state", "yielded"}, {"note", why}});
      return;
    }
    // controls
    const auto touch = [&](int tag, const char* what) {
      if (yielded.count(tag)) return;
      yielded.insert(tag);
      cancel_steps(plan, tag);
      note = std::string("you took over ") + what;
      emit("automix.override", {{"control", what}});
    };
    if (!yielded.count(kFaderA) && !in_band(kFaderA, A.fader, p, 0.07)) touch(kFaderA, "the outgoing fader");
    if (plan.has_b && !yielded.count(kFaderB) && !in_band(kFaderB, B.fader, p, 0.07)) touch(kFaderB, "the incoming fader");
    if (!yielded.count(kEqLowA) && !in_band(kEqLowA, A.eq_low, p, 0.07)) touch(kEqLowA, "the outgoing bass");
    if (plan.has_b && !yielded.count(kEqLowB) && !in_band(kEqLowB, B.eq_low, p, 0.07)) touch(kEqLowB, "the incoming bass");
    if (plan.has_b && plan.ramps && !yielded.count(kTempoB) && B.tempo_range > 0) {
      const double fader = B.tempo / B.tempo_range;
      if (!in_band(kTempoB, fader, p, 0.04)) touch(kTempoB, "the incoming tempo");
    }
    if (std::fabs(mx.crossfader - 0.5f) > 0.05f && !yielded.count(1000)) {  // the crossfader decides the blend now
      yielded.insert(1000);
      yielded.insert(kFaderA);
      yielded.insert(kFaderB);
      cancel_steps(plan, kFaderA);
      cancel_steps(plan, kFaderB);
      send(make_cmd(Op::Fader, plan.a, 1.0f));
      if (plan.has_b) send(make_cmd(Op::Fader, plan.b, 1.0f));
      note = "you took over the crossfader";
      emit("automix.override", {{"control", "crossfader"}});
    }
  }

  // ---- the state machine --------------------------------------------------------------------------------------------------------------
  void tick(std::uint64_t clock) {
    IEngine& e = eng();
    DeckView dv[kMaxDecks];
    for (int i = 0; i < kMaxDecks; ++i) dv[i] = e.deck(i);
    const MixerView mx = e.mixer();
    if (!paused) fill_endless(clock);

    switch (phase) {
      case Phase::Idle: step_idle(dv, clock); break;
      case Phase::Playing: step_playing(dv, mx, clock); break;
      case Phase::Transitioning: step_transition(dv, mx, clock); break;
    }
    for (int i = 0; i < kMaxDecks; ++i) last_pos[i] = dv[i].position;
    last_clock = clock;
    have_last = true;
    state_changed();
  }

  void step_idle(const DeckView dv[], std::uint64_t clock) {
    if (paused) return;
    if (adopt_playing(dv)) return;
    const int deck = 0;
    if (dv[deck].playing) return;
    if (!prepare(deck, clock)) return;
    start_first(deck, clock);
  }

  void begin_transition(std::uint64_t clock) {
    plan.started = true;
    plan.t0_clock = 0;
    if (plan.has_b) {
      incoming = prep;
      prep = Prep{};
      q.commit_pending(host.now_ms());
      emit("automix.transition_start",
           {{"type", plan.type}, {"from_track", std::to_string(cur_entry.track_id)}, {"to_track", std::to_string(incoming.entry.track_id)}},
           {{"from_deck", static_cast<double>(plan.a + 1)}, {"to_deck", static_cast<double>(plan.b + 1)}, {"beats", plan.beats}, {"synced", plan.synced ? 1.0 : 0.0}});
    }
    phase = Phase::Transitioning;
    eta_s = 0;
    (void)clock;
  }

  void step_playing(const DeckView dv[], const MixerView& mx, std::uint64_t clock) {
    const DeckView& A = dv[cur];
    if (!A.loaded || A.track_id != cur_entry.track_id) {  // the user emptied or reloaded the playing deck
      cancel_plan_quiet();
      cur = -1;
      phase = Phase::Idle;
      return;
    }
    const double rate = sr_of(A) * std::max(0.2, A.speed);  // track frames per second
    const bool ended = !A.playing && A.position >= A.length - 2;

    if (have_plan && plan.committed) {
      check_user(dv, mx, clock);
      if (!have_plan || !plan.committed) return;  // abandoned
      const double pr = progress(A);
      if (pr >= 0.02 || (plan.has_b && dv[plan.b].playing)) {
        begin_transition(clock);
        return;
      }
      if (ended) {
        step_ended(dv, clock);
        return;
      }
      if (skip_req) {  // plan again for the next bar
        cancel_steps(plan);
        q.abort_pending();
        have_plan = false;
        plan = Plan{};
        yielded.clear();
      } else {
        eta_s = (plan.f0 - A.position) / rate;
        return;
      }
    }
    if (ended) {
      step_ended(dv, clock);
      return;
    }
    if (!A.playing) return;  // paused by the user: wait
    if (paused) {
      eta_s = -1;
      return;
    }
    const int b = cur ^ 1;
    if (!q.peek() && prep.s == Prep::None) {
      step_empty(dv, clock);
      return;
    }
    if (!prepare(b, clock)) return;
    const double sr = std::max(1.0, e_rate());
    if (!skip_req && last_plan_check != 0 && clock < last_plan_check + static_cast<std::uint64_t>(0.1 * sr)) return;
    last_plan_check = clock;
    PlanInput in = make_input(dv, b, true);
    in.earliest_frame_a = A.position + (skip_req ? kSkipMarginSeconds : kLateMarginSeconds) * rate;
    in.immediate = skip_req;
    Plan p = build_plan(in);
    eta_s = (p.f0 - A.position) / rate;
    plan = p;  // for the status
    have_plan = true;
    if (!skip_req && A.position < p.f0 - kLeadSeconds * rate) return;
    plan_grid = in.ga;
    if (!q.begin_pending()) return;
    if (!commit_plan(plan, clock)) {
      q.abort_pending();
      have_plan = false;
      plan = Plan{};
      note_problem("the engine would not take the transition (schedule full)");
      return;
    }
    skip_req = false;
    yielded.clear();
    emit("automix.state", {{"state", "armed"}, {"type", plan.type}});
  }

  void cancel_plan_quiet() {
    if (have_plan && plan.committed) {
      cancel_steps(plan);
      if (!plan.started) q.abort_pending();
    }
    have_plan = false;
    plan = Plan{};
  }

  double e_rate() { return static_cast<double>(eng().sample_rate()); }
  static double sr_of(const DeckView& d) { return d.sample_rate > 0 ? d.sample_rate : 44100.0; }

  PlanInput make_input(const DeckView dv[], int b, bool has_b) {
    PlanInput in;
    in.st = &st;
    in.a = cur;
    in.b = b;
    in.has_b = has_b;
    in.da = dv[cur];
    in.db = dv[b];
    in.ga = BeatGrid::make(cur_facts.grid, cur_facts.bpm, sr_of(dv[cur]));
    in.gb = has_b ? BeatGrid::make(prep.facts.grid, prep.facts.bpm, sr_of(dv[b])) : BeatGrid{};
    in.ra = cur_res;
    if (has_b) in.rb = prep.res;
    in.sr_out = e_rate();
    return in;
  }

  // The queue is empty: the last track plays out, or fades at its mix-out.
  void step_empty(const DeckView dv[], std::uint64_t clock) {
    const DeckView& A = dv[cur];
    if (st.on_empty != "fade") {
      eta_s = -1;
      return;
    }
    if (have_plan && plan.committed) return;
    const double rate = sr_of(A) * std::max(0.2, A.speed);
    PlanInput in = make_input(dv, cur ^ 1, false);
    in.earliest_frame_a = A.position + kLateMarginSeconds * rate;
    Plan p = build_plan(in);
    eta_s = (p.f0 - A.position) / rate;
    if (A.position < p.f0 - kLeadSeconds * rate) return;
    plan = p;
    plan_grid = in.ga;
    have_plan = true;
    if (!commit_plan(plan, clock)) {
      have_plan = false;
      plan = Plan{};
      return;
    }
    yielded.clear();
  }

  // The playing track reached the end of its file on its own.
  void step_ended(const DeckView dv[], std::uint64_t clock) {
    const int b = cur ^ 1;
    cancel_plan_quiet();
    if (paused) return;
    if (prepare(b, clock)) {
      start_first(b, clock);  // no blend possible any more: the next one starts now
      return;
    }
    if (!q.peek() && prep.s == Prep::None && !st.endless) finish_all("the queue is empty");
    (void)dv;
  }

  void run_deferred(std::uint64_t clock) {
    if (!plan.started || plan.deferred.empty()) return;
    if (plan.t0_clock == 0) plan.t0_clock = clock;
    for (Deferred& d : plan.deferred) {
      if (d.done || clock < plan.t0_clock + d.delay) continue;
      if (!yielded.count(d.tag)) send(d.cmd);
      if (d.tag < kTagCount) plan.baseline[d.tag] = d.cmd.f;
      d.done = true;
    }
  }
  bool deferred_done() const {
    for (const Deferred& d : plan.deferred)
      if (!d.done) return false;
    return true;
  }

  void step_transition(const DeckView dv[], const MixerView& mx, std::uint64_t clock) {
    run_deferred(clock);
    check_user(dv, mx, clock);
    if (!have_plan) return;
    const DeckView& A = dv[plan.a];
    const DeckView& B = dv[plan.b];
    const double p = progress(A);
    const double unit = plan.beat_timeline ? 1.0 : 0.5;
    if (A.playing && p > plan.length + 2 * unit && !plan.rescued) {  // the stop step never came
      plan.rescued = true;
      send(make_cmd(Op::PlayToggle, plan.a));
    }
    if (plan.has_b && !B.playing && B.position < B.length - 2 && p > plan.b_play_at + 1.5 * unit && !plan.rescued_b) {
      plan.rescued_b = true;  // its start was dropped
      note_problem("the incoming track did not start on time; starting it now");
      send(make_cmd(Op::Fader, plan.b, 1.0f));
      send(make_cmd(Op::PlayToggle, plan.b));
    }
    if (!A.playing && deferred_done()) finalize(clock);
  }

  void finalize(std::uint64_t) {
    const Plan p = plan;
    send(make_cmd(Op::EqLow, p.a, 0.5f));
    if (p.type == "echo_out") send(make_cmd(Op::FxSelect, p.a, 0, kFxSlots - 1, 0.0));
    if (p.has_b) {
      send(make_cmd(Op::EqLow, p.b, 0.5f));
      if (!yielded.count(kTempoB)) {
        send(make_cmd(Op::Tempo, p.b, 0.0f));
        send(make_cmd(Op::TempoRange, p.b, static_cast<float>(p.old_range)));
      }
    }
    emit("automix.transition_end", {{"type", p.type}}, {{"from_deck", static_cast<double>(p.a + 1)}, {"to_deck", static_cast<double>(p.b + 1)}});
    have_plan = false;
    plan = Plan{};
    yielded.clear();
    if (p.has_b) {
      cur = p.b;
      cur_entry = incoming.entry;
      cur_facts = incoming.facts;
      cur_res = incoming.res;
      incoming = Prep{};
      phase = Phase::Playing;
      if (stop_after) {
        running = false;
        stop_after = false;
        persist_hint(false);
        q.clear_current();
      }
    } else {
      finish_all("the queue is empty");
    }
    have_last = false;
  }
};

AutomixService::AutomixService(AutomixHost& host, PlayQueue& queue, Library* lib) : impl_(new Impl(host, queue, lib)), queue_(queue) {
  if (lib) {
    try {
      impl_->st.load(*lib);
      impl_->resume_hint = lib->setting("automix.was_running", "0") == "1";
    } catch (const std::exception& e) {
      DH_LOG(Warn, "automix: could not read the settings: %s", e.what());
    }
  }
  Impl* im = impl_.get();
  queue.set_on_change([im] {
    im->emit("queue.changed", {}, {{"revision", static_cast<double>(im->q.revision())}, {"size", static_cast<double>(im->q.size())}});
  });
}

AutomixService::~AutomixService() {
  queue_.set_on_change(nullptr);
}

bool AutomixService::start(std::string& error) {
  std::unique_lock<std::mutex> lock(impl_->m);
  Impl& s = *impl_;
  if (s.running) {
    s.paused = false;
    return true;
  }
  s.running = true;
  s.paused = false;
  s.stop_after = false;
  s.skip_req = false;
  s.note.clear();
  s.resume_hint = false;
  s.phase = Impl::Phase::Idle;
  s.cur = -1;
  s.prep = Impl::Prep{};
  s.have_plan = false;
  s.persist_hint(true);
  s.state_changed();
  lock.unlock();
  s.flush();
  (void)error;
  return true;
}

void AutomixService::stop() {
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    Impl& s = *impl_;
    if (!s.running) return;
    if (s.phase == Impl::Phase::Transitioning) {
      s.stop_after = true;  // let the blend finish, then stop
    } else {
      s.cancel_plan_quiet();
      s.prep = Impl::Prep{};
      s.running = false;
      s.paused = false;
      s.persist_hint(false);
      s.q.clear_current();
    }
    s.state_changed();
  }
  impl_->flush();
}

void AutomixService::pause() {
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    Impl& s = *impl_;
    if (!s.running || s.paused) return;
    s.paused = true;
    if (s.have_plan && s.plan.committed && !s.plan.started) {  // not begun yet: take it back
      s.cancel_plan_quiet();
    }
    s.state_changed();
  }
  impl_->flush();
}

void AutomixService::resume() {
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    if (!impl_->running || !impl_->paused) return;
    impl_->paused = false;
    impl_->note.clear();
    impl_->state_changed();
  }
  impl_->flush();
}

bool AutomixService::skip() {
  std::lock_guard<std::mutex> lock(impl_->m);
  Impl& s = *impl_;
  if (!s.running) return false;
  if (s.phase == Impl::Phase::Transitioning) return false;
  s.paused = false;
  s.skip_req = true;
  return true;
}

void AutomixService::set_listener(std::function<void(const AutomixEvent&)> cb) {
  std::lock_guard<std::mutex> lock(impl_->evm);
  impl_->listener = std::move(cb);
}

AutomixSettings AutomixService::settings() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->st;
}

void AutomixService::set_settings(const AutomixSettings& v) {
  std::lock_guard<std::mutex> lock(impl_->m);
  impl_->st = v;
  if (impl_->lib) {
    try {
      impl_->st.save(*impl_->lib);
    } catch (const std::exception& e) {
      DH_LOG(Warn, "automix: could not save the settings: %s", e.what());
    }
  }
}

AutomixStatus AutomixService::status() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  const Impl& s = *impl_;
  AutomixStatus r;
  r.state = s.state_name();
  r.running = s.running;
  r.paused = s.paused;
  r.resume_available = !s.running && s.resume_hint;
  r.current_deck = s.cur;
  r.next_deck = s.cur >= 0 ? (s.cur ^ 1) : -1;
  r.current_track = s.cur >= 0 ? s.cur_entry.track_id : 0;
  r.current_entry = s.cur >= 0 ? s.cur_entry.id : 0;
  const auto next = s.q.peek();
  if (s.have_plan && s.plan.committed && s.phase == Impl::Phase::Playing && s.prep.entry.id) {
    r.next_track = s.prep.entry.track_id;
    r.next_entry = s.prep.entry.id;
  } else if (s.phase == Impl::Phase::Transitioning) {
    r.next_track = s.incoming.entry.track_id;
    r.next_entry = s.incoming.entry.id;
  } else if (next) {
    r.next_track = next->track_id;
    r.next_entry = next->id;
  }
  r.seconds_to_transition = s.eta_s;
  if (s.have_plan) {
    r.transition_type = s.plan.type;
    r.transition_beats = s.plan.beats;
    r.synced = s.plan.synced;
  }
  r.note = s.note;
  r.queue_revision = s.q.revision();
  r.planned_mix_out_ms = s.cur >= 0 ? static_cast<std::int64_t>(s.cur_res.mix_out_ms) : -1;
  return r;
}

void AutomixService::flush_events() { impl_->flush(); }

void AutomixService::tick() {
  Impl& s = *impl_;
  {
    std::lock_guard<std::mutex> lock(s.m);
    if (s.running) {
      try {
        s.tick(s.eng().clock_frames());
      } catch (const std::exception& e) {
        s.note_problem(std::string("internal error: ") + e.what());
      }
    }
  }
  s.flush();
}

void AutomixService::on_engine_event(const EngineEvent& e) {
  if (e.type != EventType::ScheduledFired && e.type != EventType::ScheduledDropped) return;
  std::lock_guard<std::mutex> lock(impl_->m);
  Impl& s = *impl_;
  if (!s.have_plan) return;
  for (Step& st : s.plan.steps) {
    if (st.id != e.sched_id || e.sched_id == 0) continue;
    if (e.type == EventType::ScheduledFired) {
      st.fired = true;
      if (st.tag == kPlayB && s.plan.type == "echo_out") s.plan.t0_clock = e.clock_frame;
    } else {
      st.id = 0;
      DH_LOG(Warn, "automix: a scheduled step was dropped (reason %d)", static_cast<int>(e.slot));
    }
    return;
  }
}

}  // namespace deckhand
