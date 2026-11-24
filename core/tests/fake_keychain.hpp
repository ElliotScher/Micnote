// An OS keychain in memory, for testing KeychainSecrets, the migration and
// micnote-cli's choice of key store without touching the real one.
#pragma once

#include <map>
#include <string>

#include "secrets/keychain.hpp"

namespace micnote::test {

class FakeKeychain final : public secrets::KeychainBackend {
 public:
  bool Available() override { return available; }
  Status Get(const std::string &name, std::string &value) override {
    ++gets;
    if (fail_get) {
      return Status::kFailed;
    }
    const auto it = items.find(name);
    if (it == items.end()) {
      return Status::kNotFound;
    }
    value = it->second;
    return Status::kOk;
  }
  Status Set(const std::string &name, const std::string &value) override {
    if (fail_set) {
      return Status::kFailed;
    }
    // A keychain that loses what it was given.
    items[name] = garble_sets ? std::string(value.size(), '0') : value;
    return Status::kOk;
  }
  Status Remove(const std::string &name) override {
    if (fail_remove) {
      return Status::kFailed;
    }
    return items.erase(name) == 1 ? Status::kOk : Status::kNotFound;
  }

  std::map<std::string, std::string> items;  // name -> hex
  bool available = true;
  bool fail_get = false;
  bool fail_set = false;
  bool fail_remove = false;
  bool garble_sets = false;
  int gets = 0;
};

}  // namespace micnote::test
