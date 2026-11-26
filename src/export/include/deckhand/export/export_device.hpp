#pragma once
// Device library backup, restore and the pre-gig check for a stick written by export_usb.
// Only the library folders the exporter writes are ever touched: PIONEER/rekordbox/export.pdb, exportExt.pdb,
// PIONEER/USBANLZ/** and (optionally) the audio files the database refers to. Nothing is formatted, and nothing
// outside those paths is deleted. See docs/EXPORT.md, "Device backup, restore and check".
#include <cstdint>
#include <string>
#include <vector>

namespace deckhand::usbexport {

struct BackupOptions {
  bool include_audio = false;  // also copy the audio files the database points at
  std::string stamp;           // folder name stamp "YYYYMMDD-HHMMSS"; empty = now (tests set it)
};

struct BackupInfo {
  std::string path;     // backup folder
  std::string name;     // folder name
  std::string created;  // from the manifest, "YYYY-MM-DD HH:MM:SS"
  std::size_t files = 0;
  std::uint64_t bytes = 0;
  bool has_audio = false;
  bool valid = false;   // manifest present and parses (not hash-checked: use verify_device_backup)
};

struct ManifestEntry {
  std::string path;  // relative to the stick root, '/' separated
  std::uint64_t size = 0;
  std::string sha256;
  bool audio = false;
};

// Creates <backup_dir>/DeckHand-device-<stamp>[-n]/ holding manifest.txt and files/<path>. Every file is re-read and
// hashed after the copy. On failure the half-written folder is removed. Returns the folder in `backup_path`.
bool backup_device(const std::string& stick_path, const std::string& backup_dir, const BackupOptions& options,
                   std::string& backup_path, std::string& error);

std::vector<BackupInfo> list_device_backups(const std::string& backup_dir);  // newest first

bool read_backup_manifest(const std::string& backup_path, std::vector<ManifestEntry>& entries, std::string& error);
// Re-hashes every file of the backup against its manifest; problems in plain language.
bool verify_device_backup(const std::string& backup_path, std::vector<std::string>& problems);

struct RestoreOptions {
  bool force = false;          // replace an existing library / differing audio files on the stick
  bool restore_audio = true;   // when the backup holds audio
};

struct RestoreResult {
  std::size_t files = 0;
  std::uint64_t bytes = 0;
  std::size_t removed_stale = 0;  // force only: library files of the old export that are not in the backup
};

// Refuses (error says why, nothing written) when the stick already has a library (export.pdb or analysis files)
// and `force` is off, or when the backup does not verify. Every file is hashed again after it is copied.
bool restore_device(const std::string& backup_path, const std::string& stick_path, const RestoreOptions& options,
                    RestoreResult& result, std::string& error);

struct DeviceProblem {
  bool error = true;  // false = warning
  std::string code;   // stable code, e.g. "audio.missing"
  std::string message;  // plain language
};

struct DeviceReport {
  bool ok = false;  // no errors (warnings allowed)
  std::size_t tracks = 0, playlists = 0, files_checked = 0;
  std::vector<DeviceProblem> problems;
  std::string text() const;     // "This stick looks fine: ..." or one line per problem
  std::string to_json() const;
};

struct VerifyOptions {
  bool check_audio = true;
};

// Pre-gig check: reads the database back with the export parser, validates structure, analysis files and that every
// audio file exists with the recorded size. Never modifies the stick.
bool verify_device(const std::string& stick_path, DeviceReport& report, const VerifyOptions& options = {});

std::string sha256_hex_of_file(const std::string& path, std::string& error);

}  // namespace deckhand::usbexport
