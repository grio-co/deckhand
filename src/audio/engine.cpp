#include "deckhand/audio/engine.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

#include <chrono>
#include <thread>

#include "deckhand/core/spsc.hpp"
#include "deckhand/log/log.hpp"
#include "dsp.hpp"
#include "fx.hpp"
#include "miniaudio.h"
#include "recorder.hpp"
#include "stretch.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

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

// On iOS the shell owns the audio session (category, activation, routing); the engine must
// leave it alone, or the system's default play-and-record session is forced on the app.
ma_result init_context(ma_context* ctx) {
  ma_context_config config = ma_context_config_init();
#if defined(__APPLE__) && TARGET_OS_IPHONE
  config.coreaudio.sessionCategory = ma_ios_session_category_none;
  config.coreaudio.noAudioSessionActivate = MA_TRUE;
  config.coreaudio.noAudioSessionDeactivate = MA_TRUE;
#endif
  return ma_context_init(nullptr, 0, &config, ctx);
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

  // key lock, quantise, slip, reverse, auto-gain, colour effect
  bool keylock = false, quantize = false, slip = false, slip_on = false, reverse = false;
  float key_shift = 0;
  double slip_pos = 0;
  int hold_slot = -1;         // hot cue held while playing (slip excursion)
  float autogain_db = 0, s_auto = 1.0f;
  int color = 0;
  double pos_prev = 0;        // play position at the end of the previous block (jump detection)
  double xf_pos = 0;          // old read position while crossfading across a jump
  int xf_left = 0;
  float kl_w = 0;             // 0..1 blend between varispeed and the time-stretcher
  bool st_live = false;
  bool kl_hot = false;        // key lock engaged (hysteresis)
  Stretcher st;

  Deck() {
    for (int i = 0; i < kHotCues; ++i) hot[i] = hot_end[i] = -1;
  }
};

constexpr int kSchedCap = 1024;    // live scheduled entries owned by the audio thread
constexpr int kSchedPool = 2048;   // payloads in flight between schedule() and the audio thread
constexpr std::uint64_t kLiveMask = 4095;

// Handed from schedule() to the audio thread through a preallocated pool slot.
struct SchedPayload {
  Command cmd;
  Schedule when;
  std::uint64_t id = 0;
};

struct SchedEntry {
  Command cmd;
  std::uint64_t id = 0;
  std::uint64_t at = 0;      // clock frame (clock entries)
  double frame = 0;          // track frame (deck entries)
  std::uint8_t deck = 0;     // trigger deck (deck entries)
  bool is_deck = false, armed = false, fire_if_passed = false, flagged = false, alive = false;
};

struct ClockSnap {
  double offset = 0;         // steady-clock seconds at which frame 0 would have been rendered
  std::uint32_t valid = 0;
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
  SpscRing<EngineEvent, 2048> events;
  SpscRing<LoadedTrack*, 64> garbage;

  Deck decks[kMaxDecks];
  float crossfader = 0.5f, head_mix = 0.5f, head_level = 0.8f, master_level = 0.8f;
  float g_master_prev = 1, g_head_prev = 1, s_head_mix = 0.5f;
  float peak_l = 0, peak_r = 0;

  Snapshot<DeckView> deck_views[kMaxDecks];
  Snapshot<MixerView> mixer_view;

  // effects, sampler, taps
  FxBus fxb[kMaxDecks];
  FxBus master_fx;
  double master_bpm_manual = 0, master_bpm = 0;
  double master_clock = 0;

  struct SamplerSlot {
    LoadedSample* s = nullptr;
    bool playing = false, loop = false, sync = false;
    double pos = 0;
    float env = 0, tgt = 0, vel = 1, gain = 0.8f;
    double tail_pos = 0;
    int tail_left = 0;
    float tail_gain = 0;
  };
  SamplerSlot sampler_slots[kSamplerSlots];
  float sampler_level = 0.8f;
  SpscRing<LoadedSample*, 64> sample_garbage;
  Snapshot<SamplerView> sampler_views[kSamplerSlots];

  Recorder rec;
  bool rec_pre = false;
  FrameRing stream;
  std::atomic<bool> stream_on{false};
  bool stream_pre = false;
  std::atomic<std::uint64_t> stream_dropped{0};

  // ---- engine clock and scheduled commands ---------------------------------------
  std::uint64_t clock = 0;                       // audio thread: frames rendered before the current chunk
  std::atomic<std::uint64_t> clock_pub{0};
  bool timing_on = false;                        // device mode: record callback timing
  double off_est = 0;
  bool off_init = false;
  Snapshot<ClockSnap> clock_snap;

  std::vector<SchedPayload> sched_pool = std::vector<SchedPayload>(kSchedPool);
  std::vector<std::uint32_t> sched_free_slots;   // non-real-time side (guarded by send_mutex)
  SpscRing<std::uint32_t, 4096> sched_free;      // audio -> non-real-time: slots to reuse
  std::uint64_t sched_next_id = 0;
  std::atomic<std::uint64_t> sched_live[kLiveMask + 1];
  std::atomic<std::int64_t> sched_pending{0};

  SchedEntry slab[kSchedCap];
  std::uint16_t slab_free[kSchedCap];
  int slab_free_n = 0;
  std::uint16_t clock_order[kSchedCap];          // slab indices sorted by (at, id)
  int n_clock = 0;
  std::uint16_t deck_list[kSchedCap];
  int n_deck = 0;
  int deck_cnt[kMaxDecks] = {};
  int n_flagged = 0;

  Impl() {
    sched_free_slots.reserve(kSchedPool);
    for (int i = kSchedPool - 1; i >= 0; --i) sched_free_slots.push_back(static_cast<std::uint32_t>(i));
    for (int i = 0; i < kSchedCap; ++i) slab_free[slab_free_n++] = static_cast<std::uint16_t>(kSchedCap - 1 - i);
    for (auto& l : sched_live) l.store(0, std::memory_order_relaxed);
  }

