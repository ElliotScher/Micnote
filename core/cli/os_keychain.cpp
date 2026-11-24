#include "os_keychain.hpp"

#include <keychain/keychain.h>

namespace micnote::cli {

// GCOVR_EXCL_START: this file calls the OS keychain, which CI's Linux runners
// (where coverage is measured) don't have: no Secret Service runs there.
// micnote_tests' OsKeychain test round-trips through it wherever one exists
// (a desktop session, the macOS and Windows runners); everything above it,
// KeychainSecrets and the migration, is tested with a fake backend.
namespace {

constexpr const char *kPackage = "org.micnote";
constexpr const char *kService = "micnote-cli";

secrets::KeychainBackend::Status FromError(const keychain::Error &err) {
  if (!err) {
    return secrets::KeychainBackend::Status::kOk;
  }
  return err.type == keychain::ErrorType::NotFound ? secrets::KeychainBackend::Status::kNotFound
                                                   : secrets::KeychainBackend::Status::kFailed;
}

}  // namespace

bool OsKeychain::Available() {
  keychain::Error err;
  return keychain::isAvailable(err) && !err;
}

secrets::KeychainBackend::Status OsKeychain::Get(const std::string &name, std::string &value) {
  keychain::Error err;
  value = keychain::getPassword(kPackage, kService, name, err);
  return FromError(err);
}

secrets::KeychainBackend::Status OsKeychain::Set(const std::string &name,
                                                 const std::string &value) {
  keychain::Error err;
  keychain::setPassword(kPackage, kService, name, value, err);
  return FromError(err);
}

secrets::KeychainBackend::Status OsKeychain::Remove(const std::string &name) {
  keychain::Error err;
  keychain::deletePassword(kPackage, kService, name, err);
  return FromError(err);
}
// GCOVR_EXCL_STOP

}  // namespace micnote::cli
