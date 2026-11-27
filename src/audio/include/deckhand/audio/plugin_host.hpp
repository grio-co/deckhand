#pragma once
// Where the audio process finds, vets and records third-party plug-ins. The engine (in whichever process runs the audio)
// and the app share one folder: the scan cache, the blocklist and the list of plug-ins that passed an out-of-process
// probe. Nothing here instantiates a plug-in in this process.
#include <string>

#include "deckhand/plugins/plugin.hpp"

namespace deckhand {

// Folder for plugins.cache, plugins.cache.blocklist and plugins.probed (created on demand). Normally <data dir>/plugins;
// the engine helper sets it from --data-dir, the app from its data dir. Unset: plug-ins cannot be loaded.
void plugin_host_set_dir(const std::string& dir);
std::string plugin_host_dir();
// The probe helper executable. Default: deckhand-plugin-probe next to the running executable (the DECKHAND_PLUGIN_PROBE
// environment variable overrides it, for tests).
void plugin_host_set_probe_exe(const std::string& exe);
std::string plugin_host_probe_exe();

// Resolves `id` to a scanned plug-in and makes sure it is safe to load: refuses blocklisted ids, and before the first
// load of an id/version probes it in a throw-away process (open, render one block, close). A failing plug-in goes on the
// blocklist and the reason comes back in `error`. A pass is remembered in plugins.probed (so the app can probe ahead of time
// and the engine does not repeat it). May take seconds the first time; never call it from the audio thread.
bool plugin_host_vet(const std::string& id, plugins::PluginInfo& out, std::string& error);
// Same checks without loading: true when the id is known, not blocked, and already probed OK.
bool plugin_host_is_vetted(const std::string& id);

}  // namespace deckhand
