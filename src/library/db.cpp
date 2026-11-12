#include "deckhand/library/db.hpp"

#include <sqlite3.h>

#include <stdexcept>

namespace deckhand {

Db::~Db() { close(); }

void Db::open(const std::string& path) {
  close();
  if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
    close();
    throw std::runtime_error("cannot open database " + path + ": " + msg);
  }
  sqlite3_busy_timeout(db_, 5000);
}

void Db::close() {
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
}

void Db::exec(const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "unknown error";
    sqlite3_free(err);
    throw std::runtime_error("sql error: " + msg);
  }
}

std::int64_t Db::last_insert_id() const { return sqlite3_last_insert_rowid(db_); }

Stmt::Stmt(Db& db, const char* sql) : db_(db) {
  if (sqlite3_prepare_v2(db.handle(), sql, -1, &st_, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("sql prepare failed: ") + sqlite3_errmsg(db.handle()) + " in: " + sql);
  }
}

Stmt::~Stmt() { sqlite3_finalize(st_); }

Stmt& Stmt::bind(int i, std::int64_t v) {
  sqlite3_bind_int64(st_, i, v);
  return *this;
}
Stmt& Stmt::bind(int i, double v) {
  sqlite3_bind_double(st_, i, v);
  return *this;
}
Stmt& Stmt::bind(int i, std::string_view v) {
  sqlite3_bind_text(st_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
  return *this;
}
Stmt& Stmt::bind_null(int i) {
  sqlite3_bind_null(st_, i);
  return *this;
}

bool Stmt::step() {
  const int rc = sqlite3_step(st_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  throw std::runtime_error(std::string("sql step failed: ") + sqlite3_errmsg(db_.handle()));
}

void Stmt::reset() {
  sqlite3_reset(st_);
  sqlite3_clear_bindings(st_);
}

std::int64_t Stmt::i64(int col) const { return sqlite3_column_int64(st_, col); }
double Stmt::f64(int col) const { return sqlite3_column_double(st_, col); }
std::string Stmt::text(int col) const {
  const unsigned char* t = sqlite3_column_text(st_, col);
  return t ? std::string(reinterpret_cast<const char*>(t), static_cast<std::size_t>(sqlite3_column_bytes(st_, col)))
           : std::string();
}
bool Stmt::is_null(int col) const { return sqlite3_column_type(st_, col) == SQLITE_NULL; }

}  // namespace deckhand
