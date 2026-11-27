#pragma once
// Plug-in chain unit (internal): what an FxUnit of kind EffectKind::Plugin owns. The plug-in is opened off the audio
// thread (Engine::chain_edit, in whichever process runs the audio), then the unit travels through the chain swap like any
// other; the audio thread only calls PluginEffect::run, which allocates nothing and takes no lock.
#include <string>

#include "deckhand/audio/engine.hpp"
#include "deckhand/plugins/effect.hpp"

namespace deckhand {

struct FxUnit;
struct FxCtx;

struct PluginSlot {
  plugins::PluginInfo info;
  plugins::PluginEffect eff;
  bool loaded = false;
  std::string error;  // why it did not load (the unit then passes audio through)
  double sr = 0;
};

// Opens `info` for the unit at sample rate `sr` (non-real-time). On failure the slot stays attached, unloaded, with the
// reason in slot->error; the unit then passes audio through. Returns loaded.
bool fx_plugin_attach(FxUnit& u, const plugins::PluginInfo& info, double sr, std::string& error);
// Device rate changed: reopens the plug-in at the new rate keeping its state (non-real-time, device stopped).
void fx_plugin_rate(FxUnit& u, double sr);
// Real-time: runs the unit's plug-in in place.
// The tempo (from cx.bf) and cx.beat_pos are handed to the plug-in as its host tempo and beat position.
void fx_plugin_run(FxUnit& u, const FxCtx& cx, float* buf, int n);
bool fx_plugin_busy(const FxUnit& u);
// JSON for PluginOpKind::Info.
std::string fx_plugin_info_json(const FxUnit& u);
bool fx_plugin_get_state(FxUnit& u, std::string& out, std::string& error);
bool fx_plugin_set_state(FxUnit& u, const std::string& blob, std::string& error);
bool fx_plugin_set_param(FxUnit& u, int index, double norm);

}  // namespace deckhand
