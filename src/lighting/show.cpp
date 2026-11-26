#include "deckhand/lighting/show.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace deckhand::lighting {

namespace {
constexpr double kTwoPi = 6.283185307179586;
constexpr const char* kScenes[] = {"wash", "pulse", "chase", "colour_cycle", "build_ramp", "calm", "blackout"};
constexpr const char* kMoves[] = {"none", "circle", "tilt_sweep", "pan_fan"};

double frac(double x) { return x - std::floor(x); }
float f01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }
Rgb mix(Rgb a, Rgb b, float t) { return {lerpf(a.r, b.r, t), lerpf(a.g, b.g, t), lerpf(a.b, b.b, t)}; }

std::uint64_t splitmix(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

Rgb hsv(double h_deg, double s, double v) {
  double h = std::fmod(h_deg, 360.0);
  if (h < 0) h += 360.0;
  const double c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1)), m = v - c;
  double r = 0, g = 0, b = 0;
  const int sector = static_cast<int>(h / 60.0);
  switch (sector) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
  }
  return {static_cast<float>(r + m), static_cast<float>(g + m), static_cast<float>(b + m)};
}

bool is_hit(Section s, int energy) { return s == Section::Drop || (s == Section::Chorus && energy >= 8); }

Rgb cool(Rgb c) {  // breakdown colours: pulled toward blue and dimmed
  const Rgb blue{0.08f, 0.3f, 1.0f};
  Rgb o = mix(c, blue, 0.6f);
  return o;
}
}  // namespace

const char* scene_name(SceneId s) { return kScenes[static_cast<int>(s)]; }
bool scene_from_name(const std::string& n, SceneId* o) {
  for (int i = 0; i < 7; ++i) if (n == kScenes[i]) { *o = static_cast<SceneId>(i); return true; }
  return false;
}
const char* move_name(MoveId m) { return kMoves[static_cast<int>(m)]; }
bool move_from_name(const std::string& n, MoveId* o) {
  for (int i = 0; i < 4; ++i) if (n == kMoves[i]) { *o = static_cast<MoveId>(i); return true; }
  return false;
}
const char* palette_mode_name(PaletteMode m) { return m == PaletteMode::User ? "user" : m == PaletteMode::Track ? "track" : "key"; }

struct ShowEngine::Ctx {
  double beat_pos = 0, beat_phase = 0, bar_pos = 0, bpm = 120, elapsed_beats = 0, build_p = 0;
  int beat_in_bar = 0, bar_index = 0, counter = 0;
  float base = 0.7f, low = 0;
  bool levels = false;
};

void ShowEngine::configure(const ShowConfig& cfg, CompiledPatch patch) {
  patch_ = std::move(patch);
  const std::size_t n = patch_.fixtures.size();
  frames_.assign(patch_.universes.size(), UniverseFrame{});
  for (std::size_t i = 0; i < frames_.size(); ++i) frames_[i].universe = patch_.universes[i];
  for (auto* v : {&cur_, &from_, &mix_, &held_, &final_}) v->assign(n, Look{});
  sphase_.assign(n, 0.0f);
  has_strobe_.assign(n, 0);
  for (std::size_t i = 0; i < n; ++i)
    for (const auto& ch : patch_.profiles[static_cast<std::size_t>(patch_.fixtures[i].profile)].channels)
      if (ch.role == Role::Strobe) has_strobe_[i] = 1;
  set_config(cfg);
  have_music_ = false;
  applied_ = -3;
  counter_ = 0;
  fading_ = false;
  started_ = false;
  hold_ = hold_pending_ = false;
  sel_ = Selection{};
  last_eff_scene_ = SceneId::Blackout;
  free_beats_ = 0;
  st_ = ShowStatus{};
}

void ShowEngine::set_config(const ShowConfig& cfg) {
  cfg_ = cfg;
  cfg_.master = f01(cfg_.master);
  cfg_.strobe_max_hz = std::clamp(cfg_.strobe_max_hz, 0.0f, 25.0f);
  cfg_.refresh_hz = std::clamp(cfg_.refresh_hz, 1.0, 60.0);
  cfg_.latency_ms = std::clamp(cfg_.latency_ms, -200.0, 1000.0);
  cfg_.crossfade_s = std::clamp(cfg_.crossfade_s, 0.0, 8.0);
  pal_rgb_ = 0xffffffffu;
  pal_key_ = -2;
  update_palette();
}

