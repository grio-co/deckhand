// Crash isolation. probe_plugin() opens a plug-in, renders one block of a synthetic signal and closes it. A scanner
// calls it in a throw-away helper process (the `deckhand-plugin-probe` executable, or any process that calls
// probe_plugin) via probe_out_of_process(), so a plug-in that crashes or hangs only costs the helper.
#pragma once
#include <string>

#include "deckhand/plugins/catalog.hpp"
#include "deckhand/plugins/plugin.hpp"

namespace deckhand::plugins {

struct ProbeResult {
  bool ok = false;
  std::string error;       // set when !ok ("crashed (signal 11)", "timed out", "output not finite", ...)
  double open_ms = 0, render_ms = 0;
  double out_peak = 0;     // peak of the rendered block
  int params = 0;
  double latency_seconds = 0, tail_seconds = 0;
};

// In this process. Never throws.
ProbeResult probe_plugin(const PluginInfo& info, double sample_rate = 48000, int block = 512);

// In a child process running `probe_exe <id> <rate> <block>`; killed after timeout_ms. POSIX only (others: fails).
ProbeResult probe_out_of_process(const std::string& probe_exe, const std::string& plugin_id, int timeout_ms = 8000,
                                 double sample_rate = 48000, int block = 512);

// Probes every usable plug-in out of process; failures go on the blocklist. Returns how many were blocked.
int probe_catalog(PluginCatalog& cat, const std::string& probe_exe, int timeout_ms = 8000);

// What the helper executable's main() does; returns the process exit code (0 ok) and prints one result line.
int probe_main(int argc, char** argv);

}  // namespace deckhand::plugins
