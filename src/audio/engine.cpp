#include "deckhand/audio/engine.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>

#include "deckhand/core/spsc.hpp"
#include "deckhand/log/log.hpp"
#include "miniaudio.h"

namespace deckhand {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kChunk = 512;
constexpr double kSecondsPerRev = 1.8;     // one platter revolution at 33 1/3 rpm
constexpr double kScratchTau = 0.012;      // seconds for the platter position to catch the hand
constexpr double kBendPerRev = 2.5;        // pitch-bend amount per revolution of accumulated motion
constexpr double kBendDecay = 0.08;        // seconds

struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1[2] = {0, 0}, z2[2] = {0, 0};

  inline float run(int ch, float x) {
    const float y = b0 * x + z1[ch];
    z1[ch] = b1 * x - a1 * y + z2[ch];
    z2[ch] = b2 * x - a2 * y;
    return y;
  }
  void set(double B0, double B1, double B2, double A0, double A1, double A2) {
    b0 = static_cast<float>(B0 / A0);
    b1 = static_cast<float>(B1 / A0);
    b2 = static_cast<float>(B2 / A0);
    a1 = static_cast<float>(A1 / A0);
    a2 = static_cast<float>(A2 / A0);
  }
  void identity() { b0 = 1, b1 = b2 = a1 = a2 = 0; }
};

