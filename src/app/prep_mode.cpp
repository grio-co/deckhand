#include "deckhand/app/prep_mode.hpp"

#include <algorithm>
#include <cmath>

namespace deckhand {

constexpr int PrepMode::kDeck[2];

namespace {
}

PrepMode::PrepMode(AutomixHost& host) : host_(host) {}

PrepMode::~PrepMode() {
  if (active_) exit();
}

void PrepMode::send(Op op, int deck, float f, int i, double d) {
  Command c;
  c.op = op;
  c.deck = static_cast<std::uint8_t>(deck);
  c.f = f;
  c.i = i;
  c.d = d;
  host_.engine().send(c);
}

bool PrepMode::enter(std::string& refusal, std::string& message) {
  std::lock_guard<std::recursive_mutex> lock(m_);
  if (active_) return true;
  IEngine& e = host_.engine();
  if (closing_) {
    refusal = "busy_deck";
    message = "preparation mode is still closing";
    return false;
  }
  if (blocker_) {
    const std::string why = blocker_();
    if (!why.empty()) {
      refusal = "blocked";
      message = why;
      return false;
    }
  }
  for (int s = 0; s < 2; ++s) {
    const DeckView v = e.deck(kDeck[s]);
    if (v.loaded || v.playing) {
      refusal = "busy_deck";
      message = "deck " + std::to_string(kDeck[s] + 1) + " holds a track: preparation mode needs decks 3 and 4 empty";
      return false;
    }
  }
  if (!(cue_check_ ? cue_check_() : e.output_channels() >= 4)) {
    refusal = "no_cue_output";
    message = "preparation mode needs a headphone output";
    return false;
  }
  for (int s = 0; s < 2; ++s) {
    const DeckView v = e.deck(kDeck[s]);
    saved_[s].fader = v.fader;
    saved_[s].pfl = v.pfl;
    saved_[s].trim_db = v.auto_gain_db;
    saved_[s].tempo = static_cast<float>(v.tempo);
    saved_[s].tempo_range = static_cast<float>(v.tempo_range);
    slot_[s] = Slot{};
    // Nothing of these decks reaches the master (fader at zero); the headphone cue is pre-fader.
    send(Op::Fader, kDeck[s], 0.0f);
    send(Op::Pfl, kDeck[s], 0, 1);
    e.cue_only(kDeck[s], 1);  // the engine itself keeps these decks off the master, whatever a fader or the crossfader do
  }
  tempo_match_ = true;
  blend_ = 0.5;
  swap_ids_.clear();
  swap_fired_ = 0;
  guard_hits_ = 0;
  note_.clear();
  active_ = true;
  apply_blend_commands();
  return true;
}

void PrepMode::exit() {
  std::lock_guard<std::recursive_mutex> lock(m_);
  if (!active_) return;
  IEngine& e = host_.engine();
  for (const std::uint64_t id : swap_ids_) e.cancel(id);
  swap_ids_.clear();
  active_ = false;
  closing_ = slot_[0].loading || slot_[1].loading;  // a load in flight lands first, then the decks are cleared
  if (!closing_) restore();
}

void PrepMode::restore() {
  IEngine& e = host_.engine();
  for (int s = 0; s < 2; ++s) {
    const int d = kDeck[s];
    e.cue_only(d, 0);
    if (e.deck(d).playing) send(Op::PlayToggle, d);
    send(Op::Eject, d);
    send(Op::Tempo, d, 0.0f);
    send(Op::TempoRange, d, saved_[s].tempo_range);
    send(Op::AutoGain, d, saved_[s].trim_db);
    send(Op::Pfl, d, 0, saved_[s].pfl ? 1 : 0);
    send(Op::Fader, d, saved_[s].fader);
    slot_[s] = Slot{};
  }
  closing_ = false;
}

bool PrepMode::load(int slot, std::int64_t track_id, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(m_);
  if (!active_) {
    error = "preparation mode is off";
    return false;
  }
  if (slot < 0 || slot > 1) {
    error = "no such player";
    return false;
  }
  const int d = kDeck[slot];
  if (host_.engine().deck(d).playing) send(Op::PlayToggle, d);
  Slot& s = slot_[slot];
  s.facts = host_.facts(track_id);
  if (!s.facts.ok) {
    error = "no such track";
    return false;
  }
  if (!host_.request_load(d, track_id)) {
    error = "the track could not be loaded";
    return false;
  }
  s.want = track_id;
  s.loading = true;
  note_.clear();
  return true;
}

void PrepMode::play(int slot, int mode) {
  std::lock_guard<std::recursive_mutex> lock(m_);
  if (!active_ || slot < 0 || slot > 1) return;
  const DeckView v = host_.engine().deck(kDeck[slot]);
  if (!v.loaded) return;
  if (mode < 0 || (mode == 1 && !v.playing) || (mode == 0 && v.playing)) send(Op::PlayToggle, kDeck[slot]);
}

void PrepMode::cue(int slot, bool down) {
  std::lock_guard<std::recursive_mutex> lock(m_);
  if (!active_ || slot < 0 || slot > 1 || !host_.engine().deck(kDeck[slot]).loaded) return;
  send(down ? Op::CueDown : Op::CueUp, kDeck[slot]);
}

void PrepMode::seek(int slot, double ms) {
  std::lock_guard<std::recursive_mutex> lock(m_);
  if (!active_ || slot < 0 || slot > 1) return;
  const DeckView v = host_.engine().deck(kDeck[slot]);
  if (!v.loaded) return;
  send(Op::Seek, kDeck[slot], 0, 0, std::clamp(ms, 0.0, v.length / v.sample_rate * 1000.0) / 1000.0 * v.sample_rate);
}

void PrepMode::set_tempo_match(bool on) {
  std::lock_guard<std::recursive_mutex> lock(m_);
  tempo_match_ = on;
  if (active_) apply_tempo();
}

void PrepMode::apply_tempo() {
  const DeckView a = host_.engine().deck(kDeck[0]);
  const DeckView b = host_.engine().deck(kDeck[1]);
  if (!b.loaded) return;
  if (!tempo_match_) {
    send(Op::Tempo, kDeck[1], 0.0f);
    return;
  }
  const double native_b = slot_[1].facts.bpm;
  if (!a.loaded || a.bpm <= 0 || native_b <= 0) {
    if (a.loaded) note_ = "no tempo to follow";
    return;
  }
  // B follows A's tempo; half or double time counts as the same tempo.
  double best = 1e9, target = a.bpm;
  for (const double t : {a.bpm, a.bpm * 2, a.bpm * 0.5}) {
    const double p = t / native_b - 1;
    if (std::fabs(p) < std::fabs(best)) {
      best = p;
      target = t;
    }
  }
  (void)target;
  const double range = std::fabs(best) <= 0.06 ? 0.06 : std::fabs(best) <= 0.10 ? 0.10 : std::fabs(best) <= 0.16 ? 0.16 : 0.5;
  send(Op::TempoRange, kDeck[1], static_cast<float>(range));
  send(Op::Tempo, kDeck[1], static_cast<float>(std::clamp(best / range, -1.0, 1.0)));
  note_.clear();
}

void PrepMode::apply_blend_commands() {
  // Like a crossfader: both players at full level in the middle, one fading out towards each end (down to -24 dB, then off).
  const double t = std::clamp(blend_, 0.0, 1.0);
  const double ga = std::min(1.0, 2.0 * (1.0 - t)), gb = std::min(1.0, 2.0 * t);
  const auto db = [](double g) { return static_cast<float>(std::clamp(20.0 * std::log10(std::max(g, 1e-3)), -24.0, 0.0)); };
  send(Op::AutoGain, kDeck[0], db(ga));
  send(Op::AutoGain, kDeck[1], db(gb));
  send(Op::Pfl, kDeck[0], 0, ga <= 0.001 ? 0 : 1);
  send(Op::Pfl, kDeck[1], 0, gb <= 0.001 ? 0 : 1);
}

void PrepMode::set_blend(double t) {
  std::lock_guard<std::recursive_mutex> lock(m_);
  if (!active_) return;
  blend_ = std::clamp(t, 0.0, 1.0);
  apply_blend_commands();
}

bool PrepMode::swap_at_next_bar(std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(m_);
  if (!active_) {
    error = "preparation mode is off";
    return false;
  }
  IEngine& e = host_.engine();
  const int from = blend_ < 0.5 ? 0 : blend_ > 0.5 ? 1 : (e.deck(kDeck[0]).playing ? 0 : 1), to = 1 - from;
  const DeckView vf = e.deck(kDeck[from]), vt = e.deck(kDeck[to]);
  if (!vf.loaded || !vt.loaded) {
    error = "both players need a track";
    return false;
  }
  if (!vf.playing) {
    error = "the player in the headphones is not playing";
    return false;
  }
  if (!swap_ids_.empty()) {
    error = "a swap is already waiting for the bar line";
    return false;
  }
  const Schedule when = Schedule::beats_from_now(kDeck[from], 1, true);
  const auto add = [&](Op op, int deck, float f, int i) {
    Command c;
    c.op = op;
    c.deck = static_cast<std::uint8_t>(deck);
    c.f = f;
    c.i = i;
    const std::uint64_t id = e.schedule(c, when);
    if (id) swap_ids_.push_back(id);
  };
  add(Op::AutoGain, kDeck[to], 0.0f, 0);
  add(Op::Pfl, kDeck[to], 0, 1);
  if (!vt.playing) add(Op::PlayToggle, kDeck[to], 0, 0);
  add(Op::Pfl, kDeck[from], 0, 0);  // last: its firing completes the swap
  if (swap_ids_.empty()) {
    error = "the swap could not be scheduled";
    return false;
  }
  swap_fired_ = 0;
  swap_target_ = to == 1 ? 1.0 : 0.0;
  return true;
}

void PrepMode::on_engine_event(const EngineEvent& e) {
  std::lock_guard<std::recursive_mutex> lock(m_);
  if (!active_ || swap_ids_.empty()) return;
  if (e.type != EventType::ScheduledFired && e.type != EventType::ScheduledDropped) return;
  if (std::find(swap_ids_.begin(), swap_ids_.end(), e.sched_id) == swap_ids_.end()) return;
  if (e.type == EventType::ScheduledDropped) {
    for (const std::uint64_t id : swap_ids_) host_.engine().cancel(id);
    swap_ids_.clear();
    note_ = "the swap was dropped";
    apply_blend_commands();
    return;
  }
  if (++swap_fired_ >= static_cast<int>(swap_ids_.size())) {
    swap_ids_.clear();
    blend_ = swap_target_;
  }
}

void PrepMode::tick() {
  std::lock_guard<std::recursive_mutex> lock(m_);
  if (closing_) {
    for (int s = 0; s < 2; ++s)
      if (slot_[s].loading && host_.load_state(kDeck[s], slot_[s].want) != LoadState::Pending) slot_[s].loading = false;
    if (!slot_[0].loading && !slot_[1].loading) restore();
    return;
  }
  if (!active_) return;
  IEngine& e = host_.engine();
  for (int s = 0; s < 2; ++s) {
    const int d = kDeck[s];
    const DeckView v = e.deck(d);
    if (v.fader > 0.001f) {  // the master must never hear these decks (a controller or a key moved the fader)
      send(Op::Fader, d, 0.0f);
      ++guard_hits_;
    }
    Slot& sl = slot_[s];
    if (!sl.loading) continue;
    switch (host_.load_state(d, sl.want)) {
      case LoadState::Ready:
        sl.loading = false;
        apply_blend_commands();
        apply_tempo();
        break;
      case LoadState::Failed:
        sl.loading = false;
        sl.want = 0;
        note_ = "the track could not be loaded";
        break;
      case LoadState::Pending: break;
    }
  }
  if (tempo_match_ && slot_[1].want && !slot_[1].loading) {
    const DeckView a = e.deck(kDeck[0]), b = e.deck(kDeck[1]);
    if (a.loaded && b.loaded && slot_[1].facts.bpm > 0 && a.bpm > 0) {
      // Re-follow when A's own tempo moved (a different track, or its pitch).
      const double have = b.bpm, want = a.bpm;
      if (std::fabs(have - want) > 0.02 && std::fabs(have * 2 - want) > 0.04 && std::fabs(have - want * 2) > 0.04) apply_tempo();
    }
  }
}

PrepStatus PrepMode::status() const {
  std::lock_guard<std::recursive_mutex> lock(m_);
  PrepStatus st;
  st.active = active_;
  st.tempo_match = tempo_match_;
  st.blend = blend_;
  st.swap_armed = !swap_ids_.empty();
  st.guard_hits = guard_hits_;
  st.note = note_;
  if (!active_) return st;
  for (int s = 0; s < 2; ++s) {
    const DeckView v = host_.engine().deck(kDeck[s]);
    st.loading[s] = slot_[s].loading;
    st.loaded[s] = v.loaded;
    st.playing[s] = v.playing;
    st.track[s] = v.loaded ? v.track_id : 0;
    st.position_ms[s] = v.sample_rate > 0 ? v.position / v.sample_rate * 1000.0 : 0;
    st.length_ms[s] = v.sample_rate > 0 ? v.length / v.sample_rate * 1000.0 : 0;
    st.bpm[s] = v.bpm;
    st.native_bpm[s] = slot_[s].facts.bpm;
    st.pitch[s] = v.tempo;
  }
  return st;
}

}  // namespace deckhand