void ShowEngine::set_master(float v) { cfg_.master = f01(v); }
void ShowEngine::set_strobe(bool e, float hz) { cfg_.strobe_enabled = e; cfg_.strobe_max_hz = std::clamp(hz, 0.0f, 25.0f); }
void ShowEngine::set_hold(bool on) {
  if (on && !hold_) hold_pending_ = true;
  hold_ = on;
  if (!on) hold_pending_ = false;
}

void ShowEngine::set_music(const MusicState& m) {
  const bool new_look = m.track_rgb != pal_rgb_ || m.key_index != pal_key_;
  music_ = m;
  have_music_ = true;
  if (new_look && cfg_.palette.mode != PaletteMode::User) update_palette();
  pal_rgb_ = m.track_rgb;
  pal_key_ = m.key_index;
}

void ShowEngine::update_palette() {
  static const Rgb kDefault[] = {{1.0f, 0.1f, 0.6f}, {0.1f, 0.8f, 1.0f}, {1.0f, 0.6f, 0.1f}, {0.5f, 0.2f, 1.0f}};
  pal_n_ = 0;
  auto from_hue = [&](double h, double s, double v) {
    const double off[4] = {0, 35, 180, -35};
    for (double o : off) pal_[static_cast<std::size_t>(pal_n_++)] = hsv(h + o, s, v);
  };
  if (cfg_.palette.mode == PaletteMode::Track && music_.track_rgb != 0) {
    const float r = static_cast<float>((music_.track_rgb >> 16) & 255) / 255.f, g = static_cast<float>((music_.track_rgb >> 8) & 255) / 255.f,
                b = static_cast<float>(music_.track_rgb & 255) / 255.f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    if (mx > 0.2f && d / mx > 0.15f) {
      double h = mx == r ? std::fmod((g - b) / d, 6.0) : mx == g ? (b - r) / d + 2.0 : (r - g) / d + 4.0;
      from_hue(h * 60.0, std::max<double>(d / mx, 0.7), 1.0);
    }
  }
  int num = 0;
  bool minor = false;
  if (pal_n_ == 0 && cfg_.palette.mode != PaletteMode::User && camelot_of_key(music_.key_index, &num, &minor))
    from_hue((num - 1) * 30.0, minor ? 1.0 : 0.8, minor ? 0.9 : 1.0);
  if (pal_n_ == 0) {
    if (cfg_.palette.colours.empty()) {
      for (const Rgb& c : kDefault) pal_[static_cast<std::size_t>(pal_n_++)] = c;
    } else {
      for (std::size_t i = 0; i < cfg_.palette.colours.size() && pal_n_ < 8; ++i) pal_[static_cast<std::size_t>(pal_n_++)] = cfg_.palette.colours[i];
    }
  }
}

ShowEngine::Selection ShowEngine::choose(Section s, int energy, int counter) const {
  const std::uint64_t h = splitmix(cfg_.seed ^ (static_cast<std::uint64_t>(counter) * 0x9e3779b97f4a7c15ull) ^ (static_cast<std::uint64_t>(s) << 56));
  const auto pick = [&](int shift, unsigned n) { return static_cast<unsigned>((h >> shift) % n); };
  Selection o;
  o.intensity = std::clamp(0.3f + 0.07f * static_cast<float>(energy), 0.3f, 1.0f);
  switch (s) {
    case Section::Intro:
      o.scene = energy >= 4 && pick(8, 2) == 0 ? SceneId::Pulse : SceneId::Wash;
      o.move = pick(16, 2) ? MoveId::TiltSweep : MoveId::None;
      break;
    case Section::Build:
      o.scene = SceneId::BuildRamp;
      o.move = pick(16, 2) ? MoveId::PanFan : MoveId::Circle;
      break;
    case Section::Drop:
    case Section::Chorus: {
      static const SceneId set[3] = {SceneId::Pulse, SceneId::Chase, SceneId::ColourCycle};
      o.scene = set[pick(8, 3)];
      o.move = pick(16, 2) ? MoveId::Circle : MoveId::PanFan;
      o.hit = is_hit(s, energy);
      break;
    }
    case Section::Verse:
      o.scene = pick(8, 2) ? SceneId::ColourCycle : SceneId::Pulse;
      o.move = pick(16, 2) ? MoveId::TiltSweep : MoveId::None;
      break;
    case Section::Breakdown:
    case Section::Bridge:
      o.scene = SceneId::Calm;
      o.move = MoveId::TiltSweep;
      break;
    case Section::Outro:
      o.scene = SceneId::Wash;
      o.move = MoveId::None;
      break;
    case Section::Unknown:
      o.scene = SceneId::Pulse;
      o.move = MoveId::None;
      o.intensity = 0.7f;
      break;
  }
  return o;
}

