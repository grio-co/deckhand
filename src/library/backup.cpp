#include "deckhand/library/backup.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include "deckhand/library/db.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand {
namespace {

namespace fs = std::filesystem;

fs::path utf8_path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

// UTC, sortable: 20261125-231500-123
std::string stamp() {
  using namespace std::chrono;
  const auto now = floor<milliseconds>(system_clock::now());
  const auto day = floor<days>(now);
  const year_month_day ymd{day};
  const hh_mm_ss<milliseconds> t{now - day};
  char out[48];
  std::snprintf(out, sizeof out, "%04d%02u%02u-%02d%02d%02d-%03d", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()), static_cast<int>(t.hours().count()),
                static_cast<int>(t.minutes().count()), static_cast<int>(t.seconds().count()),
                static_cast<int>(t.subseconds().count()));
  return out;
}

// Throws unless the file is a sound DeckHand library; fills in what it holds.
LibraryBackup inspect(const std::string& path) {
  if (!fs::is_regular_file(utf8_path(path))) throw std::runtime_error("no such file: " + path);
  Db db;
  db.open(path);
  {
    Stmt s(db, "PRAGMA quick_check");
    if (!s.step() || s.text(0) != "ok") throw std::runtime_error("damaged library: " + path);
  }
  LibraryBackup b;
  b.path = path;
  {
    Stmt s(db, "SELECT max(version) FROM schema_version");
    if (s.step()) b.schema_version = static_cast<int>(s.i64(0));
  }
  if (b.schema_version < 1) throw std::runtime_error("not a DeckHand library: " + path);
  Stmt s(db, "SELECT count(*) FROM tracks");
  if (s.step()) b.tracks = s.i64(0);
  return b;
}

// SQLite's online backup: a consistent copy that includes the write-ahead log, written in one transaction.
void copy_database(Db& from, Db& to) {
  sqlite3_backup* b = sqlite3_backup_init(to.handle(), "main", from.handle(), "main");
  if (!b) throw std::runtime_error(std::string("cannot copy library: ") + sqlite3_errmsg(to.handle()));
  int rc = SQLITE_OK;
  for (int tries = 0; tries < 200; ++tries) {
    rc = sqlite3_backup_step(b, -1);
    if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  sqlite3_backup_finish(b);
  if (rc != SQLITE_DONE) throw std::runtime_error(std::string("cannot copy library: ") + sqlite3_errstr(rc));
}

std::string reason_of(const std::string& name) {
  for (const char* r : {"upgrade", "before-restore", "manual"})
    if (name.ends_with(std::string("-") + r + ".db")) return r;
  return {};
}

// <dir>/backups/<stem>-<stamp>-<reason>.db
fs::path backup_dir(const fs::path& db) { return db.parent_path() / "backups"; }

std::vector<fs::path> backup_files(const std::string& db_path) {
  const fs::path db = utf8_path(db_path);
  const std::string prefix = utf8(db.stem()) + "-";
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::directory_iterator it(backup_dir(db), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = utf8(it->path().filename());
    if (name.starts_with(prefix) && !reason_of(name).empty()) out.push_back(it->path());
  }
  std::sort(out.rbegin(), out.rend());  // the stamp makes names sort by age
  return out;
}

}  // namespace

std::string backup_library(const std::string& db_path, const std::string& reason) {
  const fs::path db = utf8_path(db_path);
  if (!fs::is_regular_file(db)) throw std::runtime_error("no library at " + db_path);
  fs::create_directories(backup_dir(db));
  fs::path dst;
  do {
    dst = backup_dir(db) / utf8_path(utf8(db.stem()) + "-" + stamp() + "-" + reason + ".db");
  } while (fs::exists(dst));
  const fs::path part = utf8_path(utf8(dst) + ".part");
  try {
    fs::remove(part);
    {
      Db from, to;
      from.open(db_path);
      to.open(utf8(part));
      copy_database(from, to);
      to.exec("PRAGMA journal_mode=DELETE");  // one self-contained file
    }
    inspect(utf8(part));
    fs::rename(part, dst);
  } catch (...) {
    std::error_code ec;
    fs::remove(part, ec);
    throw;
  }
  if (reason == "upgrade") {
    int seen = 0;
    for (const auto& f : backup_files(db_path)) {
      if (reason_of(utf8(f.filename())) != "upgrade" || ++seen <= kUpgradeBackupsKept) continue;
      std::error_code ec;
      fs::remove(f, ec);
    }
  }
  return utf8(dst);
}

std::vector<LibraryBackup> library_backups(const std::string& db_path) {
  std::vector<LibraryBackup> out;
  for (const auto& f : backup_files(db_path)) {
    try {
      LibraryBackup b = inspect(utf8(f));
      b.reason = reason_of(utf8(f.filename()));
      out.push_back(std::move(b));
    } catch (const std::exception& e) {
      DH_LOG(Warn, "skipping backup: %s", e.what());
    }
  }
  return out;
}

std::string restore_library(const std::string& db_path, const std::string& backup_path) {
  const LibraryBackup src = inspect(backup_path);
  if (src.schema_version > kLibrarySchemaVersion)
    throw std::runtime_error("that copy was made by a newer DeckHand (schema " + std::to_string(src.schema_version) + ")");
  const fs::path db = utf8_path(db_path);
  if (fs::exists(db) && fs::equivalent(db, utf8_path(backup_path)))
    throw std::runtime_error("that file is the library itself");

  std::string saved;
  bool damaged = false;
  if (fs::exists(db)) {
    try {
      inspect(db_path);
    } catch (const std::exception& e) {
      const std::string what = e.what();
      if (what.find("locked") != std::string::npos) throw std::runtime_error("the library is in use: quit DeckHand first");
      damaged = true;
    }
    if (!damaged) {
      // Leaving write-ahead mode needs the only connection to the file: this is how "in use" is detected.
      bool alone = false;
      try {
        Db probe;
        probe.open(db_path);
        sqlite3_busy_timeout(probe.handle(), 200);
        Stmt s(probe, "PRAGMA journal_mode=DELETE");
        alone = s.step() && s.text(0) == "delete";
      } catch (const std::exception&) {
      }
      if (!alone) throw std::runtime_error("the library is in use: quit DeckHand first");
      saved = backup_library(db_path, "before-restore");
    } else {
      saved = db_path + ".damaged-" + stamp();
      for (const char* suffix : {"", "-wal", "-shm"}) {
        const fs::path from = utf8_path(db_path + suffix);
        if (fs::exists(from)) fs::rename(from, utf8_path(saved + suffix));
      }
    }
  }
  Db from, to;
  from.open(backup_path);
  to.open(db_path);
  copy_database(from, to);
  DH_LOG(Info, "library restored from %s", backup_path.c_str());
  return saved;
}

}  // namespace deckhand
