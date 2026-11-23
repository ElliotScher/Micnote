// Schema migrations (stage 2 §4.3): the numbered .sql files in
// store/migrations/, embedded at build time (CMake generates
// store/migrations.inc), applied in order inside one transaction each, with
// PRAGMA user_version recording how many have run.
#pragma once

#include <span>
#include <string_view>

#include "store/db.hpp"

namespace micnote::store {

struct Migration {
  std::string_view name;
  std::string_view sql;
};

// The embedded migrations, in order.
std::span<const Migration> Migrations();

// Brings db up to date. A database from a newer build (user_version beyond
// what this build knows) is refused, not downgraded.
DbResult<void> Migrate(Db &db);
DbResult<void> Migrate(Db &db, std::span<const Migration> migrations);

DbResult<int> SchemaVersion(Db &db);

}  // namespace micnote::store
