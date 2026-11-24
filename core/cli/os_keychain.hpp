// micnote-cli's OS keychain: hrantzsch/keychain (pinned in
// core/cmake/deps.cmake) behind secrets::KeychainBackend. Items are under
// the package "org.micnote" and the service "micnote-cli", one per secret,
// named by the secret's name (host.key, db_key, pucks/<device_id>.link).
#pragma once

#include <string>

#include "secrets/keychain.hpp"

namespace micnote::cli {

class OsKeychain final : public secrets::KeychainBackend {
 public:
  bool Available() override;
  Status Get(const std::string &name, std::string &value) override;
  Status Set(const std::string &name, const std::string &value) override;
  Status Remove(const std::string &name) override;
};

}  // namespace micnote::cli