// Standard audio-EQ biquad forms.
void low_shelf(Biquad& q, double fs, double f0, double db) {
  const double A = std::pow(10.0, db / 40.0), w = 2 * kPi * f0 / fs, c = std::cos(w);
  const double al = std::sin(w) / 2 * std::sqrt(2.0), s = 2 * std::sqrt(A) * al;
  q.set(A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
        (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
}
void high_shelf(Biquad& q, double fs, double f0, double db) {
  const double A = std::pow(10.0, db / 40.0), w = 2 * kPi * f0 / fs, c = std::cos(w);
  const double al = std::sin(w) / 2 * std::sqrt(2.0), s = 2 * std::sqrt(A) * al;
  q.set(A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
        (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
}
void peaking(Biquad& q, double fs, double f0, double Q, double db) {
  const double A = std::pow(10.0, db / 40.0), w = 2 * kPi * f0 / fs, c = std::cos(w), al = std::sin(w) / (2 * Q);
  q.set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}
void low_pass(Biquad& q, double fs, double f0, double Q) {
  const double w = 2 * kPi * f0 / fs, c = std::cos(w), al = std::sin(w) / (2 * Q);
  q.set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
void high_pass(Biquad& q, double fs, double f0, double Q) {
  const double w = 2 * kPi * f0 / fs, c = std::cos(w), al = std::sin(w) / (2 * Q);
  q.set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

// Knob position (0..1, centre = flat) to EQ gain in dB: -26 dB at minimum, +6 dB at maximum.
double eq_db(float k) { return k < 0.5f ? -26.0 * (1.0 - 2.0 * k) : 6.0 * (2.0 * k - 1.0); }
// Trim: silence at minimum, unity at centre, +9 dB at maximum.
float trim_gain(float k) {
  return k < 0.5f ? (2 * k) * (2 * k) : static_cast<float>(std::pow(10.0, (2.0 * k - 1.0) * 9.0 / 20.0));
}
float level_gain(float k) { return k * k * 1.5625f; }  // unity at 0.8

inline float soft_clip(float x) {
  const float a = std::fabs(x);
  if (a <= 0.7f) return x;
  const float y = 0.7f + 0.3f * std::tanh((a - 0.7f) / 0.3f);
  return x < 0 ? -y : y;
}

// 4-point Hermite interpolation of a stereo frame; silence outside the track.
inline void read_frame(const float* pcm, std::int64_t frames, double pos, float& l, float& r) {
  const std::int64_t i = static_cast<std::int64_t>(std::floor(pos));
  if (i < -1 || i >= frames) {
    l = r = 0;
    return;
  }
  const float t = static_cast<float>(pos - static_cast<double>(i));
  float out[2];
  for (int c = 0; c < 2; ++c) {
    const auto at = [&](std::int64_t k) { return (k < 0 || k >= frames) ? 0.0f : pcm[k * 2 + c]; };
    const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    out[c] = ((c3 * t + c2) * t + c1) * t + x0;
  }
  l = out[0];
  r = out[1];
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Single-writer snapshot the UI can read without blocking the audio thread.
template <class T>
class Snapshot {
 public:
  void write(const T& v) {
    seq_.fetch_add(1, std::memory_order_acq_rel);
    std::memcpy(&value_, &v, sizeof(T));
    seq_.fetch_add(1, std::memory_order_release);
  }
  T read() const {
    T out;
    for (;;) {
      const std::uint32_t a = seq_.load(std::memory_order_acquire);
      std::memcpy(&out, &value_, sizeof(T));
      const std::uint32_t b = seq_.load(std::memory_order_acquire);
      if (a == b && (a & 1u) == 0) return out;
    }
  }

 private:
  std::atomic<std::uint32_t> seq_{0};
  T value_{};
};

struct Deck {
  LoadedTrack* trk = nullptr;
  double pos = 0;
  bool playing = false;
  bool preview = false;       // playing only while a cue / hot cue button is held
  double preview_return = 0;
  int preview_slot = -1;      // -1 = main cue
  float tempo_fader = 0;      // -1..1
  float tempo_range = 0.06f;
  bool sync = false;
  bool touching = false;
  double scratch_target = 0;
  double speed = 0;
  double bend = 0;            // accumulated side-wheel motion, in revolutions
  double cue = 0;
  double hot[kHotCues];
  double hot_end[kHotCues];
  bool loop_active = false;
  double loop_in = -1, loop_out = -1;
  float amp = 0;
  double tempo_mult = 1;      // last effective tempo multiplier
  double bpm_now = 0;

  float trim = 0.5f, eq_high = 0.5f, eq_mid = 0.5f, eq_low = 0.5f, filter = 0.5f, fader = 1.0f;
  bool pfl = false;
  float s_trim = 0.5f, s_high = 0.5f, s_mid = 0.5f, s_low = 0.5f, s_filter = 0.5f;
  float c_high = -1, c_mid = -1, c_low = -1, c_filter = -1;  // values the coefficients were built for
  Biquad q_low, q_mid, q_high, q_filter;
  float g_trim_prev = 1, g_master_prev = 0, g_cue_prev = 0;
  float peak = 0;

  Deck() {
    for (int i = 0; i < kHotCues; ++i) hot[i] = hot_end[i] = -1;
  }
};

}  // namespace

struct Engine::Impl {
  ma_context ctx{};
  bool ctx_ok = false;
  ma_device dev{};
  bool dev_ok = false;
  std::string dev_name;

  std::uint32_t sr = 44100;
  int channels = 2;
  int master_ch = 0;
  int phones_ch = -1;

  SpscRing<Command, 1024> commands;
  std::mutex send_mutex;
  SpscRing<EngineEvent, 256> events;
  SpscRing<LoadedTrack*, 64> garbage;

  Deck decks[kMaxDecks];
  float crossfader = 0.5f, head_mix = 0.5f, head_level = 0.8f, master_level = 0.8f;
  float g_master_prev = 1, g_head_prev = 1, s_head_mix = 0.5f;
  float peak_l = 0, peak_r = 0;

  Snapshot<DeckView> deck_views[kMaxDecks];
  Snapshot<MixerView> mixer_view;

  // ---- helpers (audio thread) ----------------------------------------------------
  const GridSection* section_at(const Deck& d) const {
    if (!d.trk || d.trk->grid.empty()) return nullptr;
    const auto& g = d.trk->grid;
    auto it = std::upper_bound(g.begin(), g.end(), d.pos,
                               [](double p, const GridSection& s) { return p < s.first_beat_frame; });
    return it == g.begin() ? &g.front() : &*(it - 1);
  }
  double bpm_at(const Deck& d) const {
    if (const GridSection* s = section_at(d)) return s->bpm;
    return d.trk ? d.trk->bpm : 0;
  }
  // Frames per beat in the track's own sample rate (120 BPM when unknown).
  double beat_frames(const Deck& d) const {
    const double bpm = bpm_at(d);
    return 60.0 / (bpm > 0 ? bpm : 120.0) * (d.trk ? d.trk->audio->sample_rate : 44100.0);
  }
  double beat_phase(const Deck& d) const {
    const GridSection* s = section_at(d);
    if (!s || s->bpm <= 0) return -1;
    const double beats = (d.pos - s->first_beat_frame) / beat_frames(d);
    return beats - std::floor(beats);
  }
  double own_tempo(const Deck& d) const { return 1.0 + static_cast<double>(d.tempo_fader) * d.tempo_range; }

  // The deck a synced deck follows, or -1.
  int sync_master(int di) const {
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; ++pass) {
      for (int k = 0; k < kMaxDecks; ++k) {
        const int o = k == 0 ? (di ^ 1) : (k - 1 >= (di ^ 1) ? k : k - 1);
        if (o == di || o < 0 || o >= kMaxDecks) continue;
        const Deck& od = decks[o];
        if (!od.trk || bpm_at(od) <= 0) continue;
        if (od.sync && o > di) continue;          // two synced decks: the lower index leads
        if (pass == 0 && !od.playing) continue;   // prefer a playing deck
        best = o;
        break;
      }
    }
    return best;
  }
  double effective_tempo(const Deck& d, int di) const {
    const double mine = bpm_at(d);
    if (d.sync && mine > 0) {
      const int o = sync_master(di);
      if (o >= 0) return bpm_at(decks[o]) * own_tempo(decks[o]) / mine;
    }
    return own_tempo(d);
  }
  void phase_align(Deck& d, int di) {
    const int o = sync_master(di);
    if (o < 0 || !d.playing || !decks[o].playing) return;
    const double mine = beat_phase(d), theirs = beat_phase(decks[o]);
    if (mine < 0 || theirs < 0) return;
    double delta = theirs - mine;
    if (delta > 0.5) delta -= 1.0;
    if (delta < -0.5) delta += 1.0;
    d.pos = std::max(0.0, d.pos + delta * beat_frames(d));
  }
  void emit(EventType t, int di, int slot, double frame) {
    EngineEvent e;
    e.type = t;
    e.deck = static_cast<std::uint8_t>(di);
    e.slot = slot;
    e.frame = frame;
    e.track_id = decks[di].trk ? decks[di].trk->track_id : 0;
    events.push(e);
  }
  double clamp_pos(const Deck& d, double p) const {
    return std::clamp(p, 0.0, d.trk ? static_cast<double>(d.trk->audio->frames) : 0.0);
  }
  void end_preview(Deck& d) {
    if (!d.preview) return;
    d.playing = false;
    d.pos = d.preview_return;
    d.speed = 0;  // stop dead on the cue point instead of gliding past it
    d.preview = false;
    d.preview_slot = -1;
  }

  void apply(const Command& c) {
    if (c.op == Op::Crossfader) { crossfader = std::clamp(c.f, 0.0f, 1.0f); return; }
    if (c.op == Op::HeadMix) { head_mix = std::clamp(c.f, 0.0f, 1.0f); return; }
    if (c.op == Op::HeadLevel) { head_level = std::clamp(c.f, 0.0f, 1.0f); return; }
    if (c.op == Op::MasterLevel) { master_level = std::clamp(c.f, 0.0f, 1.0f); return; }
    if (c.deck >= kMaxDecks) {
      if (c.op == Op::Load && c.track) garbage.push(c.track);
      return;
    }
    const int di = c.deck;
    Deck& d = decks[di];
    const float k = std::clamp(c.f, 0.0f, 1.0f);

    switch (c.op) {
      case Op::Load: {
        if (d.trk) garbage.push(d.trk);  // freed later, off the audio thread
        d.trk = c.track;
        d.playing = d.preview = d.touching = d.loop_active = false;
        d.loop_in = d.loop_out = -1;
        d.speed = 0;
        d.bend = 0;
        d.amp = 0;
        if (d.trk) {
          d.cue = d.pos = clamp_pos(d, d.trk->cue_point);
          for (int i = 0; i < kHotCues; ++i) {
            d.hot[i] = d.trk->hot_cue[i];
            d.hot_end[i] = d.trk->hot_loop_end[i];
          }
        }
        return;
      }
      case Op::Trim: d.trim = k; return;
      case Op::EqHigh: d.eq_high = k; return;
      case Op::EqMid: d.eq_mid = k; return;
      case Op::EqLow: d.eq_low = k; return;
      case Op::Filter: d.filter = k; return;
      case Op::Fader: d.fader = k; return;
      case Op::Pfl: d.pfl = c.i < 0 ? !d.pfl : c.i != 0; return;
      case Op::Tempo: d.tempo_fader = std::clamp(c.f, -1.0f, 1.0f); return;
      case Op::TempoRange: d.tempo_range = std::clamp(c.f, 0.0f, 1.0f); return;
      case Op::SyncToggle:
        d.sync = !d.sync;
        if (d.sync && d.trk) phase_align(d, di);
        return;
      default: break;
    }
    if (!d.trk) return;  // everything below needs a track

    const double tol = d.trk->audio->sample_rate * 0.01;
    switch (c.op) {
      case Op::Eject:
        garbage.push(d.trk);
        d.trk = nullptr;
        d.playing = d.preview = d.loop_active = false;
        break;
      case Op::PlayToggle:
        if (d.preview) {
          d.preview = false;  // keep playing after the held button is released
          d.preview_slot = -1;
        } else if (d.playing) {
          d.playing = false;
        } else if (d.pos < static_cast<double>(d.trk->audio->frames) - 1) {
          d.playing = true;
          if (d.sync) phase_align(d, di);
        }
        break;
      case Op::CueDown:
        if (d.playing && !d.preview) {
          d.pos = d.cue;
          d.speed = 0;
          d.playing = false;
        } else if (!d.playing) {
          if (std::fabs(d.pos - d.cue) <= tol) {
            d.preview = true;
            d.preview_slot = -1;
            d.preview_return = d.cue;
            d.playing = true;
          } else {
            d.cue = d.pos;
            emit(EventType::CueSet, di, -1, d.cue);
          }
        }
        break;
      case Op::CueUp:
        if (d.preview && d.preview_slot == -1) end_preview(d);
        break;
      case Op::HotCueDown: {
        if (c.i < 0 || c.i >= kHotCues) break;
        if (d.hot[c.i] < 0) {
          d.hot[c.i] = d.pos;
          d.hot_end[c.i] = -1;
          emit(EventType::HotCueSet, di, c.i, d.pos);
          break;
        }
        if (!d.playing) {
          d.preview = true;
          d.preview_slot = c.i;
          d.preview_return = d.hot[c.i];
          d.playing = true;
        }
        d.pos = d.hot[c.i];
        if (d.hot_end[c.i] > d.hot[c.i]) {
          d.loop_in = d.hot[c.i];
          d.loop_out = d.hot_end[c.i];
          d.loop_active = true;
        }
        break;
      }
      case Op::HotCueUp:
        if (d.preview && d.preview_slot == c.i) end_preview(d);
        break;
      case Op::HotCueClear:
        if (c.i >= 0 && c.i < kHotCues && d.hot[c.i] >= 0) {
          d.hot[c.i] = d.hot_end[c.i] = -1;
          emit(EventType::HotCueCleared, di, c.i, 0);
        }
        break;
      case Op::Seek:
        d.pos = clamp_pos(d, c.d);
        if (d.touching) d.scratch_target = d.pos;
        break;
      case Op::JogTouch:
        d.touching = c.i != 0;
        if (d.touching) d.scratch_target = d.pos;
        break;
      case Op::JogTurn:
        if (c.i == 1 && d.touching) {
          d.scratch_target += c.d * kSecondsPerRev * d.trk->audio->sample_rate;
        } else {
          d.bend += c.d;
        }
        break;
      case Op::LoopIn:
        d.loop_in = d.pos;
        d.loop_out = -1;
        d.loop_active = false;
        break;
      case Op::LoopOut:
        if (d.loop_in >= 0 && d.pos > d.loop_in + tol) {
          d.loop_out = d.pos;
          d.loop_active = true;
          d.pos = d.loop_in;
        }
        break;
      case Op::LoopToggle:
      case Op::BeatLoop: {
        const double len = std::max(c.f, 1.0f / 32) * beat_frames(d);
        const bool same = d.loop_active && std::fabs((d.loop_out - d.loop_in) - len) < 2.0;
        if (d.loop_active && (c.op == Op::LoopToggle || same)) {
          d.loop_active = false;
          break;
        }
        if (!d.loop_active) d.loop_in = d.pos;
        d.loop_out = d.loop_in + len;
        d.loop_active = true;
        if (d.pos >= d.loop_out) d.pos = d.loop_in;
        break;
      }
      case Op::Reloop:
        if (d.loop_active) {
          d.loop_active = false;
        } else if (d.loop_in >= 0 && d.loop_out > d.loop_in) {
          d.loop_active = true;
          d.pos = d.loop_in;
        }
        break;
      case Op::LoopScale:
        if (d.loop_active && c.f > 0) {
          const double len = std::max((d.loop_out - d.loop_in) * c.f, beat_frames(d) / 32);
          d.loop_out = d.loop_in + len;
          if (d.pos >= d.loop_out) d.pos = d.loop_in + std::fmod(d.pos - d.loop_in, len);
        }
        break;
      case Op::BeatJump:
        d.pos = clamp_pos(d, d.pos + c.f * beat_frames(d));
        if (d.touching) d.scratch_target = d.pos;
        break;
      case Op::CueCall: {
        if (d.loop_active) {
          Command s;
          s.op = Op::LoopScale;
          s.deck = c.deck;
          s.f = c.i < 0 ? 0.5f : 2.0f;
          apply(s);
          break;
        }
        const auto& m = d.trk->memory_cues;
        if (m.empty()) break;
        double target = -1;
        if (c.i > 0) {
          for (double v : m)
            if (v > d.pos + tol) { target = v; break; }
        } else {
          for (auto it = m.rbegin(); it != m.rend(); ++it)
            if (*it < d.pos - tol) { target = *it; break; }
        }
        if (target < 0) break;
        d.playing = d.preview = false;
        d.speed = 0;
        d.cue = d.pos = target;
        break;
      }
      default: break;
    }
  }

  // Raw deck audio (after the play/scratch transport, before the mixer) into buf.
  void render_deck(Deck& d, int di, float* buf, int n) {
    const TrackAudio& a = *d.trk->audio;
    const double ratio = static_cast<double>(a.sample_rate) / sr;
    const std::int64_t total = static_cast<std::int64_t>(a.frames);
    const double block_dt = static_cast<double>(n) / sr;

    d.bpm_now = bpm_at(d);
    d.tempo_mult = effective_tempo(d, di);
    const double bend = std::clamp(d.bend * kBendPerRev, -0.6, 0.6);
    d.bend *= std::exp(-block_dt / kBendDecay);
    if (std::fabs(d.bend) < 1e-5) d.bend = 0;

    double target;
    if (d.touching) {
      target = std::clamp((d.scratch_target - d.pos) / (kScratchTau * a.sample_rate), -12.0, 12.0);
    } else if (d.playing) {
      target = d.tempo_mult * (1.0 + bend);
    } else {
      target = bend * 6.0;  // nudging a stopped deck searches slowly
    }
    const bool steady = d.playing && !d.touching;
    if (!steady && target == 0 && std::fabs(d.speed) < 1e-4 && d.amp < 1e-4f) {
      d.speed = 0;
      d.amp = 0;
      std::memset(buf, 0, sizeof(float) * 2 * static_cast<std::size_t>(n));
      return;
    }

    const double slew = 1.0 - std::exp(-1.0 / (0.003 * sr));
    const float amp_slew = static_cast<float>(1.0 - std::exp(-1.0 / (0.004 * sr)));
    for (int i = 0; i < n; ++i) {
      d.speed += (target - d.speed) * slew;
      const float amp_target = steady ? 1.0f : static_cast<float>(std::min(1.0, std::fabs(d.speed) * 25.0));
      d.amp += (amp_target - d.amp) * amp_slew;
      float l, r;
      read_frame(a.pcm.data(), total, d.pos, l, r);
      buf[i * 2] = l * d.amp;
      buf[i * 2 + 1] = r * d.amp;
      d.pos += d.speed * ratio;
      if (d.loop_active && d.speed > 0 && d.pos >= d.loop_out) {
        const double len = d.loop_out - d.loop_in;
        d.pos = len > 1 ? d.loop_in + std::fmod(d.pos - d.loop_out, len) : d.loop_in;
        if (d.touching) d.scratch_target -= len;
      }
      if (d.pos >= static_cast<double>(total)) {
        d.pos = static_cast<double>(total);
        if (d.playing && !d.touching) {
          d.playing = d.preview = false;
          emit(EventType::TrackEnded, di, 0, d.pos);
          target = 0;
        }
      } else if (d.pos < 0) {
        d.pos = 0;
      }
    }
  }

  // Trim, three-band EQ and the filter knob, in place. Also tracks the pre-fader level.
  void process_channel(Deck& d, float* buf, int n) {
    const float sm = 0.4f;
    d.s_trim += (d.trim - d.s_trim) * sm;
    d.s_high += (d.eq_high - d.s_high) * sm;
    d.s_mid += (d.eq_mid - d.s_mid) * sm;
    d.s_low += (d.eq_low - d.s_low) * sm;
    d.s_filter += (d.filter - d.s_filter) * sm;
    const double fs = sr;
    if (std::fabs(d.s_low - d.c_low) > 1e-4f) low_shelf(d.q_low, fs, 180.0, eq_db(d.c_low = d.s_low));
    if (std::fabs(d.s_mid - d.c_mid) > 1e-4f) peaking(d.q_mid, fs, 1000.0, 0.8, eq_db(d.c_mid = d.s_mid));
    if (std::fabs(d.s_high - d.c_high) > 1e-4f) high_shelf(d.q_high, fs, 5000.0, eq_db(d.c_high = d.s_high));
    if (std::fabs(d.s_filter - d.c_filter) > 1e-4f) {
      const float f = d.c_filter = d.s_filter;
      const double top = std::min(20000.0, fs * 0.45);
      if (f < 0.47f) {
        low_pass(d.q_filter, fs, top * std::pow(80.0 / top, (0.47 - f) / 0.47), 1.0);
      } else if (f > 0.53f) {
        high_pass(d.q_filter, fs, std::min(top, 20.0 * std::pow(8000.0 / 20.0, (f - 0.53) / 0.47)), 1.0);
      } else {
        d.q_filter.identity();
      }
    }
    const float g0 = d.g_trim_prev, g1 = trim_gain(d.s_trim);
    d.g_trim_prev = g1;
    float peak = 0;
    for (int i = 0; i < n; ++i) {
      const float g = g0 + (g1 - g0) * (static_cast<float>(i) / static_cast<float>(n));
      for (int ch = 0; ch < 2; ++ch) {
        float x = buf[i * 2 + ch] * g;
        x = d.q_low.run(ch, x);
        x = d.q_mid.run(ch, x);
        x = d.q_high.run(ch, x);
        x = d.q_filter.run(ch, x);
        buf[i * 2 + ch] = x;
        peak = std::max(peak, std::fabs(x));
      }
    }
    d.peak = std::max(peak, d.peak * static_cast<float>(std::exp(-static_cast<double>(n) / (0.3 * sr))));
  }

  void render_chunk(float* out, int n) {
    float master[kChunk * 2];
    float cue[kChunk * 2];
    float buf[kChunk * 2];
    std::memset(master, 0, sizeof(float) * 2 * static_cast<std::size_t>(n));
    std::memset(cue, 0, sizeof(float) * 2 * static_cast<std::size_t>(n));
    const float inv_n = 1.0f / static_cast<float>(n);

    for (int di = 0; di < kMaxDecks; ++di) {
      Deck& d = decks[di];
      if (!d.trk) {
        d.peak = 0;
        d.g_master_prev = 0;
        continue;
      }
      render_deck(d, di, buf, n);
      process_channel(d, buf, n);
      const float xf = (di % 2 == 0) ? std::min(1.0f, 2.0f * (1.0f - crossfader)) : std::min(1.0f, 2.0f * crossfader);
      const float gm1 = d.fader * d.fader * xf, gm0 = d.g_master_prev;
      const float gc1 = d.pfl ? 1.0f : 0.0f, gc0 = d.g_cue_prev;
      d.g_master_prev = gm1;
      d.g_cue_prev = gc1;
      for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) * inv_n;
        const float gm = gm0 + (gm1 - gm0) * t, gc = gc0 + (gc1 - gc0) * t;
        master[i * 2] += buf[i * 2] * gm;
        master[i * 2 + 1] += buf[i * 2 + 1] * gm;
        cue[i * 2] += buf[i * 2] * gc;
        cue[i * 2 + 1] += buf[i * 2 + 1] * gc;
      }
    }

    const float mg1 = level_gain(master_level), mg0 = g_master_prev;
    const float hg1 = level_gain(head_level), hg0 = g_head_prev;
    g_master_prev = mg1;
    g_head_prev = hg1;
    s_head_mix += (head_mix - s_head_mix) * 0.4f;
    const float mix = s_head_mix;
    float pl = 0, pr = 0;
    for (int i = 0; i < n; ++i) {
      const float t = static_cast<float>(i) * inv_n;
      const float mg = mg0 + (mg1 - mg0) * t, hg = hg0 + (hg1 - hg0) * t;
      const float ml = soft_clip(master[i * 2] * mg), mr = soft_clip(master[i * 2 + 1] * mg);
      float* frame = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(channels);
      frame[master_ch] = ml;
      frame[master_ch + 1] = mr;
      if (phones_ch >= 0) {
        frame[phones_ch] = soft_clip((cue[i * 2] * (1 - mix) + master[i * 2] * mix) * hg);
        frame[phones_ch + 1] = soft_clip((cue[i * 2 + 1] * (1 - mix) + master[i * 2 + 1] * mix) * hg);
      }
      pl = std::max(pl, std::fabs(ml));
      pr = std::max(pr, std::fabs(mr));
    }
    const float decay = static_cast<float>(std::exp(-static_cast<double>(n) / (0.3 * sr)));
    peak_l = std::max(pl, peak_l * decay);
    peak_r = std::max(pr, peak_r * decay);
  }

  void publish() {
    for (int di = 0; di < kMaxDecks; ++di) {
      const Deck& d = decks[di];
      DeckView v;
      v.loaded = d.trk != nullptr;
      v.playing = d.playing;
      v.sync = d.sync;
      v.pfl = d.pfl;
      v.loop_active = d.loop_active;
      v.touching = d.touching;
      v.trim = d.trim;
      v.eq_high = d.eq_high;
      v.eq_mid = d.eq_mid;
      v.eq_low = d.eq_low;
      v.filter = d.filter;
      v.fader = d.fader;
      v.tempo_range = d.tempo_range;
      v.level = d.peak;
      if (d.trk) {
        v.track_id = d.trk->track_id;
        v.position = d.pos;
        v.length = static_cast<double>(d.trk->audio->frames);
        v.sample_rate = d.trk->audio->sample_rate;
        v.speed = d.speed;
        v.tempo = d.tempo_mult - 1.0;
        v.bpm = d.bpm_now * d.tempo_mult;
        v.cue = d.cue;
        v.loop_in = d.loop_in;
        v.loop_out = d.loop_out;
        v.at_cue = !d.playing && std::fabs(d.pos - d.cue) <= v.sample_rate * 0.01;
        for (int i = 0; i < kHotCues; ++i) v.hot_cue[i] = d.hot[i];
      }
      deck_views[di].write(v);
    }
    MixerView m;
    m.crossfader = crossfader;
    m.head_mix = head_mix;
    m.head_level = head_level;
    m.master_level = master_level;
    m.peak_left = peak_l;
    m.peak_right = peak_r;
    mixer_view.write(m);
  }

  void render(float* out, std::uint32_t frames) {
    Command c;
    while (commands.pop(c)) apply(c);
    std::memset(out, 0, sizeof(float) * frames * static_cast<std::size_t>(channels));
    std::uint32_t done = 0;
    while (done < frames) {
      const int n = static_cast<int>(std::min<std::uint32_t>(kChunk, frames - done));
      render_chunk(out + static_cast<std::size_t>(done) * static_cast<std::size_t>(channels), n);
      done += static_cast<std::uint32_t>(n);
    }
    publish();
  }

  static void data_callback(ma_device* device, void* output, const void*, ma_uint32 frames) {
    static_cast<Impl*>(device->pUserData)->render(static_cast<float*>(output), frames);
  }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) { impl_->publish(); }

Engine::~Engine() {
  stop();
  Command c;
  while (impl_->commands.pop(c)) delete c.track;
  for (Deck& d : impl_->decks) delete d.trk;
  collect_garbage();
}

std::vector<std::string> Engine::output_devices() {
  std::vector<std::string> names;
  ma_context ctx;
  if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return names;
  ma_device_info* infos = nullptr;
  ma_uint32 count = 0;
  if (ma_context_get_devices(&ctx, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
    for (ma_uint32 i = 0; i < count; ++i) names.emplace_back(infos[i].name);
  }
  ma_context_uninit(&ctx);
  return names;
}

bool Engine::start(const OutputConfig& config, std::string& error) {
  stop();
  Impl& s = *impl_;
  if (ma_context_init(nullptr, 0, nullptr, &s.ctx) != MA_SUCCESS) {
    error = "could not initialise the audio system";
    return false;
  }
  s.ctx_ok = true;

  ma_device_info* infos = nullptr;
  ma_uint32 count = 0;
  ma_context_get_devices(&s.ctx, &infos, &count, nullptr, nullptr);
  const ma_device_id* id = nullptr;
  s.dev_name = "system default";
  const std::string hint = lower(config.device_hint);
  for (ma_uint32 i = 0; i < count; ++i) {
    if (!hint.empty() && lower(infos[i].name).find(hint) != std::string::npos) {
      id = &infos[i].id;
      s.dev_name = infos[i].name;
      break;
    }
    if (hint.empty() && infos[i].isDefault) s.dev_name = infos[i].name;
  }
  if (!hint.empty() && !id) {
    for (ma_uint32 i = 0; i < count; ++i)
      if (infos[i].isDefault) s.dev_name = infos[i].name;
  }

  ma_uint32 native_channels = 2;
  ma_device_info info;
  if (ma_context_get_device_info(&s.ctx, ma_device_type_playback, id, &info) == MA_SUCCESS) {
    for (ma_uint32 i = 0; i < info.nativeDataFormatCount; ++i)
      native_channels = std::max(native_channels, info.nativeDataFormats[i].channels);
  }

  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.pDeviceID = id;
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = native_channels >= 4 ? std::min<ma_uint32>(native_channels, 8) : 2;
  cfg.sampleRate = 0;  // use the device's own rate
  cfg.periodSizeInFrames = config.period_frames;
  cfg.performanceProfile = ma_performance_profile_low_latency;
  cfg.dataCallback = &Impl::data_callback;
  cfg.pUserData = &s;
  if (ma_device_init(&s.ctx, &cfg, &s.dev) != MA_SUCCESS) {
    error = "could not open audio device: " + s.dev_name;
    ma_context_uninit(&s.ctx);
    s.ctx_ok = false;
    return false;
  }
  s.dev_ok = true;
  s.sr = s.dev.sampleRate;
  s.channels = static_cast<int>(s.dev.playback.channels);
  s.master_ch = config.master_channel + 1 < s.channels ? config.master_channel : 0;
  s.phones_ch = (s.channels >= 4 && config.phones_channel >= 0 && config.phones_channel + 1 < s.channels &&
                 config.phones_channel != s.master_ch)
                    ? config.phones_channel
                    : -1;
  for (Deck& d : s.decks) d.c_low = d.c_mid = d.c_high = d.c_filter = -1;  // rebuild filters for this rate

  if (ma_device_start(&s.dev) != MA_SUCCESS) {
    error = "could not start audio device: " + s.dev_name;
    stop();
    return false;
  }
  DH_LOG(Info, "audio: %s, %d ch, %u Hz, period %u, headphones %s, direct %s", s.dev_name.c_str(), s.channels,
         s.sr, s.dev.playback.internalPeriodSizeInFrames, s.phones_ch >= 0 ? "on" : "off (stereo device)",
         s.dev.playback.converter.isPassthrough ? "yes" : "no (converted)");
  return true;
}

void Engine::stop() {
  Impl& s = *impl_;
  if (s.dev_ok) ma_device_uninit(&s.dev);
  if (s.ctx_ok) ma_context_uninit(&s.ctx);
  s.dev_ok = s.ctx_ok = false;
}

bool Engine::running() const { return impl_->dev_ok; }
std::string Engine::device_name() const { return impl_->dev_name; }
int Engine::output_channels() const { return impl_->channels; }
std::uint32_t Engine::sample_rate() const { return impl_->sr; }

void Engine::send(const Command& c) {
  std::lock_guard<std::mutex> lock(impl_->send_mutex);
  if (!impl_->commands.push(c)) {
    DH_LOG(Warn, "engine command queue full; dropped a command");
    delete c.track;
  }
}

void Engine::load(int deck, std::unique_ptr<LoadedTrack> track) {
  Command c;
  c.op = Op::Load;
  c.deck = static_cast<std::uint8_t>(deck);
  c.track = track.release();
  send(c);
}

DeckView Engine::deck(int index) const { return impl_->deck_views[std::clamp(index, 0, kMaxDecks - 1)].read(); }
MixerView Engine::mixer() const { return impl_->mixer_view.read(); }
bool Engine::poll(EngineEvent& out) { return impl_->events.pop(out); }

void Engine::collect_garbage() {
  LoadedTrack* t = nullptr;
  while (impl_->garbage.pop(t)) delete t;
}

void Engine::configure_offline(std::uint32_t sample_rate, int channels) {
  impl_->sr = sample_rate;
  impl_->channels = channels;
  impl_->master_ch = 0;
  impl_->phones_ch = channels >= 4 ? 2 : -1;
}

void Engine::render(float* out, std::uint32_t frames) { impl_->render(out, frames); }

}  // namespace deckhand
