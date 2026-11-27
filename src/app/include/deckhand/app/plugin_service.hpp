#pragma once
// The application's view of installed effect plug-ins: the scanned list, which ones passed the out-of-process probe, and a
// background rescan with progress. The scan only reads metadata; every plug-in is probed in a throw-away process
// (deckhand-plugin-probe) before it can be loaded by the engine; failures are blocklisted and reported.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace deckhand {

struct PluginRow {
  std::string id, name, maker, format;
  bool blocked = false;
  bool vetted = false;  // passed the probe
  std::string reason;   // why it is blocked
};
struct PluginScanStatus {
  bool supported = false;  // this build can host plug-ins at all
  bool running = false;
  int done = 0, total = 0;
  std::string current;      // plug-in being probed
  int blocked_now = 0;      // blocked during the last run
  std::string last_message; // "3 plug-ins, 1 blocked"
  std::uint64_t generation = 0;  // counts finished runs
};

class PluginService {
 public:
  ~PluginService();
  void set_dir(const std::string& data_plugins_dir);  // also tells the engine's plug-in host (plugin_host_set_dir)
  std::vector<PluginRow> list();                      // from the cache (a first call without one starts a scan)
  void rescan(bool force = true);                     // background: scan, then probe what has not been probed; no-op while running
  PluginScanStatus status() const;
  void wait();                                        // joins the background run (tests)
  // Probes one plug-in now (blocking, seconds at most): true when it may be loaded.
  bool vet(const std::string& id, std::string& error);

 private:
  void run(bool force);
  mutable std::mutex mx_;
  std::string dir_;
  std::thread thread_;
  PluginScanStatus st_;
};

PluginService& plugin_service();  // the process-wide instance

}  // namespace deckhand
