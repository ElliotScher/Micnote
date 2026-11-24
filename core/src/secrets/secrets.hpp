// Secrets (stage 2 §4.6): small named byte strings that must never sit in
// the database or a plain file next to it: db_key, host_x25519_priv, and
// one link key per paired puck ("link_key/<device_id hex>").
//
// Where they live is the host's business:
// - the app passes get/set callbacks through the C ABI (flutter_secure_storage
//   on every platform), CallbackSecrets;
// - the CLI keeps them in the OS keychain (KeychainSecrets, keychain.hpp),
//   or, with no keychain or with --key, in files under its config directory,
//   mode 0600 (FileSecrets, the documented headless fallback).
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace micnote::secrets {

enum class SecretError : std::uint8_t {
  kNotFound,
  // The name has characters other than [A-Za-z0-9_./-], or a ".." part.
  kBadName,
  kIo,
  // A stored secret of the wrong size.
  kCorrupt,
  // The host's callback reported a failure.
  kProvider,
};

class Secrets {
 public:
  Secrets() = default;
  Secrets(const Secrets &) = delete;
  Secrets &operator=(const Secrets &) = delete;
  Secrets(Secrets &&) = delete;
  Secrets &operator=(Secrets &&) = delete;
  // GCOVR_EXCL_START: the deleting destructor only runs for an object whose
  // dynamic type is Secrets, which is abstract.
  virtual ~Secrets() = default;
  // GCOVR_EXCL_STOP

  virtual std::expected<std::vector<std::byte>, SecretError> Get(std::string_view name) = 0;
  virtual std::expected<void, SecretError> Set(std::string_view name,
                                               std::span<const std::byte> value) = 0;
  // Forgets `name`; one that isn't there is fine. kProvider where the store
  // can't delete (the C ABI's callbacks have no delete).
  virtual std::expected<void, SecretError> Remove(std::string_view name);
};

// The secret `name`, creating it from `size` random bytes on first use. One
// stored with another size is kCorrupt (a db_key must never be "regenerated"
// silently: that would lose the database).
std::expected<std::vector<std::byte>, SecretError> GetOrCreate(Secrets &s, std::string_view name,
                                                               std::size_t size);

bool ValidName(std::string_view name);

// The names the core uses. They match micnote-cli's files (<config>/host.key
// and <config>/pucks/<device_id>.link), so a FileSecrets rooted at the CLI's
// config directory sees what `micnote-cli keygen` and `pair` stored.
inline constexpr std::string_view kHostKey = "host.key";
inline constexpr std::string_view kDbKey = "db_key";
inline std::string LinkKeyName(std::string_view device_id_hex) {
  return "pucks/" + std::string(device_id_hex) + ".link";
}

class FileSecrets final : public Secrets {
 public:
  explicit FileSecrets(std::filesystem::path dir) : dir_(std::move(dir)) {}
  std::expected<std::vector<std::byte>, SecretError> Get(std::string_view name) override;
  std::expected<void, SecretError> Set(std::string_view name,
                                       std::span<const std::byte> value) override;
  std::expected<void, SecretError> Remove(std::string_view name) override;

 private:
  std::filesystem::path dir_;
};

// The C ABI's provider: get returns 0 with *len set, or non-zero (1: not
// found); set returns 0 or non-zero.
struct SecretCallbacks {
  std::function<int(const char *name, std::uint8_t *out, std::size_t cap, std::size_t *len)> get;
  std::function<int(const char *name, const std::uint8_t *data, std::size_t len)> set;
};

class CallbackSecrets final : public Secrets {
 public:
  explicit CallbackSecrets(SecretCallbacks cb) : cb_(std::move(cb)) {}
  std::expected<std::vector<std::byte>, SecretError> Get(std::string_view name) override;
  std::expected<void, SecretError> Set(std::string_view name,
                                       std::span<const std::byte> value) override;

 private:
  SecretCallbacks cb_;
};

}  // namespace micnote::secrets
