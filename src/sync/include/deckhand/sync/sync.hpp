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
  // Non-empty: encrypt this device's files with a key derived from it, and accept only encrypted files from others.
  // It is never written to the folder or the database by the engine.
  std::string passphrase;
  // Compaction drops the details of anything deleted longer ago than this (the deletion itself is kept).
  // Negative: never.
  std::int64_t tombstone_retention_ms = 180ll * 24 * 3600 * 1000;
  int compact_after_files = 64;  // sync_once compacts once this device has more files than this; 0 = never

  // ---- linked mix points ----
  bool sync_mix_links = true;  // linked mix points travel with the library (docs/SYNC.md, "Linked mix points")

  // ---- analysis data (phrases, vocal regions, analysis cue points) ----
  // Off: nothing is written, and what other devices wrote is kept but not applied. Beat grid, key, BPM and gain always
  // travel with the track metadata and are not affected by this switch.
  bool sync_analysis = false;

  // ---- audio files (SyncEngine::sync_audio) ----
  std::vector<std::int64_t> audio_playlists;  // local playlist (or folder) ids whose tracks' audio this device publishes
  std::string music_folder;                   // where audio from other devices is imported; empty = do not import
  std::uint64_t audio_budget_bytes = 0;       // most the audio store in the sync folder may hold; 0 = no limit
  std::uint64_t audio_import_budget_bytes = 0;  // most one sync_audio() call copies into music_folder; 0 = no limit
  // With a passphrase set, audio is encrypted the same way as the logs. A build without a crypto library cannot do that:
  // sync_audio() then refuses unless this is true, in which case audio is stored and accepted unencrypted.
  bool allow_unencrypted_audio = false;
};

// What one SyncEngine::sync_audio() call did. Counts are files; bytes are audio bytes (not container overhead).
struct AudioReport {
  bool ok = false;
  std::string error;           // set when ok is false (nothing was copied)
  int exported = 0;            // files copied into the sync folder
  std::uint64_t exported_bytes = 0;
  int already_stored = 0;      // selected files whose audio was already in the folder (de-duplicated)
  int imported = 0;            // files copied into music_folder and linked to their track
  std::uint64_t imported_bytes = 0;
  int relinked = 0;            // missing tracks relinked to a file that was already on this computer (nothing copied)
  int skipped = 0;             // selected or wanted but not possible: file missing locally, unreadable, no file name
  int waiting_budget = 0;      // left for later because a size budget would be exceeded
  int waiting_files = 0;       // other devices' audio that has not arrived (or is incomplete) in the folder yet
  int failed = 0;              // arrived damaged (hash mismatch, cut off, wrong passphrase) and was not used
  std::uint64_t store_bytes = 0;  // audio store size in the folder after this call
  std::vector<std::string> warnings;
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
  int files_skipped = 0;      // other devices' files not usable with this passphrase (or lack of one)
  int records_pruned = 0;     // details of long-deleted items dropped by compaction
  int links_pending = 0;      // linked mix points known here whose track is not in this library yet
  int analysis_applied = 0;   // analysis items (phrases, vocals, cue points) written into the library from other devices
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
  // Audio files for the playlists in SyncConfig::audio_playlists (publish) and for what other devices published (import).
  // Resumable: run it as often as you like; finished files are skipped, a cut-off copy is redone. Run after sync_once().
  AudioReport sync_audio();

  Status status();
  std::vector<DeviceInfo> devices();

  // False in builds without a crypto library: a passphrase is then an error.
  static bool encryption_available();

  // Content fingerprint: size plus a hash of three 64 KiB windows (whole file when small). "" if unreadable.
  static std::string fingerprint_file(const std::string& path);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deckhand::sync
