// The encrypted SQLite database (stage 2 §4.3): SQLite3MultipleCiphers with
// its SQLCipher v4-compatible cipher, keyed by 32 random bytes from the
// secret store, WAL, foreign keys on. A thin RAII layer: Db owns the
// connection, Stmt a prepared statement, Txn a transaction that rolls back
// unless committed. Every call returns std::expected; nothing throws.
//
// A Db is used by one thread at a time (the store's writer thread, or one
// reader each); SQLite is built threadsafe, so separate connections to the
// same file are fine.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace micnote::store {

inline constexpr std::size_t kDbKeySize = 32;
using DbKey = std::array<std::byte, kDbKeySize>;

struct DbError {
  // SQLite's extended result code (SQLITE_NOTADB for a wrong key, ...).
  int code = 0;
  std::string message;
};

template <typename T>
using DbResult = std::expected<T, DbError>;

class Stmt {
 public:
  Stmt(const Stmt &) = delete;
  Stmt &operator=(const Stmt &) = delete;
  Stmt(Stmt &&) noexcept = default;
  Stmt &operator=(Stmt &&) noexcept = default;
  ~Stmt() = default;

  // Bind by 1-based index. A failed bind (a bad index) is reported by the
  // next Step()/Run().
  Stmt &Bind(int idx, std::int64_t v);
  Stmt &Bind(int idx, double v);
  Stmt &Bind(int idx, std::string_view v);
  Stmt &Bind(int idx, std::span<const std::byte> v);
  Stmt &BindNull(int idx);
  // Binds v, or NULL when it's empty.
  Stmt &Bind(int idx, std::optional<std::int64_t> v);

  // true: a row is ready to read; false: done.
  DbResult<bool> Step();
  // Steps to completion, for statements that return no rows.
  DbResult<void> Run();
  // Ready to run again with new bindings.
  void Reset();

  [[nodiscard]] std::int64_t Int(int col) const;
  [[nodiscard]] std::optional<std::int64_t> OptInt(int col) const;
  [[nodiscard]] double Real(int col) const;
  [[nodiscard]] std::string Text(int col) const;
  [[nodiscard]] std::vector<std::byte> Blob(int col) const;
  [[nodiscard]] bool IsNull(int col) const;

 private:
  friend class Db;
  Stmt(sqlite3_stmt *stmt, sqlite3 *db);
  void Note(int rc);
  struct Finalizer {
    void operator()(sqlite3_stmt *s) const;
  };
  std::unique_ptr<sqlite3_stmt, Finalizer> stmt_;
  sqlite3 *db_ = nullptr;
  int bind_error_ = 0;
};

class Db {
 public:
  // Opens (creating if need be) the database at path with key. A wrong key
  // or a file that isn't an encrypted Micnote database gives SQLITE_NOTADB.
  static DbResult<Db> Open(const std::filesystem::path &path, const DbKey &key);

  Db(const Db &) = delete;
  Db &operator=(const Db &) = delete;
  Db(Db &&) noexcept = default;
  Db &operator=(Db &&) noexcept = default;
  ~Db() = default;

  // Runs one or more statements that return no rows.
  DbResult<void> Exec(std::string_view sql);
  DbResult<Stmt> Prepare(std::string_view sql);

  [[nodiscard]] std::int64_t LastInsertRowid() const;
  [[nodiscard]] int Changes() const;

  // Test hook (fault injection): the `calls`-th database call from now on
  // this thread (Exec, Prepare or Step) fails with SQLITE_IOERR; 0 turns it
  // off. Tests sweep it to drive every error path.
  static void FailAfter(int calls);
  // True while the fault FailAfter() armed hasn't fired yet.
  static bool FaultPending();

 private:
  explicit Db(sqlite3 *db);
  struct Closer {
    void operator()(sqlite3 *db) const;
  };
  std::unique_ptr<sqlite3, Closer> db_;
};

// BEGIN IMMEDIATE ... COMMIT; rolls back when destroyed uncommitted.
class Txn {
 public:
  static DbResult<Txn> Begin(Db &db);
  DbResult<void> Commit();

  Txn(const Txn &) = delete;
  Txn &operator=(const Txn &) = delete;
  Txn(Txn &&other) noexcept : db_(other.db_) { other.db_ = nullptr; }
  Txn &operator=(Txn &&) = delete;
  ~Txn();

 private:
  explicit Txn(Db &db) : db_(&db) {}
  Db *db_;
};

}  // namespace micnote::store
