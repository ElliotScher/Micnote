#include "store/schema.hpp"

#include <array>
#include <string>

namespace micnote::store {

namespace {

constexpr std::array kMigrations = {
#include "store/migrations.inc"
};

// SQLite's code for "this database is newer than this program".
constexpr int kTooNew = 1;

}  // namespace

std::span<const Migration> Migrations() { return kMigrations; }

DbResult<int> SchemaVersion(Db &db) {
  auto stmt = db.Prepare("PRAGMA user_version;");
  if (!stmt) {
    return std::unexpected(stmt.error());
  }
  if (auto row = stmt->Step(); !row) {
    return std::unexpected(row.error());
  }
  return static_cast<int>(stmt->Int(0));
}

DbResult<void> Migrate(Db &db, std::span<const Migration> migrations) {
  auto version = SchemaVersion(db);
  if (!version) {
    return std::unexpected(version.error());
  }
  const auto known = static_cast<int>(migrations.size());
  if (*version > known) {
    return std::unexpected(DbError{.code = kTooNew,
                                   .message = "the database is from a newer Micnote (schema " +
                                              std::to_string(*version) + ", this build knows " +
                                              std::to_string(known) + ")"});
  }
  for (int v = *version; v < known; ++v) {
    auto txn = Txn::Begin(db);
    if (!txn) {
      return std::unexpected(txn.error());
    }
    const Migration &m = migrations[static_cast<std::size_t>(v)];
    if (auto ok = db.Exec(m.sql); !ok) {
      DbError e = ok.error();
      e.message = std::string(m.name) + ": " + e.message;
      return std::unexpected(std::move(e));
    }
    if (auto ok = db.Exec("PRAGMA user_version = " + std::to_string(v + 1) + ";"); !ok) {
      return std::unexpected(ok.error());
    }
    if (auto ok = txn->Commit(); !ok) {
      return std::unexpected(ok.error());
    }
  }
  return {};
}

DbResult<void> Migrate(Db &db) { return Migrate(db, Migrations()); }

}  // namespace micnote::store