void ShowEngine::begin_fade(double dur) {
  if (dur <= 0) { fading_ = false; return; }
  from_ = mix_;
  fade_dur_ = dur;
  fade_t_ = 0;
  fading_ = true;
}

void ShowEngine::eval_scene(SceneId s, const Ctx& c, std::vector<Look>& out) const {
  const std::size_t n = patch_.fixtures.size();
  const float nf = static_cast<float>(std::max<std::size_t>(n, 1));
  const Rgb base_col = palette_colour(c.counter);
  const float lvl = c.levels ? 0.8f + 0.2f * c.low : 1.0f;
  for (std::size_t i = 0; i < n; ++i) {
    const CompiledFixture& f = patch_.fixtures[i];
    Look& L = out[i];
    L = Look{};
    const float u = f.u;
    switch (s) {
      case SceneId::Blackout: break;
      case SceneId::Wash:
        L.r = base_col.r; L.g = base_col.g; L.b = base_col.b;
        L.intensity = c.base * 0.8f;
        break;
      case SceneId::Pulse: {
        const double ph = frac(c.beat_phase + ((f.tags & kTagBack) ? 0.5 : 0.0));  // back fixtures hit on the off-beat
        const float env = static_cast<float>(std::pow(1.0 - ph, 3.0));
        const float accent = c.beat_in_bar == 0 && !(f.tags & kTagBack) ? 1.0f : 0.8f;
        L.r = base_col.r; L.g = base_col.g; L.b = base_col.b;
        L.intensity = f01(c.base * accent * lvl * (0.2f + 0.8f * env));
        break;
      }
      case SceneId::Chase: {
        const double head = frac(c.bar_pos);
        double d = std::fabs(static_cast<double>(u) - head);
        d = std::min(d, 1.0 - d);
        const float w = std::max(0.2f, 1.5f / nf);
        const float tail = static_cast<float>(std::max(0.0, 1.0 - d / w));
        L.r = base_col.r; L.g = base_col.g; L.b = base_col.b;
        L.intensity = f01(c.base * lvl * (0.1f + 0.9f * tail * tail));
        break;
      }
      case SceneId::ColourCycle: {
        const double t = c.bar_pos / 8.0 + u * 0.35 + c.counter;
        const double fl = std::floor(t);
        const Rgb a = palette_colour(static_cast<int>(fl)), b = palette_colour(static_cast<int>(fl) + 1);
        const Rgb col = mix(a, b, static_cast<float>(frac(t)));
        L.r = col.r; L.g = col.g; L.b = col.b;
        const float env = static_cast<float>(std::pow(1.0 - c.beat_phase, 2.0));
        L.intensity = f01(c.base * lvl * (0.55f + 0.45f * env));
        break;
      }
      case SceneId::BuildRamp: {
        const float p = f01(static_cast<float>(c.build_p));
        const double head = frac(c.elapsed_beats * (1.0 + 3.0 * p) / 4.0);
        double d = std::fabs(static_cast<double>(u) - head);
        d = std::min(d, 1.0 - d);
        const float w = std::max(0.25f, 1.5f / nf);
        const float tail = static_cast<float>(std::max(0.0, 1.0 - d / w));
        L.r = base_col.r; L.g = base_col.g; L.b = base_col.b;
        L.intensity = f01(c.base * (0.15f + 0.5f * p + (0.35f + 0.5f * p) * tail));
        if (p > 0.8f) L.strobe_hz = 2.0f + 18.0f * (p - 0.8f) / 0.2f;  // still subject to the hard cap
        break;
      }
      case SceneId::Calm: {
        const Rgb col = cool(base_col);
        const float sw = 0.5f + 0.5f * static_cast<float>(std::sin(kTwoPi * (c.bar_pos / 8.0 + u * 0.25)));
        L.r = col.r; L.g = col.g; L.b = col.b;
        L.intensity = f01(c.base * (0.18f + 0.3f * sw));
        break;
      }
    }
  }
}

