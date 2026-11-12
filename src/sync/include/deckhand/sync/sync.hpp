#pragma once
// Serverless library sync. Each device appends its library changes to its own log files inside a folder that the
// user's own cloud service (or a USB stick) replicates; every device reads the others' logs and merges them.
// The SQLite database itself is never placed in the folder. See docs/SYNC.md.
//
// No threads inside: call from a worker. While a call runs, nothing else may use the same Library connection.
// No exceptions escape the public API; failures come back in SyncResult::error.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"

namespace deckhand::sync {

struct SyncConfig {
  std::string folder;       // the replicated folder; DeckHandSync/v1/ is created inside it
  std::string device_name;  // shown to the other devices ("Studio Mac")

  // Optional overrides, mostly for tests.
  std::function<std::int64_t()> clock;                               // unix ms; default system clock
  std::function<std::string(const std::string& abs_path)> fingerprint;  // "" = unreadable; default content fingerprint
  std::function<bool(const char* stage)> fault;                      // return true to simulate a crash at a stage
  std::vector<std::string> settings_allow;                           // key prefixes that sync; see DefaultSettingsAllow
  std::size_t max_log_bytes = 1u << 20;                              // close a log file once it reaches this size
  bool trust_path_hints = true;  // relink a missing track when the other device's file path also exists here
};

struct SyncResult {
  bool ok = false;
  std::string error;  // set when ok is false
  int changes_captured = 0;   // local field/entity changes found by the diff
  int records_written = 0;
  int files_written = 0;
  int files_read = 0;
  int files_corrupt = 0;      // read but incomplete or damaged (valid records are still used)
  int records_applied = 0;    // remote records that changed local state
  int tracks_missing = 0;     // rows whose audio file is not on this device
  std::vector<std::string> warnings;
};

struct Status {
  std::string device_id, device_name, folder;
  int pending_records = 0;        // captured but not yet written to a log file
  std::int64_t last_sync_ms = 0;  // 0 = never
  std::string last_error;
  int tracks_missing = 0;
};

struct DeviceInfo {
  std::string id, name;
  bool is_self = false;
  std::uint64_t last_seq = 0;     // newest file sequence number
  int files = 0;
  std::int64_t last_write_ms = 0;  // creation time of the newest file (that device's clock)
};

// Settings key prefixes that sync by default. Keys containing token, password, secret, api, audio, midi, device
// or path never sync, whatever the allowlist says.
std::vector<std::string> DefaultSettingsAllow();

class SyncEngine {
 public:
  SyncEngine(Library& lib, SyncConfig cfg);
  ~SyncEngine();
  SyncEngine(const SyncEngine&) = delete;
  SyncEngine& operator=(const SyncEngine&) = delete;

  // capture -> write -> read -> apply. Safe to repeat; a repeat with nothing new changes nothing.
  SyncResult sync_once();
  // Only the first step: record local changes as pending (they are written by the next sync_once).
  SyncResult capture();
  // Write a snapshot of this device's state and delete its older log files.
  SyncResult compact();
  // Point "missing file" rows at files found under these folders (matched by content fingerprint).
  int relink_missing(const std::vector<std::string>& search_dirs);

  Status status();
  std::vector<DeviceInfo> devices();

  // Content fingerprint: size plus a hash of three 64 KiB windows (whole file when small). "" if unreadable.
  static std::string fingerprint_file(const std::string& path);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deckhand::sync
