// Secrets in the OS keychain (stage 2 §4.6), for micnote-cli: macOS
// Keychain, Windows Credential Manager, the Secret Service (GNOME Keyring,
// KWallet) on Linux. The app doesn't use this: it hands the core its secrets
// through the C ABI's callbacks (CallbackSecrets), from flutter_secure_storage.
//
// The OS part is a KeychainBackend: text items under one service, named by
// the secret's name. micnote-cli's is in core/cli/os_keychain.cpp (it links
// the keychain library, which libmicnote doesn't); tests use a fake.
// KeychainSecrets stores each secret as lowercase hex, since keychains hold
// text.
//
// MoveFileSecrets moves the CLI's older key files into the keychain.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "secrets/secrets.hpp"

namespace micnote::secrets {

class KeychainBackend {
 public:
  enum class Status : std::uint8_t {
    kOk,
    kNotFound,
    kFailed,  // denied, locked, or the keychain went away
  };

  KeychainBackend() = default;
  KeychainBackend(const KeychainBackend &) = delete;
  KeychainBackend &operator=(const KeychainBackend &) = delete;
  KeychainBackend(KeychainBackend &&) = delete;
  KeychainBackend &operator=(KeychainBackend &&) = delete;
  // GCOVR_EXCL_START: the deleting destructor only runs for an object whose
  // dynamic type is KeychainBackend, which is abstract.
  virtual ~KeychainBackend() = default;
  // GCOVR_EXCL_STOP

  // Whether there's a keychain to use at all (on Linux, a Secret Service).
  virtual bool Available() = 0;
  virtual Status Get(const std::string &name, std::string &value) = 0;
  virtual Status Set(const std::string &name, const std::string &value) = 0;
  virtual Status Remove(const std::string &name) = 0;
};

class KeychainSecrets final : public Secrets {
 public:
  // backend must outlive this.
  explicit KeychainSecrets(KeychainBackend &backend) : backend_(&backend) {}
  std::expected<std::vector<std::byte>, SecretError> Get(std::string_view name) override;
  std::expected<void, SecretError> Set(std::string_view name,
                                       std::span<const std::byte> value) override;
  std::expected<void, SecretError> Remove(std::string_view name) override;

 private:
  KeychainBackend *backend_;
};

struct MovedSecret {
  enum class Outcome : std::uint8_t {
    kMoved,      // now in the keychain; the file is gone
    kDuplicate,  // the keychain already had the same bytes; the file is gone
    kConflict,   // the keychain has different bytes: both kept, the file untouched
    kFailed,     // couldn't be stored or read back: the file untouched
  };
  std::string name;
  Outcome outcome = Outcome::kFailed;
};

// Moves the secrets kept as files in dir (host.key, db_key, pucks/*.link,
// the names FileSecrets uses) into `to`. A file is deleted, after being
// overwritten with zeros, only once `to` holds the same bytes. Nothing to
// move gives an empty list.
std::vector<MovedSecret> MoveFileSecrets(const std::filesystem::path &dir, Secrets &to);

}  // namespace micnote::secrets