void ShowEngine::eval_move(MoveId m, const Ctx& c, std::vector<Look>& out) const {
  const float amp = 0.12f + 0.2f * c.base;
  for (std::size_t i = 0; i < patch_.fixtures.size(); ++i) {
    const double u = patch_.fixtures[i].u;
    Look& L = out[i];
    switch (m) {
      case MoveId::None: break;
      case MoveId::Circle: {
        const double a = kTwoPi * (c.bar_pos / 2.0 + u * 0.5);
        L.pan = f01(0.5f + amp * static_cast<float>(std::cos(a)));
        L.tilt = f01(0.45f + 0.7f * amp * static_cast<float>(std::sin(a)));
        break;
      }
      case MoveId::TiltSweep:
        L.pan = f01(0.5f + static_cast<float>(u - 0.5) * 0.2f);
        L.tilt = f01(0.5f + 1.5f * amp * static_cast<float>(std::sin(kTwoPi * (c.bar_pos / 4.0) + u * 1.5707963)));
        break;
      case MoveId::PanFan: {
        const float swing = 0.5f + 0.5f * static_cast<float>(std::sin(kTwoPi * c.bar_pos / 4.0));
        L.pan = f01(0.5f + static_cast<float>(u - 0.5) * 0.8f * swing);
        L.tilt = f01(0.4f + 0.1f * static_cast<float>(std::sin(kTwoPi * c.bar_pos / 8.0)));
        break;
      }
    }
  }
}

