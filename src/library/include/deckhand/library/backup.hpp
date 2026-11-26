#pragma once
// Safety copies of DeckHand's library. A copy is taken automatically before a schema upgrade; copies can also be
// made and restored by hand (deckhandctl backup | backups | restore). Errors throw std::runtime_error.
#include <cstdint>
#include <string>
#include <vector>

namespace deckhand {

// The schema this build writes. Bump it together with Library::migrate().
constexpr int kLibrarySchemaVersion = 8;
// Automatic pre-upgrade copies kept; older ones are removed. Manual and pre-restore copies are never removed.
constexpr int kUpgradeBackupsKept = 5;

struct LibraryBackup {
  std::string path;
  std::string reason;  // "upgrade", "manual" or "before-restore"
  int schema_version = 0;
  std::int64_t tracks = 0;
};

// Copies the library (including changes still in its write-ahead log) into `backups/` beside it, checks the copy and
// returns its path. Safe while the library is open.
std::string backup_library(const std::string& db_path, const std::string& reason = "manual");
// The copies beside this library, newest first. Files that are not readable libraries are skipped.
std::vector<LibraryBackup> library_backups(const std::string& db_path);
// Replaces the library with a copy. Nothing may have the library open (quit DeckHand first). The replaced library is
// saved first; returns where ("" if there was no library). A damaged library is renamed aside instead.
std::string restore_library(const std::string& db_path, const std::string& backup_path);

}  // namespace deckhand
