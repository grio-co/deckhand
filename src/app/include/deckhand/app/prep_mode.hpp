#pragma once
// Preparation mode: two tracks auditioned against each other in the headphones, without touching the decks that play.
// The engine has ONE pre-listen voice, so this mode borrows decks 3 and 4 (index 2 and 3) while both are empty: their
// channel faders go to zero (nothing reaches the master) and headphone cue goes on. Leaving the mode puts the faders,
// cue buttons, trim offsets and tempo back and unloads what the mode loaded. Refused while either deck holds a track
// or plays, while the host has no headphone output, or while the host names another reason.
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "deckhand/app/automix.hpp"

namespace deckhand {

struct PrepStatus {
  bool active = false;
  bool loading[2] = {false, false};
  bool loaded[2] = {false, false};
  bool playing[2] = {false, false};
  std::int64_t track[2] = {0, 0};
  double position_ms[2] = {0, 0}, length_ms[2] = {0, 0};
  double bpm[2] = {0, 0};          // effective tempo of each deck now
  double native_bpm[2] = {0, 0};   // the track's own
  double pitch[2] = {0, 0};        // tempo offset applied (fraction)
  bool tempo_match = true;         // B follows A's tempo
  double blend = 0.5;              // 0 = only A in the headphones, 0.5 = both at full level, 1 = only B
  bool swap_armed = false;         // a swap at the next bar line is scheduled
  int guard_hits = 0;              // times a fader had to be pulled back to zero
  std::string note;                // last problem (load failed, no tempo ...)
};

class PrepMode {
 public:
  static constexpr int kDeck[2] = {2, 3};  // slot A and B

  // `host` gives the engine, track facts and loading (the same adapter automix uses).
  explicit PrepMode(AutomixHost& host);
  ~PrepMode();

  // Extra reasons to refuse (e.g. "automix is running"): return empty to allow. Default: none.
  void set_blocker(std::function<std::string()> blocker) { blocker_ = std::move(blocker); }
  // Whether the engine has a headphone output; default: four or more output channels.
  void set_cue_output_check(std::function<bool()> check) { cue_check_ = std::move(check); }

  // Enters the mode. On refusal returns false, `refusal` is one of busy_deck | no_cue_output | blocked, `message` English text.
  bool enter(std::string& refusal, std::string& message);
  void exit();
  bool active() const {
    std::lock_guard<std::recursive_mutex> lock(m_);
    return active_;
  }

  bool load(int slot, std::int64_t track_id, std::string& error);
  void play(int slot, int mode);              // 1 play, 0 pause, -1 toggle
  void cue(int slot, bool down);              // the deck's cue button
  void seek(int slot, double ms);
  void set_tempo_match(bool on);
  void set_blend(double t);                   // 0..1 like a crossfader; the ends leave one player alone
  bool swap_at_next_bar(std::string& error);  // the deck in the headphones hands over to the other one at its next bar line

  void tick();                                // every 10 to 50 ms (loads finish, the faders are held, tempo follows)
  void on_engine_event(const EngineEvent& e);
  PrepStatus status() const;

 private:
  void send(Op op, int deck, float f = 0, int i = 0, double d = 0);
  void restore();
  void apply_tempo();
  void apply_blend_commands();

  mutable std::recursive_mutex m_;  // the interface thread and the engine-event thread both call in
  AutomixHost& host_;
  std::function<std::string()> blocker_;
  std::function<bool()> cue_check_;
  bool active_ = false, closing_ = false;
  struct Slot {
    std::int64_t want = 0;
    bool loading = false;
    TrackFacts facts;
  } slot_[2];
  struct Saved {
    float fader = 1, trim_db = 0, tempo = 0, tempo_range = 0.06f;
    bool pfl = false;
  } saved_[2];
  bool tempo_match_ = true;
  double blend_ = 0.5;
  std::vector<std::uint64_t> swap_ids_;
  int swap_fired_ = 0;
  double swap_target_ = 0;
  int guard_hits_ = 0;
  std::string note_;
};

}  // namespace deckhand
