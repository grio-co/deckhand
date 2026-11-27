#pragma once
// Effect engine (internal). Each effect is one entry in the table in fx.cpp: a name, defaults and a process
// function. FxUnit is one effect instance. A chain (FxChainRT) runs up to kFxChainMax units in series and is edited
// by swapping in a prepared FxChainState at a block boundary; FxBus keeps what is left of the old fixed bus: the
// colour unit driven by the channel filter knob and the free-running beat clock of a stopped deck.
#include <cstdint>
#include <vector>

#include "deckhand/audio/engine.hpp"
#include "deckhand/core/spsc.hpp"

namespace deckhand {

constexpr int kFxBlock = 512;  // largest block a chain processes in one go (the engine's chunk size)

struct PluginSlot;

struct FxCtx {
  double sr = 44100;
  double bf = 22050;       // output frames per beat (from the deck or master BPM)
  double beat_pos = 0;     // position in beats at the start of the block
};

struct FxUnit {
  EffectKind kind = EffectKind::None;
  bool on = false, prev_on = false, rise = false;
  bool hold = false;       // the effect keeps itself engaged (build-up waiting for its release beat)
  float wet = 0.5f, param = 0.5f, beats = 1.0f;
  float param2 = 0.5f, param3 = 0.5f;
  double seconds = 0;  // > 0: fixed time instead of beats
  double sr = 44100;
  int id = 0;          // chain unit id (0 for the colour unit)
  int legacy = -1;     // fixed slot 0..2 this unit stands for, -1 = user unit

  // smoothed values
  float on_s = 0, wet_s = 0, param_s = 0.5f, param2_s = 0.5f, param3_s = 0.5f;
  double time_s = -1;  // delay time in frames; < 0 = snap to target on first use
  float env_in = 0;    // input envelope follower
  std::int64_t quiet = 0;

  std::vector<float> mem, aux;  // stereo delay memory and a small second buffer (pitch shifter)
  std::int64_t cap = 0, acap = 0, w = 0, aw = 0;
  double ph = 0, d0 = 0, d1 = 0;
  float s[48] = {};
  std::int32_t n[64] = {};
  std::uint32_t rng = 1;

  PluginSlot* plug = nullptr;  // kind Plugin: the hosted plug-in (owned; freed with the unit, off the audio thread)

  FxUnit() = default;
  FxUnit(const FxUnit&) = delete;
  FxUnit& operator=(const FxUnit&) = delete;
  ~FxUnit();
  void clear();  // zero all state and memory (not real-time safe beyond a memset)
};

// Allocates (off the audio thread) a unit of `kind` at sample rate `sr` with the effect's default settings.
FxUnit* fx_make_unit(EffectKind kind, double sr, std::uint32_t seed);
// Re-sizes the memory of an existing unit for a new sample rate (device stopped).
void fx_unit_prepare(FxUnit& u, double sr);
bool fx_unit_busy(const FxUnit& u);  // still sounding: on, fading, holding, or ringing out a tail
void fx_unit_info(const FxUnit& u, FxUnitInfo& out);
void fx_unit_set(FxUnit& u, FxField field, double value);

// Immutable list of the units a chain runs, in order. Built off the audio thread; the units are shared with the
// state it replaces so their delay memory survives reordering.
struct FxChainState {
  int count = 0;
  FxUnit* unit[kFxChainMax] = {};
  std::uint32_t rev = 0;
};

struct FxGarbage {
  FxUnit* unit = nullptr;
  FxChainState* state = nullptr;
};
using FxGarbageRing = SpscRing<FxGarbage, 1024>;

// The audio thread's side of one chain. Nothing here allocates or locks.
struct FxChainRT {
  FxChainState* cur = nullptr;
  FxChainState* pending = nullptr;  // installed after a short dip to the dry signal
  int phase = 0;                    // 0 steady, 1 dipped: swap at the start of the next block
  FxUnit* ghost[kFxChainMax] = {};  // removed units still ringing out
  bool bypass = false;
  bool immediate = false;           // swap at once, no dip (unused: stem buses run their chains like any other)
  float wet = 1.0f, wet_s = 1.0f, act = 1.0f;
  std::uint32_t leaked = 0;         // garbage that did not fit the ring (should stay 0)

  void install(FxChainState* next, FxGarbageRing& gar);
  void run(float* buf, int n, const FxCtx& cx, FxGarbageRing& gar);  // interleaved stereo, in place
  bool busy() const;                // needs to keep running even with no input
  bool used() const { return cur || pending; }
  // The chain has something to do with its input right now (units running, an edit waiting, a tail ringing): a stem bus
  // is split out of the shared deck path only while this is true.
  bool wants_audio() const {
    if (pending) return true;
    for (const FxUnit* g : ghost)
      if (g) return true;
    return cur && cur->count > 0 && !(bypass && act <= 0.0f);
  }
  bool active() const { return busy() || (cur && cur->count > 0); }  // has something to run
  FxUnit* find_id(int id) const;
  FxUnit* find_legacy(int slot) const;
  void view(FxChainInfo& out) const;
  void view_slots(FxSlotView* out) const;  // the three fixed slots
  void collect(std::vector<FxUnit*>& units, std::vector<FxChainState*>& states) const;  // everything owned (shutdown)

 private:
  void run_block(float* buf, int n, const FxCtx& cx, FxGarbageRing& gar);
  void do_swap(FxGarbageRing& gar);
  void retire(FxUnit* u, FxChainState* s, FxGarbageRing& gar);
};

// What is left of the old fixed bus.
struct FxBus {
  FxUnit colour;
  double clock = 0;  // free-running beat clock for stopped decks

  void prepare(double sr);  // allocates; not real-time
  void select(int slot_index, EffectKind kind);  // slot_index kFxSlots = colour unit (the only one left)
  void run_colour(float* buf, int n, const FxCtx& cx, float knob);
  bool busy() const;  // colour unit only
};

}  // namespace deckhand