  void prepare_audio() {
    for (Deck& d : decks) d.st.prepare(sr);
    for (FxBus& b : fxb) b.prepare(sr);
    master_fx.prepare(sr);
    std::size_t cap = 1;
    while (cap < static_cast<std::size_t>(sr) * 3) cap <<= 1;
    if (stream.capacity() != cap) stream.init(cap);
    for (Deck& d : decks) d.c_low = d.c_mid = d.c_high = d.c_filter = -1;
  }

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
  const GridSection* section_for(const Deck& d, double p) const {
    if (!d.trk || d.trk->grid.empty()) return nullptr;
    const auto& g = d.trk->grid;
    auto it = std::upper_bound(g.begin(), g.end(), p, [](double x, const GridSection& s) { return x < s.first_beat_frame; });
    return it == g.begin() ? &g.front() : &*(it - 1);
  }
  // Nearest beat of the loaded grid (the position itself without a grid).
  double snap_beat(const Deck& d, double p) const {
    const GridSection* s = section_for(d, p);
    if (!s || s->bpm <= 0) return p;
    const double bf = 60.0 / s->bpm * d.trk->audio->sample_rate;
    double q = s->first_beat_frame + std::round((p - s->first_beat_frame) / bf) * bf;
    const auto& g = d.trk->grid;
    for (const GridSection& n : g)
      if (n.first_beat_frame > s->first_beat_frame && q > n.first_beat_frame) { q = n.first_beat_frame; break; }
    return std::clamp(q, 0.0, static_cast<double>(d.trk->audio->frames));
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

  void apply_core(const Command& c) {
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
        d.slip_on = d.reverse = false;
        d.hold_slot = -1;
        d.xf_left = 0;
        d.kl_w = 0;
        d.st_live = false;
        if (d.trk) {
          d.cue = d.pos = clamp_pos(d, d.trk->cue_point);
          d.pos_prev = d.pos;
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
          const double at = d.quantize ? snap_beat(d, d.pos) : d.pos;
          d.hot[c.i] = at;
          d.hot_end[c.i] = -1;
          emit(EventType::HotCueSet, di, c.i, at);
          break;
        }
        const bool was_playing = d.playing;
        if (!d.playing) {
          d.preview = true;
          d.preview_slot = c.i;
          d.preview_return = d.hot[c.i];
          d.playing = true;
        }
        d.pos = d.quantize ? snap_beat(d, d.hot[c.i]) : d.hot[c.i];
        if (d.slip && was_playing) d.hold_slot = c.i;
        if (d.hot_end[c.i] > d.hot[c.i]) {
          d.loop_in = d.hot[c.i];
          d.loop_out = d.hot_end[c.i];
          d.loop_active = true;
        }
        break;
      }
      case Op::HotCueUp:
        if (d.preview && d.preview_slot == c.i) end_preview(d);
        if (d.hold_slot == c.i) d.hold_slot = -1;
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
        d.loop_in = d.quantize ? snap_beat(d, d.pos) : d.pos;
        d.loop_out = -1;
        d.loop_active = false;
        break;
      case Op::LoopOut: {
        const double at = d.quantize ? snap_beat(d, d.pos) : d.pos;
        if (d.loop_in >= 0 && at > d.loop_in + tol) {
          d.loop_out = at;
          d.loop_active = true;
          d.pos = d.loop_in;
        }
        break;
      }
      case Op::LoopToggle:
      case Op::BeatLoop: {
        const double len = std::clamp(c.f, 1.0f / 32, 32.0f) * beat_frames(d);
        const bool same = d.loop_active && std::fabs((d.loop_out - d.loop_in) - len) < 2.0;
        if (d.loop_active && (c.op == Op::LoopToggle || same)) {
          d.loop_active = false;
          break;
        }
        if (!d.loop_active) d.loop_in = d.quantize ? snap_beat(d, d.pos) : d.pos;
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
        d.pos = clamp_pos(d, (d.quantize ? snap_beat(d, d.pos) : d.pos) + c.f * beat_frames(d));
        if (d.touching) d.scratch_target = d.pos;
        break;
      case Op::CueCall: {
        if (d.loop_active) {
          Command s;
          s.op = Op::LoopScale;
          s.deck = c.deck;
          s.f = c.i < 0 ? 0.5f : 2.0f;
          apply_core(s);
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

  // ---- appended commands: key lock, quantise, slip, reverse, effects, sampler, recording ----------
  static bool flag(bool cur, int i) { return i < 0 ? !cur : i != 0; }
  bool exc_active(const Deck& d) const { return d.touching || d.loop_active || d.reverse || d.hold_slot >= 0; }
  FxUnit* fx_unit(int target, int slot) {
    if (slot < 0 || slot >= kFxSlots) return nullptr;
    if (target == kFxMaster) return &master_fx.slot[slot];
    if (target >= 0 && target < kMaxDecks) return &fxb[target].slot[slot];
    return nullptr;
  }
  static EffectKind color_kind(int c) {
    switch (c) {
      case 1: return EffectKind::ColorNoise;
      case 2: return EffectKind::ColorDubEcho;
      case 3: return EffectKind::ColorCrush;
      case 4: return EffectKind::ColorPitch;
      case 5: return EffectKind::ColorSpace;
      default: return EffectKind::None;
    }
  }

  bool apply_ext(const Command& c) {
    if (c.op < Op::KeyLock) return false;
    const int di = c.deck;
    Deck* dp = di < kMaxDecks ? &decks[di] : nullptr;
    switch (c.op) {
      case Op::KeyLock: if (dp) dp->keylock = flag(dp->keylock, c.i); return true;
      case Op::KeyShift: if (dp) dp->key_shift = std::clamp(c.f, -12.0f, 12.0f); return true;
      case Op::Quantize: if (dp) dp->quantize = flag(dp->quantize, c.i); return true;
      case Op::Slip:
        if (dp) {
          dp->slip = flag(dp->slip, c.i);
          if (!dp->slip) dp->slip_on = false;
        }
        return true;
      case Op::Reverse: if (dp) dp->reverse = flag(dp->reverse, c.i); return true;
      case Op::LoopExit: if (dp) dp->loop_active = false; return true;
      case Op::LoopMove:
        if (dp && dp->trk && dp->loop_in >= 0 && dp->loop_out > dp->loop_in) {
          Deck& d = *dp;
          const double len = d.loop_out - d.loop_in;
          const double bf = 60.0 / std::max(1.0, bpm_at(d) > 0 ? bpm_at(d) : 120.0) * d.trk->audio->sample_rate;
          const double ni = std::clamp(d.loop_in + c.f * bf, 0.0, std::max(0.0, static_cast<double>(d.trk->audio->frames) - len));
          const double delta = ni - d.loop_in;
          d.loop_in = ni;
          d.loop_out = ni + len;
          if (d.loop_active) d.pos = clamp_pos(d, d.pos + delta);
        }
        return true;
      case Op::AutoGain: if (dp) dp->autogain_db = std::clamp(c.f, -24.0f, 12.0f); return true;
      case Op::ColorFx:
        if (dp) {
          dp->color = std::clamp(c.i, 0, static_cast<int>(ColorFx::Count) - 1);
          dp->c_filter = -1;
          fxb[di].select(kFxSlots, color_kind(dp->color));
        }
        return true;
      case Op::FxSelect: {
        FxBus* b = c.deck == kFxMaster ? &master_fx : (c.deck < kMaxDecks ? &fxb[c.deck] : nullptr);
        if (b && c.i >= 0 && c.i < kFxSlots) {
          const int k = static_cast<int>(c.d);
          b->select(c.i, k > 0 && k < static_cast<int>(EffectKind::Count) ? static_cast<EffectKind>(k) : EffectKind::None);
        }
        return true;
      }
      case Op::FxEnable: if (FxUnit* u = fx_unit(c.deck, c.i)) u->on = c.f > 0.5f; return true;
      case Op::FxWet: if (FxUnit* u = fx_unit(c.deck, c.i)) u->wet = std::clamp(c.f, 0.0f, 1.0f); return true;
      case Op::FxParam: if (FxUnit* u = fx_unit(c.deck, c.i)) u->param = std::clamp(c.f, 0.0f, 1.0f); return true;
      case Op::FxTime:
        if (FxUnit* u = fx_unit(c.deck, c.i)) {
          u->beats = std::clamp(c.f, 1.0f / 16, 16.0f);
          u->seconds = c.d > 0 ? c.d : 0;
        }
        return true;
      case Op::MasterBpm: master_bpm_manual = std::max(0.0f, c.f); return true;
      case Op::SamplerLoad:
        if (c.deck < kSamplerSlots) {
          SamplerSlot& s = sampler_slots[c.deck];
          if (s.s) sample_garbage.push(s.s);
          s = SamplerSlot{};
          s.s = c.sample;
          if (c.sample) {
            s.loop = c.sample->loop;
            s.sync = c.sample->sync;
          }
        } else if (c.sample) {
          sample_garbage.push(c.sample);
        }
        return true;
      case Op::SamplerTrigger:
        if (c.deck < kSamplerSlots && sampler_slots[c.deck].s && sampler_slots[c.deck].s->audio) {
          SamplerSlot& s = sampler_slots[c.deck];
          if (s.playing) {
            s.tail_pos = s.pos;
            s.tail_left = 96;
            s.tail_gain = s.env;
          }
          s.pos = 0;
          s.env = 0;
          s.vel = c.f > 0 ? std::min(c.f, 1.0f) : 1.0f;
          s.tgt = s.vel;
          s.playing = true;
        }
        return true;
      case Op::SamplerStop:
        for (int k = 0; k < kSamplerSlots; ++k)
          if (c.i < 0 || c.i == k) sampler_slots[k].tgt = 0;
        return true;
      case Op::SamplerLevel: sampler_level = std::clamp(c.f, 0.0f, 1.0f); return true;
      case Op::SamplerSync: if (c.deck < kSamplerSlots) sampler_slots[c.deck].sync = c.i != 0; return true;
      case Op::SamplerLoop: if (c.deck < kSamplerSlots) sampler_slots[c.deck].loop = c.i != 0; return true;
      case Op::SamplerGain: if (c.deck < kSamplerSlots) sampler_slots[c.deck].gain = std::clamp(c.f, 0.0f, 1.0f); return true;
      case Op::RecStart: rec_pre = c.f > 0.5f; rec.start(); return true;
      case Op::RecStop: rec.stop(); return true;
      case Op::StreamEnable: stream_pre = c.f > 0.5f; stream_on.store(c.i != 0); return true;
      case Op::SchedInsert: sched_accept(c.i); return true;
      case Op::SchedCancel: sched_cancel(static_cast<std::uint64_t>(c.d)); return true;
      case Op::SchedCancelAll: sched_cancel_all(); return true;
      default: return true;
    }
  }

  void apply(const Command& c) {
    Deck* d = c.deck < kMaxDecks ? &decks[c.deck] : nullptr;
    const bool track = d && d->trk && d->slip;
    const bool was = track && exc_active(*d);
    const double before = track ? d->pos : 0;
    const bool was_play = track && d->playing && !d->preview;
    if (!apply_ext(c)) apply_core(c);
    if ((c.op == Op::Load || c.op == Op::Eject) && d && n_deck > 0) sched_deck_unloaded(c.deck);
    if (d && d->trk && d->slip) {
      const bool now = exc_active(*d);
      if (!was && now && was_play) {
        d->slip_on = true;
        d->slip_pos = before;
      } else if (was && !now && d->slip_on) {
        d->pos = clamp_pos(*d, d->slip_pos);
        d->slip_on = false;
      }
      if (!d->playing) d->slip_on = false;
    }
  }

  // ---- scheduling core (audio thread) ----------------------------------------------
  void emit_sched(EventType t, std::uint64_t id, const Command& c, int slot, double frame) {
    EngineEvent e;
    e.type = t;
    e.deck = c.deck;
    e.slot = slot;
    e.frame = frame;
    e.sched_id = id;
    e.clock_frame = clock;
    events.push(e);
  }
  void sched_release(SchedEntry& e, int idx) {
    e.alive = false;
    std::uint64_t expect = e.id;
    sched_live[e.id & kLiveMask].compare_exchange_strong(expect, 0);
    sched_pending.fetch_sub(1);
    slab_free[slab_free_n++] = static_cast<std::uint16_t>(idx);
  }
  void list_erase(std::uint16_t* a, int& n, int idx) {
    for (int k = 0; k < n; ++k)
      if (a[k] == idx) {
        std::memmove(a + k, a + k + 1, sizeof(std::uint16_t) * static_cast<std::size_t>(n - k - 1));
        --n;
        return;
      }
  }
  // Removes an entry from its list and frees it.
  void sched_remove(int idx) {
    SchedEntry& e = slab[idx];
    if (e.is_deck) {
      list_erase(deck_list, n_deck, idx);
      --deck_cnt[e.deck];
      if (e.flagged) --n_flagged;
    } else {
      list_erase(clock_order, n_clock, idx);
    }
    sched_release(e, idx);
  }
  void sched_fire(int idx) {
    SchedEntry& e = slab[idx];
    if (!e.alive) return;
    const Command c = e.cmd;
    const std::uint64_t id = e.id;
    const double at = e.is_deck ? decks[e.deck].pos : 0.0;
    sched_remove(idx);
    apply(c);
    emit_sched(EventType::ScheduledFired, id, c, static_cast<int>(c.op), at);
  }
  void sched_drop(int idx, DropReason r) {
    SchedEntry& e = slab[idx];
    if (!e.alive) return;
    const Command c = e.cmd;
    const std::uint64_t id = e.id;
    sched_remove(idx);
    emit_sched(EventType::ScheduledDropped, id, c, static_cast<int>(r), 0);
  }
  void sched_deck_unloaded(int di) {
    for (int k = n_deck - 1; k >= 0; --k)
      if (k < n_deck && slab[deck_list[k]].deck == di) sched_drop(deck_list[k], DropReason::DeckUnloaded);
  }
  void sched_cancel(std::uint64_t id) {
    for (int i = 0; i < kSchedCap; ++i)
      if (slab[i].alive && slab[i].id == id) { sched_remove(i); return; }
  }
  void sched_cancel_all() {
    for (int i = 0; i < kSchedCap; ++i)
      if (slab[i].alive) sched_remove(i);
  }

  // Beat grid arithmetic. Beat 0 is the first grid beat; section i + 1 continues the count after the whole beats of
  // section i (so beat numbers are monotonic across tempo changes).
  struct Grid {
    const GridSection* s = nullptr;
    int n = 0;
    GridSection virt;
    double sr = 44100;
    double base[64];  // beat number at the start of each section (first 64 sections)
    double bf(int i) const { return 60.0 / s[i].bpm * sr; }
  };
  static bool make_grid(const LoadedTrack& t, Grid& g) {
    g.sr = t.audio->sample_rate;
    if (t.grid.empty()) {
      if (t.bpm <= 0) return false;
      g.virt = GridSection{0, t.bpm, 1};
      g.s = &g.virt;
      g.n = 1;
    } else {
      g.s = t.grid.data();
      g.n = static_cast<int>(std::min<std::size_t>(t.grid.size(), 64));
    }
    g.base[0] = 0;
    for (int i = 0; i < g.n; ++i) {
      if (g.s[i].bpm <= 0) return false;
      if (i > 0) g.base[i] = g.base[i - 1] + std::ceil((g.s[i].first_beat_frame - g.s[i - 1].first_beat_frame) / g.bf(i - 1) - 1e-6);
    }
    return true;
  }
  static double grid_beat_to_frame(const Grid& g, double b) {
    int i = 0;
    while (i + 1 < g.n && g.base[i + 1] <= b) ++i;
    return g.s[i].first_beat_frame + (b - g.base[i]) * g.bf(i);
  }
  static double grid_frame_to_beat(const Grid& g, double p) {
    int i = 0;
    while (i + 1 < g.n && g.s[i + 1].first_beat_frame <= p) ++i;
    return g.base[i] + (p - g.s[i].first_beat_frame) / g.bf(i);
  }
  static int grid_bar_beat(const Grid& g, double b) {  // 1..4
    int i = 0;
    while (i + 1 < g.n && g.base[i + 1] <= b) ++i;
    const long k = static_cast<long>(g.s[i].bar_beat - 1) + static_cast<long>(std::llround(b - g.base[i]));
    return static_cast<int>(((k % 4) + 4) % 4) + 1;
  }
  bool resolve_beat(const Deck& d, const Schedule& w, double& frame) const {
    Grid g;
    if (!d.trk || !make_grid(*d.trk, g)) return false;
    double b;
    if (w.relative) b = std::floor(grid_frame_to_beat(g, d.pos) + 1e-9) + w.beat;
    else b = w.beat;
    if (w.bar_aligned) {
      b = std::ceil(b - 1e-9);
      for (int k = 0; k < 4 && grid_bar_beat(g, b) != 1; ++k) b += 1;
    }
    frame = grid_beat_to_frame(g, b);
    return true;
  }

  void sched_accept(int slot) {
    const SchedPayload p = sched_pool[static_cast<std::size_t>(slot)];
    sched_free.push(static_cast<std::uint32_t>(slot));
    auto reject = [&](DropReason r) {
      std::uint64_t expect = p.id;
      sched_live[p.id & kLiveMask].compare_exchange_strong(expect, 0);
      sched_pending.fetch_sub(1);
      emit_sched(EventType::ScheduledDropped, p.id, p.cmd, static_cast<int>(r), 0);
    };
    if (slab_free_n == 0) return reject(DropReason::TableFull);
    double frame = p.when.frame;
    const bool is_deck = p.when.kind != ScheduleKind::ClockFrame;
    if (is_deck) {
      if (p.when.deck >= kMaxDecks || !decks[p.when.deck].trk) return reject(DropReason::Unresolvable);
      if (p.when.kind == ScheduleKind::DeckBeat && !resolve_beat(decks[p.when.deck], p.when, frame))
        return reject(DropReason::Unresolvable);
    }
    const int idx = slab_free[--slab_free_n];
    SchedEntry& e = slab[idx];
    e = SchedEntry{};
    e.cmd = p.cmd;
    e.id = p.id;
    e.alive = true;
    e.is_deck = is_deck;
    if (!is_deck) {
      e.at = p.when.clock_frame;
      int lo = 0, hi = n_clock;  // upper_bound on `at`: equal frames keep insertion order
      while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (slab[clock_order[mid]].at <= e.at) lo = mid + 1; else hi = mid;
      }
      std::memmove(clock_order + lo + 1, clock_order + lo, sizeof(std::uint16_t) * static_cast<std::size_t>(n_clock - lo));
      clock_order[lo] = static_cast<std::uint16_t>(idx);
      ++n_clock;
    } else {
      e.deck = p.when.deck;
      e.frame = frame;
      e.fire_if_passed = p.when.fire_if_passed;
      e.armed = decks[e.deck].pos < frame;
      if (!e.armed && e.fire_if_passed) { e.flagged = true; ++n_flagged; }
      deck_list[n_deck++] = static_cast<std::uint16_t>(idx);
      ++deck_cnt[e.deck];
    }
  }

  // Mirrors render_deck's position recurrence for a steadily playing deck; returns the first step (0-based) in
  // which the playhead moves from below `fmin` to at / beyond it, or -1.
  int sim_cross(const Deck& d, int di, int n, double fmin, double& lo, double& hi) const {
    const TrackAudio& a = *d.trk->audio;
    const double ratio = static_cast<double>(a.sample_rate) / sr;
    const double total = static_cast<double>(static_cast<std::int64_t>(a.frames));
    const double bend = std::clamp(d.bend * kBendPerRev, -0.6, 0.6);
    double target = effective_tempo(d, di) * (1.0 + bend);
    const double slew = 1.0 - std::exp(-1.0 / (0.003 * sr));
    double speed = d.speed, pos = d.pos;
    for (int i = 0; i < n; ++i) {
      speed += (target - speed) * slew;
      const double before = pos;
      pos += speed * ratio;
      if (speed > 0 && before < fmin && pos >= fmin) {
        lo = before;
        hi = pos;
        return i;
      }
      if (d.loop_active && speed > 0 && pos >= d.loop_out) {
        const double len = d.loop_out - d.loop_in;
        pos = len > 1 ? d.loop_in + std::fmod(pos - d.loop_out, len) : d.loop_in;
      } else if (d.loop_active && speed < 0 && pos < d.loop_in) {
        const double len = d.loop_out - d.loop_in;
        pos = len > 1 ? d.loop_out - std::fmod(d.loop_in - pos, len) : d.loop_out;
      }
      if (pos >= total) {
        pos = total;
        target = 0;
      } else if (pos < 0) {
        pos = 0;
      }
    }
    return -1;
  }

  // Called before each chunk while anything is scheduled: fires what is due now and shortens the chunk so the
  // next due trigger lands exactly on a chunk start.
  void sched_begin_chunk(int& n) {
    while (n_clock > 0 && slab[clock_order[0]].at <= clock) sched_fire(clock_order[0]);
    if (n_deck > 0) {
      std::uint16_t buf[kSchedCap];
      int nb = 0;
      bool active[kMaxDecks];
      for (int di = 0; di < kMaxDecks; ++di) {
        const Deck& d = decks[di];
        active[di] = d.trk && deck_cnt[di] > 0 && (d.playing || d.touching || d.speed != 0 || d.pos != d.pos_prev);
      }
      for (int k = 0; k < n_deck; ++k) {
        SchedEntry& e = slab[deck_list[k]];
        if (e.flagged) { buf[nb++] = deck_list[k]; continue; }
        if (!active[e.deck]) continue;
        const double pos = decks[e.deck].pos;
        if (e.armed) {
          if (pos >= e.frame) {  // passed without being seen crossing: a jump
            if (e.fire_if_passed) buf[nb++] = deck_list[k];
            else e.armed = false;
          }
        } else if (pos < e.frame) {
          e.armed = true;
        }
      }
      std::sort(buf, buf + nb, [&](std::uint16_t x, std::uint16_t y) { return slab[x].id < slab[y].id; });
      for (int k = 0; k < nb; ++k) sched_fire(buf[k]);
    }
    if (n_clock > 0) {
      const std::uint64_t until = slab[clock_order[0]].at - clock;
      if (until < static_cast<std::uint64_t>(n)) n = static_cast<int>(until);
    }
    if (n_deck == 0) return;
    int step[kMaxDecks];
    double lo[kMaxDecks], hi[kMaxDecks];
    for (int di = 0; di < kMaxDecks; ++di) {
      step[di] = -1;
      const Deck& d = decks[di];
      if (deck_cnt[di] == 0 || !d.trk || !d.playing || d.touching || d.reverse) continue;
      double fmin = 1e300;
      for (int k = 0; k < n_deck; ++k) {
        const SchedEntry& e = slab[deck_list[k]];
        if (e.deck == di && e.armed && !e.flagged && e.frame > d.pos) fmin = std::min(fmin, e.frame);
      }
      if (fmin >= 1e300) continue;
      const int k = sim_cross(d, di, n, fmin, lo[di], hi[di]);
      if (k >= 0) {
        step[di] = k;
        n = k + 1;
      }
    }
    for (int di = 0; di < kMaxDecks; ++di) {
      if (step[di] < 0 || step[di] + 1 != n) continue;
      for (int k = 0; k < n_deck; ++k) {
        SchedEntry& e = slab[deck_list[k]];
        if (e.deck == di && e.armed && !e.flagged && e.frame > lo[di] && e.frame <= hi[di]) {
          e.flagged = true;
          ++n_flagged;
        }
      }
    }
  }

  void note_callback_time() {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double raw = t - static_cast<double>(clock) / sr;
    if (!off_init) {
      off_est = raw;
      off_init = true;
    } else if (raw < off_est) {
      off_est = raw;  // an early callback is the best look at the true timeline
    } else {
      off_est += (raw - off_est) * 0.002;  // slow leak absorbs clock skew
    }
    ClockSnap cs;
    cs.offset = off_est;
    cs.valid = 1;
    clock_snap.write(cs);
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
      if (d.reverse) target = -target;
    } else {
      target = bend * 6.0;  // nudging a stopped deck searches slowly
    }
    const bool steady = d.playing && !d.touching;
    if (!steady && target == 0 && std::fabs(d.speed) < 1e-4 && d.amp < 1e-4f) {
      d.speed = 0;
      d.amp = 0;
      d.pos_prev = d.pos;
      d.kl_w = 0;
      d.st_live = false;
      d.xf_left = 0;
      std::memset(buf, 0, sizeof(float) * 2 * static_cast<std::size_t>(n));
      return;
    }

    // A jump of the play position (command, loop, slip return): crossfade from where we would have been.
    if (d.pos != d.pos_prev && steady && d.amp > 0.01f) {
      d.xf_pos = d.pos_prev;
      d.xf_left = 128;
      if (d.st_live) d.st.flush();
    }
    if (d.slip_on) d.slip_pos = std::min(d.slip_pos + d.tempo_mult * ratio * n, static_cast<double>(total));

    // Key lock engages for steady forward play away from 1.0 (or with a key shift).
    const double pitch = std::exp2(static_cast<double>(d.key_shift) / 12.0);
    const double dev = std::fabs(d.tempo_mult - 1.0);
    const bool away = dev > (d.kl_hot ? 0.0005 : 0.001) || d.key_shift != 0.0f;
    const bool want = d.keylock && steady && target > 0 && away;
    d.kl_hot = want;
    const float w_target = want ? 1.0f : 0.0f;
    const float w_step = 1.0f / 256.0f;

    const double slew = 1.0 - std::exp(-1.0 / (0.003 * sr));
    const float amp_slew = static_cast<float>(1.0 - std::exp(-1.0 / (0.004 * sr)));
    for (int i = 0; i < n; ++i) {
      d.speed += (target - d.speed) * slew;
      const float amp_target = steady ? 1.0f : static_cast<float>(std::min(1.0, std::fabs(d.speed) * 25.0));
      d.amp += (amp_target - d.amp) * amp_slew;
      if (d.kl_w < w_target) d.kl_w = std::min(w_target, d.kl_w + w_step);
      else if (d.kl_w > w_target) d.kl_w = std::max(w_target, d.kl_w - w_step);
      if (d.kl_w > 0 && !d.st_live) {
        d.st.reset();
        d.st_live = true;
      } else if (d.kl_w == 0) {
        d.st_live = false;
      }
      float l = 0, r = 0;
      if (d.kl_w < 1.0f) read_frame(a.pcm.data(), total, d.pos, l, r);
      if (d.xf_left > 0) {
        if (d.kl_w < 1.0f) {
          float ol, orr;
          read_frame(a.pcm.data(), total, d.xf_pos, ol, orr);
          const float t = static_cast<float>(d.xf_left) / 128.0f;
          l = l * (1.0f - t) + ol * t;
          r = r * (1.0f - t) + orr * t;
        }
        d.xf_pos += d.speed * ratio;
        --d.xf_left;
      }
      if (d.kl_w > 0.0f) {
        float sl, sr_;
        d.st.next(a.pcm.data(), total, ratio, d.pos, std::fabs(d.speed) * ratio, pitch, sl, sr_);
        if (d.kl_w < 1.0f) {
          l = l * (1.0f - d.kl_w) + sl * d.kl_w;
          r = r * (1.0f - d.kl_w) + sr_ * d.kl_w;
        } else {
          l = sl;
          r = sr_;
        }
      }
      buf[i * 2] = l * d.amp;
      buf[i * 2 + 1] = r * d.amp;
      d.pos += d.speed * ratio;
      if (d.loop_active && d.speed > 0 && d.pos >= d.loop_out) {
        const double len = d.loop_out - d.loop_in;
        d.xf_pos = d.pos;
        d.xf_left = 128;
        if (d.st_live) d.st.flush();
        d.pos = len > 1 ? d.loop_in + std::fmod(d.pos - d.loop_out, len) : d.loop_in;
        if (d.touching) d.scratch_target -= len;
      } else if (d.loop_active && d.speed < 0 && d.pos < d.loop_in) {
        const double len = d.loop_out - d.loop_in;
        d.xf_pos = d.pos;
        d.xf_left = 128;
        d.pos = len > 1 ? d.loop_out - std::fmod(d.loop_in - d.pos, len) : d.loop_out;
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
        if (d.playing && !d.touching && d.reverse) {
          d.playing = d.preview = false;
          target = 0;
        }
      }
    }
    d.pos_prev = d.pos;
  }

  // Trim, three-band EQ and the filter knob, in place. Also tracks the pre-fader level.
  void process_channel(Deck& d, int di, float* buf, int n, const FxCtx& cx) {
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
    const float fk = d.color == 0 ? d.s_filter : 0.5f;  // other colour effects take the knob instead
    if (std::fabs(fk - d.c_filter) > 1e-4f) {
      const float f = d.c_filter = fk;
      const double top = std::min(20000.0, fs * 0.45);
      if (f < 0.47f) {
        low_pass(d.q_filter, fs, top * std::pow(80.0 / top, (0.47 - f) / 0.47), 1.0);
      } else if (f > 0.53f) {
        high_pass(d.q_filter, fs, std::min(top, 20.0 * std::pow(8000.0 / 20.0, (f - 0.53) / 0.47)), 1.0);
      } else {
        d.q_filter.identity();
      }
    }
    d.s_auto += (std::pow(10.0f, d.autogain_db / 20.0f) - d.s_auto) * sm;
    const float g0 = d.g_trim_prev, g1 = trim_gain(d.s_trim) * d.s_auto;
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
    if (d.color != 0) fxb[di].run_colour(buf, n, cx, d.s_filter);
  }

  FxCtx make_ctx(const Deck& d, FxBus& b, int n) {
    FxCtx cx;
    cx.sr = sr;
    double bpm = d.bpm_now * d.tempo_mult;
    if (bpm <= 0) bpm = master_bpm > 0 ? master_bpm : 120.0;
    cx.bf = 60.0 / bpm * sr;
    const GridSection* s = d.trk && d.playing && !d.touching ? section_at(d) : nullptr;
    cx.beat_pos = s && s->bpm > 0 ? (d.pos - s->first_beat_frame) / beat_frames(d) : b.clock;
    b.clock += n / cx.bf;
    return cx;
  }

  void render_sampler(float* master, int n) {
    const float lg = level_gain(sampler_level);
    for (int k = 0; k < kSamplerSlots; ++k) {
      SamplerSlot& s = sampler_slots[k];
      if (!s.playing || !s.s || !s.s->audio) continue;
      const TrackAudio& a = *s.s->audio;
      const std::int64_t total = static_cast<std::int64_t>(a.frames);
      const double rate = (s.sync && s.s->bpm > 0 && master_bpm > 0) ? std::clamp(master_bpm / s.s->bpm, 0.5, 2.0) : 1.0;
      const double step = static_cast<double>(a.sample_rate) / sr * rate;
      const float g = level_gain(s.gain) * s.s->gain * lg;
      for (int i = 0; i < n; ++i) {
        if (s.env < s.tgt) s.env = std::min(s.tgt, s.env + 1.0f / 96.0f);
        else if (s.env > s.tgt) s.env = std::max(s.tgt, s.env - 1.0f / 256.0f);
        if (s.tgt == 0 && s.env == 0) {
          s.playing = false;
          break;
        }
        float l, r;
        read_frame(a.pcm.data(), total, s.pos, l, r);
        master[i * 2] += l * s.env * g;
        master[i * 2 + 1] += r * s.env * g;
        if (s.tail_left > 0) {
          float tl, tr;
          read_frame(a.pcm.data(), total, s.tail_pos, tl, tr);
          const float tg = s.tail_gain * static_cast<float>(s.tail_left) / 96.0f * g;
          master[i * 2] += tl * tg;
          master[i * 2 + 1] += tr * tg;
          s.tail_pos += step;
          --s.tail_left;
        }
        s.pos += step;
        if (s.pos >= static_cast<double>(total)) {
          if (s.loop) s.pos = std::fmod(s.pos, static_cast<double>(std::max<std::int64_t>(total, 1)));
          else s.tgt = 0;
        }
      }
    }
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
        if (!fxb[di].busy()) continue;
        std::memset(buf, 0, sizeof(float) * 2 * static_cast<std::size_t>(n));  // let effect tails ring out
        const FxCtx cx = make_ctx(d, fxb[di], n);
        fxb[di].run_slots(buf, n, cx);
        for (int i = 0; i < n * 2; ++i) master[i] += buf[i] * d.fader * d.fader;
        continue;
      }
      render_deck(d, di, buf, n);
      const FxCtx cx = make_ctx(d, fxb[di], n);
      process_channel(d, di, buf, n, cx);
      fxb[di].run_slots(buf, n, cx);
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

    // master tempo: the manual value, else the first playing deck
    master_bpm = master_bpm_manual;
    const Deck* lead = nullptr;
    for (const Deck& d : decks)
      if (d.trk && d.playing && d.bpm_now > 0) { lead = &d; break; }
    if (master_bpm <= 0 && lead) master_bpm = lead->bpm_now * lead->tempo_mult;
    render_sampler(master, n);
    {
      FxCtx cx;
      cx.sr = sr;
      const double bpm = master_bpm > 0 ? master_bpm : 120.0;
      cx.bf = 60.0 / bpm * sr;
      const GridSection* s = lead ? section_at(*lead) : nullptr;
      cx.beat_pos = s && s->bpm > 0 ? (lead->pos - s->first_beat_frame) / beat_frames(*lead) : master_clock;
      master_clock += n / cx.bf;
      master_fx.run_slots(master, n, cx);
    }

    const float mg1 = level_gain(master_level), mg0 = g_master_prev;
    const float hg1 = level_gain(head_level), hg0 = g_head_prev;
    g_master_prev = mg1;
    g_head_prev = hg1;
    s_head_mix += (head_mix - s_head_mix) * 0.4f;
    const float mix = s_head_mix;
    float pl = 0, pr = 0;
    float tap_pre[kChunk * 2], tap_post[kChunk * 2];
    for (int i = 0; i < n; ++i) {
      const float t = static_cast<float>(i) * inv_n;
      const float mg = mg0 + (mg1 - mg0) * t, hg = hg0 + (hg1 - hg0) * t;
      const float ml = soft_clip(master[i * 2] * mg), mr = soft_clip(master[i * 2 + 1] * mg);
      float* frame = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(channels);
      tap_pre[i * 2] = master[i * 2] * mg;
      tap_pre[i * 2 + 1] = master[i * 2 + 1] * mg;
      tap_post[i * 2] = ml;
      tap_post[i * 2 + 1] = mr;
      frame[master_ch] = ml;
      frame[master_ch + 1] = mr;
      if (phones_ch >= 0) {
        frame[phones_ch] = soft_clip((cue[i * 2] * (1 - mix) + master[i * 2] * mix) * hg);
        frame[phones_ch + 1] = soft_clip((cue[i * 2 + 1] * (1 - mix) + master[i * 2 + 1] * mix) * hg);
      }
      pl = std::max(pl, std::fabs(ml));
      pr = std::max(pr, std::fabs(mr));
    }
    if (rec.recording()) rec.push(rec_pre ? tap_pre : tap_post, static_cast<std::size_t>(n));
    if (stream_on.load(std::memory_order_relaxed)) {
      const std::size_t wr = stream.push(stream_pre ? tap_pre : tap_post, static_cast<std::size_t>(n));
      if (wr < static_cast<std::size_t>(n)) stream_dropped.fetch_add(static_cast<std::size_t>(n) - wr, std::memory_order_relaxed);
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
      v.key_lock = d.keylock;
      v.key_lock_active = d.kl_w > 0;
      v.quantize = d.quantize;
      v.slip = d.slip;
      v.slip_active = d.slip_on;
      v.slip_position = d.slip_on ? d.slip_pos : -1;
      v.reverse = d.reverse;
      v.key_shift = d.key_shift;
      v.auto_gain_db = d.autogain_db;
      v.color_fx = d.color;
      fxb[di].view(v.fx);
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
    master_fx.view(m.fx);
    m.sampler_level = sampler_level;
    m.master_bpm = master_bpm;
    m.stream_enabled = stream_on.load();
    for (int k = 0; k < kSamplerSlots; ++k) {
      const SamplerSlot& s = sampler_slots[k];
      SamplerView sv;
      sv.loaded = s.s != nullptr;
      sv.playing = s.playing;
      sv.loop = s.loop;
      sv.sync = s.sync;
      sv.position = s.pos;
      sv.length = s.s && s.s->audio ? static_cast<double>(s.s->audio->frames) : 0;
      sampler_views[k].write(sv);
    }
    mixer_view.write(m);
  }

  void render(float* out, std::uint32_t frames) {
    if (timing_on) note_callback_time();
    Command c;
    while (commands.pop(c)) apply(c);
    std::memset(out, 0, sizeof(float) * frames * static_cast<std::size_t>(channels));
    std::uint32_t done = 0;
    while (done < frames) {
      int n = static_cast<int>(std::min<std::uint32_t>(kChunk, frames - done));
      if (n_clock + n_deck > 0) sched_begin_chunk(n);  // may shorten the chunk to land a trigger on its first sample
      render_chunk(out + static_cast<std::size_t>(done) * static_cast<std::size_t>(channels), n);
      done += static_cast<std::uint32_t>(n);
      clock += static_cast<std::uint64_t>(n);
    }
    publish();
    clock_pub.store(clock, std::memory_order_release);
  }

  static void data_callback(ma_device* device, void* output, const void*, ma_uint32 frames) {
    static_cast<Impl*>(device->pUserData)->render(static_cast<float*>(output), frames);
  }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {
  impl_->prepare_audio();
  impl_->publish();
}

Engine::~Engine() {
  stop();
  Command c;
  impl_->rec.abort();
  while (impl_->commands.pop(c)) {
    delete c.track;
    delete c.sample;
  }
  for (auto& sl : impl_->sampler_slots) delete sl.s;
  { LoadedSample* ls = nullptr; while (impl_->sample_garbage.pop(ls)) delete ls; }
  for (Deck& d : impl_->decks) delete d.trk;
  collect_garbage();
}

std::vector<std::string> Engine::output_devices() {
  std::vector<std::string> names;
  ma_context ctx;
  if (init_context(&ctx) != MA_SUCCESS) return names;
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
  if (init_context(&s.ctx) != MA_SUCCESS) {
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
  s.prepare_audio();  // rebuild filters, stretchers and effect memory for this rate

  s.off_init = false;
  s.timing_on = true;
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
  s.timing_on = false;
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
    delete c.sample;
  }
}

void Engine::load(int deck, std::unique_ptr<LoadedTrack> track) {
  Command c;
  c.op = Op::Load;
  c.deck = static_cast<std::uint8_t>(deck);
  c.track = track.release();
  send(c);
}

std::uint64_t Engine::clock_frames() const { return impl_->clock_pub.load(std::memory_order_acquire); }
double Engine::clock_seconds() const { return static_cast<double>(clock_frames()) / impl_->sr; }

std::uint64_t Engine::clock_at_wall(std::chrono::steady_clock::time_point t) const {
  const Impl& s = *impl_;
  const ClockSnap cs = s.clock_snap.read();
  if (!s.dev_ok || !cs.valid) return clock_frames();
  const double secs = std::chrono::duration<double>(t.time_since_epoch()).count() - cs.offset;
  return secs <= 0 ? 0 : static_cast<std::uint64_t>(secs * s.sr);
}

std::uint64_t Engine::schedule(const Command& c, const Schedule& when) {
  Impl& s = *impl_;
  if (c.op == Op::Load || c.op == Op::SamplerLoad || c.op >= Op::SchedInsert || c.track || c.sample) return 0;
  if (when.kind > ScheduleKind::DeckBeat) return 0;
  if (when.kind != ScheduleKind::ClockFrame && when.deck >= kMaxDecks) return 0;
  if (when.kind == ScheduleKind::DeckBeat && when.relative && !(when.beat > 0)) return 0;
  std::lock_guard<std::mutex> lock(s.send_mutex);
  std::uint32_t slot;
  while (s.sched_free.pop(slot)) s.sched_free_slots.push_back(slot);
  if (s.sched_free_slots.empty()) return 0;
  slot = s.sched_free_slots.back();
  s.sched_free_slots.pop_back();
  const std::uint64_t id = ++s.sched_next_id;
  SchedPayload& p = s.sched_pool[slot];
  p.cmd = c;
  p.when = when;
  p.id = id;
  Command ins;
  ins.op = Op::SchedInsert;
  ins.i = static_cast<std::int32_t>(slot);
  s.sched_live[id & kLiveMask].store(id);  // before the push: the audio thread may retire it at once
  s.sched_pending.fetch_add(1);
  if (!s.commands.push(ins)) {
    s.sched_pending.fetch_sub(1);
    s.sched_live[id & kLiveMask].store(0);
    s.sched_free_slots.push_back(slot);
    return 0;
  }
  return id;
}

bool Engine::cancel(std::uint64_t id) {
  Impl& s = *impl_;
  if (id == 0) return false;
  std::lock_guard<std::mutex> lock(s.send_mutex);
  if (s.sched_live[id & kLiveMask].load() != id) return false;
  Command c;
  c.op = Op::SchedCancel;
  c.d = static_cast<double>(id);
  return s.commands.push(c);
}

std::size_t Engine::scheduled_count() const {
  const std::int64_t n = impl_->sched_pending.load();
  return n > 0 ? static_cast<std::size_t>(n) : 0;
}

void Engine::cancel_all() {
  Command c;
  c.op = Op::SchedCancelAll;
  send(c);
}

SamplerView Engine::sampler(int slot) const { return impl_->sampler_views[std::clamp(slot, 0, kSamplerSlots - 1)].read(); }

void Engine::sampler_load(int slot, std::unique_ptr<LoadedSample> sample) {
  Command c;
  c.op = Op::SamplerLoad;
  c.deck = static_cast<std::uint8_t>(std::clamp(slot, 0, 255));
  c.sample = sample.release();
  send(c);
}

bool Engine::record_open(const std::string& path, bool pre_clip, std::string& error) {
  if (!impl_->rec.open(path, impl_->sr, error)) return false;
  Command c;
  c.op = Op::RecStart;
  c.f = pre_clip ? 1.0f : 0.0f;
  send(c);
  return true;
}
void Engine::record_stop() {
  Command c;
  c.op = Op::RecStop;
  send(c);
}
void Engine::record_close() {
  record_stop();
  for (int i = 0; i < 400 && impl_->rec.state() != Recorder::Done && impl_->rec.state() != Recorder::Idle; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (impl_->rec.state() != Recorder::Done && impl_->rec.state() != Recorder::Idle) impl_->rec.abort();
  impl_->rec.close();
}
RecorderStatus Engine::recorder_status() const {
  RecorderStatus st;
  const Recorder& r = impl_->rec;
  const int s = r.state();
  st.open = s == Recorder::Armed || s == Recorder::Recording || s == Recorder::Stopping;
  st.recording = s == Recorder::Recording;
  st.finished = r.finished();
  st.error = r.error();
  st.seconds = r.seconds();
  st.dropped_frames = r.dropped();
  st.bytes = r.bytes();
  return st;
}
void Engine::stream_enable(bool on, bool pre_clip) {
  Command c;
  c.op = Op::StreamEnable;
  c.i = on ? 1 : 0;
  c.f = pre_clip ? 1.0f : 0.0f;
  send(c);
}
std::size_t Engine::stream_read(float* out, std::size_t max_frames) { return impl_->stream.pop(out, max_frames); }
std::uint64_t Engine::stream_dropped_frames() const { return impl_->stream_dropped.load(); }

DeckView Engine::deck(int index) const { return impl_->deck_views[std::clamp(index, 0, kMaxDecks - 1)].read(); }
MixerView Engine::mixer() const { return impl_->mixer_view.read(); }
bool Engine::poll(EngineEvent& out) { return impl_->events.pop(out); }

void Engine::collect_garbage() {
  LoadedTrack* t = nullptr;
  while (impl_->garbage.pop(t)) delete t;
  LoadedSample* ls = nullptr;
  while (impl_->sample_garbage.pop(ls)) delete ls;
}

void Engine::configure_offline(std::uint32_t sample_rate, int channels) {
  impl_->sr = sample_rate;
  impl_->channels = channels;
  impl_->master_ch = 0;
  impl_->phones_ch = channels >= 4 ? 2 : -1;
  impl_->prepare_audio();
}

void Engine::render(float* out, std::uint32_t frames) { impl_->render(out, frames); }

}  // namespace deckhand