void ShowEngine::render(double now) {
  const double dt = started_ ? std::clamp(now - last_now_, 0.0, 0.25) : 0.0;
  started_ = true;
  last_now_ = now;
  const MusicState& m = music_;
  const bool fresh = have_music_ && (now - m.stamp_s) <= cfg_.stale_s;
  const bool active = fresh && m.playing;
  const double ahead = std::clamp(now - m.stamp_s, 0.0, 5.0) + cfg_.latency_ms / 1000.0;

  Ctx c;
  double ttb = -1, elapsed = 0;
  bool known = false;
  int idx = -1, en = 5;
  Section sec = Section::Unknown;
  if (active) {
    c.bpm = m.bpm > 0 ? m.bpm : 120.0;
    if (m.bpm > 0) {
      c.beat_pos = m.beat_pos + ahead * c.bpm / 60.0;
    } else {
      free_beats_ += dt * c.bpm / 60.0;
      c.beat_pos = free_beats_;
    }
    idx = m.section_index;
    sec = m.section;
    en = m.energy;
    elapsed = m.section_elapsed_s + ahead;
    known = m.time_to_boundary_s >= 0;
    ttb = known ? m.time_to_boundary_s - ahead : -1;
    if (idx >= 0 && known && m.has_next) {
      const double lead = is_hit(m.next_section, m.next_energy) ? 0.0 : cfg_.crossfade_s * 0.5;
      // Once the next section was taken early, stay on it while the incoming states catch up (tempo jitter).
      if (ttb <= lead || (applied_ == idx + 1 && ttb <= lead + 0.3)) {
        ++idx;
        sec = m.next_section;
        en = m.next_energy;
        elapsed = std::max(0.0, -ttb);
        ttb = -1;
        known = false;
      }
    }
  }
  c.beat_pos += 1e-7;  // a beat that lands a rounding error early still counts as the beat
  c.beat_phase = frac(c.beat_pos);
  c.bar_pos = (c.beat_pos - m.downbeat_phase) / 4.0;
  c.bar_index = static_cast<int>(std::floor(c.bar_pos));
  c.beat_in_bar = (((static_cast<int>(std::floor(c.beat_pos)) - m.downbeat_phase) % 4) + 4) % 4;
  c.elapsed_beats = elapsed * c.bpm / 60.0;
  c.levels = m.levels_valid;
  c.low = m.low;

  bool faded_now = false;
  const int key = active ? idx : -3;
  if (key != applied_) {
    const bool boundary = active && applied_ >= 0 && idx == applied_ + 1;
    Selection ns;
    double dur = cfg_.stop_fade_s;
    if (active) {
      ++counter_;
      ns = idx >= 0 ? choose(sec, en, counter_) : choose(Section::Unknown, en, counter_);
      dur = boundary ? (ns.hit ? 0.0 : cfg_.crossfade_s) : 0.4;
    }
    begin_fade(dur);
    faded_now = true;
    sel_ = ns;
    applied_ = key;
  }
  Selection e = sel_;
  if (ovr_.has_scene) e.scene = ovr_.scene;
  if (ovr_.has_move) e.move = ovr_.move;
  else if (!cfg_.auto_movement) e.move = MoveId::None;
  if (ovr_.has_intensity) e.intensity = f01(ovr_.intensity);
  if (e.scene != last_eff_scene_ && !faded_now) begin_fade(0.3);
  last_eff_scene_ = e.scene;

  const double total = elapsed + std::max(ttb, 0.0);
  c.build_p = (active && ttb >= 0 && total > 0.1) ? elapsed / total : 0.3;
  c.counter = counter_;
  c.base = e.intensity;
  eval_scene(e.scene, c, cur_);
  if (e.move != MoveId::None && e.scene != SceneId::Blackout) eval_move(e.move, c, cur_);
  bool hit = false;
  if (e.hit && active && !ovr_.has_scene && c.elapsed_beats < 1.0) {  // blinder on drop entry
    hit = true;
    const float h = static_cast<float>(1.0 - c.elapsed_beats);
    for (auto& L : cur_) {
      L.r = lerpf(L.r, 1, h); L.g = lerpf(L.g, 1, h); L.b = lerpf(L.b, 1, h);
      L.intensity = std::max(L.intensity, h);
      if (c.elapsed_beats < 0.5) L.strobe_hz = std::max(L.strobe_hz, 8.0f);
    }
  }

  float fade_p = 1;
  if (fading_) {
    fade_t_ += dt;
    const float t = f01(static_cast<float>(fade_t_ / fade_dur_));
    fade_p = t;
    const float k = t * t * (3 - 2 * t);
    for (std::size_t i = 0; i < cur_.size(); ++i) {
      const Look &a = from_[i], &b = cur_[i];
      Look& o = mix_[i];
      o.r = lerpf(a.r, b.r, k); o.g = lerpf(a.g, b.g, k); o.b = lerpf(a.b, b.b, k);
      o.intensity = lerpf(a.intensity, b.intensity, k);
      o.strobe_hz = k >= 0.5f ? b.strobe_hz : 0.0f;
      o.pan = lerpf(a.pan, b.pan, k); o.tilt = lerpf(a.tilt, b.tilt, k);
    }
    if (t >= 1) fading_ = false;
  } else {
    mix_ = cur_;
  }

  final_ = mix_;
  if (ovr_.has_colour)
    for (auto& L : final_) { L.r = f01(ovr_.colour.r); L.g = f01(ovr_.colour.g); L.b = f01(ovr_.colour.b); }
  if (ovr_.has_strobe)
    for (auto& L : final_) L.strobe_hz = std::max(0.0f, ovr_.strobe_hz);
  if (hold_pending_) { held_ = final_; hold_pending_ = false; }
  if (hold_) {
    final_ = held_;
    for (auto& L : final_) L.strobe_hz = 0;
  }

  // ---- safety: strobe cap, master, blackout (last stage, nothing bypasses it) ----
  const float cap = std::min(cfg_.strobe_max_hz, static_cast<float>(cfg_.refresh_hz / 4.0));
  float top = 0;
  for (std::size_t i = 0; i < final_.size(); ++i) {
    Look& L = final_[i];
    float hz = cfg_.strobe_enabled ? std::min(L.strobe_hz, cap) : 0.0f;
    if (hz < 0.01f) hz = 0;
    L.intensity = blackout_ ? 0.0f : f01(L.intensity) * cfg_.master;
    if (blackout_) hz = 0;
    L.strobe_hz = hz;
    top = std::max(top, hz);
    if (hz > 0 && !has_strobe_[i]) {  // fixtures without a strobe channel flash by gating their output
      sphase_[i] += hz * static_cast<float>(dt);
      sphase_[i] -= std::floor(sphase_[i]);
      if (sphase_[i] >= 0.5f) L.intensity = 0;
    }
  }

  std::uint8_t tmp[kDmxSlots];
  for (std::size_t i = 0; i < final_.size(); ++i) {
    const CompiledFixture& f = patch_.fixtures[i];
    const Profile& p = patch_.profiles[static_cast<std::size_t>(f.profile)];
    map_look(p, final_[i], tmp);
    std::memcpy(frames_[static_cast<std::size_t>(f.universe_slot)].data.data() + f.address0, tmp, static_cast<std::size_t>(p.channel_count()));
  }

  st_.scene = e.scene;
  st_.move = e.move;
  st_.section = active ? sec : Section::Unknown;
  st_.section_index = active ? idx : -1;
  st_.phrase_counter = counter_;
  st_.crossfade = fade_p;
  st_.active = active;
  st_.blackout = blackout_;
  st_.hold = hold_;
  st_.override_active = ovr_.any();
  st_.hit = hit;
  st_.strobe_hz = top;
  st_.master = cfg_.master;
  st_.beat_pos = c.beat_pos;
}

}  // namespace deckhand::lighting
