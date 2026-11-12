#pragma once
// Effect bus (internal). Each effect is one entry in the table in fx.cpp: a name, defaults and a process
// function. A bus holds kFxSlots assignable units plus one colour unit driven by the filter knob.
#include <cstdint>
#include <vector>

#include "deckhand/audio/engine.hpp"

namespace deckhand {

struct FxCtx {
  double sr = 44100;
  double bf = 22050;       // output frames per beat (from the deck or master BPM)
  double beat_pos = 0;     // position in beats at the start of the block
};

struct FxUnit {
  EffectKind kind = EffectKind::None;
  bool on = false, prev_on = false, rise = false;
  float wet = 0.5f, param = 0.5f, beats = 1.0f;
  double seconds = 0;  // > 0: fixed time instead of beats
  double sr = 44100;

  // smoothed values
  float on_s = 0, wet_s = 0, param_s = 0.5f;
  double time_s = -1;  // delay time in frames; < 0 = snap to target on first use
  float env_in = 0;    // input envelope follower
  std::int64_t quiet = 0;

  std::vector<float> mem, aux;  // stereo delay memory and a small second buffer (pitch shifter)
  std::int64_t cap = 0, acap = 0, w = 0, aw = 0;
  double ph = 0, d0 = 0, d1 = 0;
  float s[48] = {};
  std::int32_t n[64] = {};
  std::uint32_t rng = 1;

  void clear();  // zero all state and memory (not real-time safe beyond a memset)
};

struct FxBus {
  FxUnit slot[kFxSlots];
  FxUnit colour;
  double clock = 0;  // free-running beat clock for stopped decks

  void prepare(double sr);  // allocates; not real-time
  void select(int slot_index, EffectKind kind);            // slot_index kFxSlots = colour unit
  void run_slots(float* buf, int n, const FxCtx& cx);       // in place, interleaved stereo
  void run_colour(float* buf, int n, const FxCtx& cx, float knob);
  bool busy() const;
  void view(FxSlotView* out) const;
};

}  // namespace deckhand
