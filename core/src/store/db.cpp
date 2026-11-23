#include "store/db.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <sqlite3.h>

namespace micnote::store {

namespace {

DbError Error(sqlite3 *db, int code) {
  return DbError{.code = code,
                 .message = db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(code)};
}

std::string HexKey(const DbKey &key) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  // SQLCipher's raw-key syntax: x'<64 hex digits>', no key derivation.
  std::string s = "x'";
  for (const std::byte b : key) {
    const auto v = std::to_integer<unsigned>(b);
    s += kDigits.at(v >> 4U);
    s += kDigits.at(v & 0xFU);
  }
  s += '\'';
  return s;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup of `s`

thread_local int fault_countdown = 0;

// True when this call is the one FailAfter() picked.
bool Faulted() {
  if (fault_countdown <= 0) {
    return false;
  }
  --fault_countdown;
  return fault_countdown == 0;
}

DbError Injected() { return DbError{.code = SQLITE_IOERR, .message = "injected fault"}; }

// SQLite takes int lengths. Nothing the store binds comes near 2 GB; past
// that a bind fails with SQLITE_TOOBIG (a negative length would mean "up to
// the NUL" to SQLite, so it's never passed).
constexpr std::size_t kMaxBind = static_cast<std::size_t>(std::numeric_limits<int>::max());

}  // namespace

// ---- Stmt ------------------------------------------------------------------

void Stmt::Finalizer::operator()(sqlite3_stmt *s) const { sqlite3_finalize(s); }

Stmt::Stmt(sqlite3_stmt *stmt, sqlite3 *db) : stmt_(stmt), db_(db) {}

void Stmt::Note(int rc) {
  if (bind_error_ == SQLITE_OK) {
    bind_error_ = rc;  // the first failure is the one reported
  }
}

Stmt &Stmt::Bind(int idx, std::int64_t v) {
  const int rc = sqlite3_bind_int64(stmt_.get(), idx, v);
  Note(rc);
  return *this;
}

Stmt &Stmt::Bind(int idx, double v) {
  const int rc = sqlite3_bind_double(stmt_.get(), idx, v);
  Note(rc);
  return *this;
}

Stmt &Stmt::Bind(int idx, std::string_view v) {
  // GCOVR_EXCL_START: a 2 GB string can't be made in a test.
  if (v.size() > kMaxBind) {
    Note(SQLITE_TOOBIG);
    return *this;
  }
  // GCOVR_EXCL_STOP
  const int rc =
      sqlite3_bind_text(stmt_.get(), idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
  Note(rc);
  return *this;
}

Stmt &Stmt::Bind(int idx, std::span<const std::byte> v) {
  // GCOVR_EXCL_START: as above.
  if (v.size() > kMaxBind) {
    Note(SQLITE_TOOBIG);
    return *this;
  }
  // GCOVR_EXCL_STOP
  // A zero-length blob, not NULL, even when v.data() is null.
  const int rc = v.empty() ? sqlite3_bind_zeroblob(stmt_.get(), idx, 0)
                           : sqlite3_bind_blob(stmt_.get(), idx, v.data(),
                                               static_cast<int>(v.size()), SQLITE_TRANSIENT);
  Note(rc);
  return *this;
}

Stmt &Stmt::BindNull(int idx) {
  const int rc = sqlite3_bind_null(stmt_.get(), idx);
  Note(rc);
  return *this;
}

Stmt &Stmt::Bind(int idx, std::optional<std::int64_t> v) {
  return v ? Bind(idx, *v) : BindNull(idx);
}

DbResult<bool> Stmt::Step() {
  if (Faulted()) {
    return std::unexpected(Injected());
  }
  if (bind_error_ != SQLITE_OK) {
    return std::unexpected(Error(nullptr, bind_error_));
  }
  const int rc = sqlite3_step(stmt_.get());
  if (rc == SQLITE_ROW) {
    return true;
  }
  if (rc == SQLITE_DONE) {
    return false;
  }
  return std::unexpected(Error(db_, sqlite3_extended_errcode(db_)));
}

DbResult<void> Stmt::Run() {
  for (;;) {
    auto row = Step();
    if (!row) {
      return std::unexpected(row.error());
    }
    if (!*row) {
      return {};
    }
  }
}

void Stmt::Reset() {
  sqlite3_reset(stmt_.get());
  sqlite3_clear_bindings(stmt_.get());
  bind_error_ = SQLITE_OK;
}

std::int64_t Stmt::Int(int col) const { return sqlite3_column_int64(stmt_.get(), col); }

std::optional<std::int64_t> Stmt::OptInt(int col) const {
  return IsNull(col) ? std::nullopt : std::optional(Int(col));
}

double Stmt::Real(int col) const { return sqlite3_column_double(stmt_.get(), col); }

std::string Stmt::Text(int col) const {
  const auto *p = sqlite3_column_text(stmt_.get(), col);
  const int n = sqlite3_column_bytes(stmt_.get(), col);
  // GCOVR_EXCL_BR_START: both arms run; the rest are gcc's exception-cleanup
  // branches for the temporaries.
  return p != nullptr ? std::string(reinterpret_cast<const char *>(p), static_cast<std::size_t>(n))
                      : std::string();
  // GCOVR_EXCL_BR_STOP
}

std::vector<std::byte> Stmt::Blob(int col) const {
  const auto *p = static_cast<const std::byte *>(sqlite3_column_blob(stmt_.get(), col));
  const int n = sqlite3_column_bytes(stmt_.get(), col);
  // GCOVR_EXCL_BR_START: as in Text().
  return p != nullptr ? std::vector<std::byte>(p, p + n) : std::vector<std::byte>();
  // GCOVR_EXCL_BR_STOP
}

bool Stmt::IsNull(int col) const { return sqlite3_column_type(stmt_.get(), col) == SQLITE_NULL; }

// ---- Db --------------------------------------------------------------------

void Db::Closer::operator()(sqlite3 *db) const { sqlite3_close_v2(db); }

Db::Db(sqlite3 *db) : db_(db) {}

DbResult<Db> Db::Open(const std::filesystem::path &path, const DbKey &key) {
  sqlite3 *raw = nullptr;
  const std::string utf8 = path.string();
  const int rc =
      sqlite3_open_v2(utf8.c_str(), &raw,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
  Db db(raw);  // owns raw even when the open failed
  if (rc != SQLITE_OK) {
    return std::unexpected(Error(raw, rc));
  }
  sqlite3_extended_result_codes(raw, 1);
  // SQLCipher v4's settings (legacy=4), then the raw key; the first read
  // after that is what fails on a wrong key.
  std::string setup = "PRAGMA cipher = 'sqlcipher'; PRAGMA legacy = 4; PRAGMA key = \"";
  setup += HexKey(key);
  setup += "\";";
  auto keyed = db.Exec(setup);
  std::ranges::fill(setup, '\0');
  if (!keyed) {
    return std::unexpected(keyed.error());
  }
  if (auto checked = db.Exec("SELECT count(*) FROM sqlite_master;"); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto ok = db.Exec("PRAGMA journal_mode = WAL; PRAGMA foreign_keys = ON;"
                        "PRAGMA busy_timeout = 5000; PRAGMA secure_delete = ON;");
      !ok) {
    return std::unexpected(ok.error());
  }
  return db;
}

void Db::FailAfter(int calls) { fault_countdown = calls; }

bool Db::FaultPending() { return fault_countdown > 0; }

DbResult<void> Db::Exec(std::string_view sql) {
  if (Faulted()) {
    return std::unexpected(Injected());
  }
  const std::string s(sql);
  const int rc = sqlite3_exec(db_.get(), s.c_str(), nullptr, nullptr, nullptr);
  if (rc != SQLITE_OK) {
    return std::unexpected(Error(db_.get(), sqlite3_extended_errcode(db_.get())));
  }
  return {};
}

DbResult<Stmt> Db::Prepare(std::string_view sql) {
  if (Faulted()) {
    return std::unexpected(Injected());
  }
  sqlite3_stmt *stmt = nullptr;
  // The store's SQL is its own, and short.
  const int rc =
      sqlite3_prepare_v2(db_.get(), sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::unexpected(Error(db_.get(), sqlite3_extended_errcode(db_.get())));
  }
  return Stmt(stmt, db_.get());
}

std::int64_t Db::LastInsertRowid() const { return sqlite3_last_insert_rowid(db_.get()); }

int Db::Changes() const { return sqlite3_changes(db_.get()); }

// ---- Txn -------------------------------------------------------------------

DbResult<Txn> Txn::Begin(Db &db) {
  if (auto ok = db.Exec("BEGIN IMMEDIATE;"); !ok) {
    return std::unexpected(ok.error());
  }
  return Txn(db);
}

DbResult<void> Txn::Commit() {
  Db *db = std::exchange(db_, nullptr);
  auto ok = db->Exec("COMMIT;");
  if (!ok) {
    // A failed COMMIT (SQLITE_BUSY, say) leaves the transaction open; on a
    // long-lived connection every later BEGIN would then fail.
    [[maybe_unused]] const auto rolled_back = db->Exec("ROLLBACK;");
  }
  return ok;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup of `ok`

Txn::~Txn() {
  if (db_ != nullptr) {
    // Nothing more to do if even the rollback fails: SQLite rolls back an
    // open transaction itself when the connection closes.
    [[maybe_unused]] const auto rolled_back = db_->Exec("ROLLBACK;");
  }
}

}  // namespace micnote::store
