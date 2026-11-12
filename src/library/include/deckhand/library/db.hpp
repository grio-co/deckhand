#pragma once
// Thin RAII wrapper over SQLite. Errors throw std::runtime_error.
#include <cstdint>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace deckhand {

class Db {
 public:
  Db() = default;
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  void open(const std::string& path);
  void close();
  void exec(const std::string& sql);
  std::int64_t last_insert_id() const;
  sqlite3* handle() const { return db_; }

 private:
  sqlite3* db_ = nullptr;
};

class Stmt {
 public:
  Stmt(Db& db, const char* sql);
  ~Stmt();
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  // Parameters are 1-based, columns 0-based (as in SQLite).
  Stmt& bind(int i, std::int64_t v);
  Stmt& bind(int i, int v) { return bind(i, static_cast<std::int64_t>(v)); }
  Stmt& bind(int i, double v);
  Stmt& bind(int i, std::string_view v);
  Stmt& bind(int i, const char* v) { return bind(i, std::string_view(v)); }
  Stmt& bind_null(int i);

  bool step();   // true while a row is available
  void reset();  // reuse with new bindings
  void run() {   // step to completion and reset
    while (step()) {}
    reset();
  }

  std::int64_t i64(int col) const;
  double f64(int col) const;
  std::string text(int col) const;
  bool is_null(int col) const;

 private:
  Db& db_;
  sqlite3_stmt* st_ = nullptr;
};

// RAII transaction: rolls back unless commit() is called.
class Transaction {
 public:
  explicit Transaction(Db& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
  ~Transaction() {
    if (!done_) {
      try {
        db_.exec("ROLLBACK");
      } catch (...) {
      }
    }
  }
  void commit() {
    db_.exec("COMMIT");
    done_ = true;
  }

 private:
  Db& db_;
  bool done_ = false;
};

}  // namespace deckhand
