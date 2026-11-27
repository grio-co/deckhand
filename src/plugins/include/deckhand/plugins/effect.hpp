// PluginEffect: a hosted plug-in shaped like one unit in DeckHand's effect chains (interleaved stereo float, in place,
// blocks up to 512 frames, on / wet / parameter 0..1 style, no allocation in run()). Registering it is the
// integrator's job (see docs/PLUGINS.md); nothing in src/audio depends on this module.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "deckhand/plugins/plugin.hpp"

namespace deckhand::plugins {

class PluginEffect {
 public:
  static constexpr int kBlock = 512;  // same as the engine's chain block

  // Opens the plug-in (non-RT, allocates). False with err set on failure.
  bool load(const PluginInfo& info, double sample_rate, std::string* err = nullptr);
  bool loaded() const { return inst_ != nullptr; }
  PluginInstance* instance() { return inst_.get(); }

  // Control (any thread, lock free).
  void set_on(bool on) { on_.store(on, std::memory_order_relaxed); }
  void set_wet(float w) { wet_.store(w < 0 ? 0 : w > 1 ? 1 : w, std::memory_order_relaxed); }  // 0..1
  bool on() const { return on_.load(std::memory_order_relaxed); }
  float wet() const { return wet_.load(std::memory_order_relaxed); }
  // Parameter by position in params(), normalised 0..1 (what FxParam / FxField::Param* style controls would send).
  int param_count() const { return inst_ ? (int)inst_->params().size() : 0; }
  void set_param(int index, float norm);
  float get_param(int index) const;
  // Insert mode (default): out = dry * (1 - m) + fx * m, m = wet * on (smoothed). Send mode: out = dry + fx * m, and
  // the plug-in keeps running with silent input for its tail time after the unit is switched off.
  void set_send_mode(bool s) { send_ = s; }

  // Real-time. In place on interleaved stereo; `frames` may exceed kBlock. Does nothing while idle (off, faded out and
  // any tail finished).
  void run(float* interleaved, int frames);
  bool busy() const;  // on, still fading, or ringing out a tail
  // Host tempo and the beat position at the start of the next run() call (any thread; run() advances it per block).
  void set_transport(double tempo_bpm, double beat, bool playing) {
    tempo_ = tempo_bpm;
    beat_ = beat;
    playing_ = playing;
  }
  void reset();       // clears the plug-in's tails and the smoothing (not RT)

  // Chain persistence: opaque plug-in state plus the unit's own settings.
  bool save_state(std::vector<std::uint8_t>& out);
  bool restore_state(const std::uint8_t* d, std::size_t n);

 private:
  std::unique_ptr<PluginInstance> inst_;
  std::atomic<bool> on_{false};
  std::atomic<float> wet_{1.0f};
  bool send_ = false;
  float m_ = 0;                  // smoothed mix
  long long tail_left_ = 0;      // frames of tail still to render after switching off
  double sr_ = 48000;
  double tempo_ = 0, beat_ = 0;
  bool playing_ = false;
  std::vector<float> in_[2], out_[2];  // allocated in load()
};

}  // namespace deckhand::plugins
