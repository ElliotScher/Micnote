#include "keys.hpp"

#include <format>
#include <utility>

#include "crypto/psa.hpp"
#include "secrets/keychain.hpp"

namespace micnote::cli {

namespace {

namespace fs = std::filesystem;
using secrets::MovedSecret;

Keys Files(const fs::path &dir, std::string host_key) {
  // GCOVR_EXCL_BR_START: gcc's exception cleanup of the initializer's temporaries.
  return Keys{.store = std::make_unique<secrets::FileSecrets>(dir),
              .host_key = std::move(host_key),
              .dir = dir,
              .in_keychain = false};
  // GCOVR_EXCL_BR_STOP
}

// Moves the config directory's key files into the keychain, saying what
// happened to each. False if one that should have moved didn't: the files
// are then still the whole truth, so the command uses them.
bool MoveIn(const fs::path &config, secrets::Secrets &keychain, const Env &env) {
  bool ok = true;
  for (const MovedSecret &m : secrets::MoveFileSecrets(config, keychain)) {
    const std::string file = (config / m.name).string();
    switch (m.outcome) {
      case MovedSecret::Outcome::kMoved:
        *env.err << std::format("micnote-cli: moved {} into the keychain\n", file);
        break;
      case MovedSecret::Outcome::kDuplicate:
        *env.err << std::format("micnote-cli: removed {}: the keychain already has it\n", file);
        break;
      case MovedSecret::Outcome::kConflict:
        *env.err << std::format(
            "micnote-cli: WARNING: {} differs from {} in the keychain; using the keychain's and "
            "leaving the file alone (delete whichever is wrong)\n",
            file, m.name);
        break;
      default:
        *env.err << std::format("micnote-cli: couldn't move {} into the keychain\n", file);
        ok = false;
        break;
    }
  }
  return ok;
}

}  // namespace

std::optional<recording::HostKey> LoadHostKey(const Keys &keys, const Env &env) {
  auto bytes = keys.store->Get(keys.host_key);
  if (!bytes) {
    *env.err << std::format("micnote-cli: no host key at {}; run: micnote-cli keygen\n",
                            keys.Where(keys.host_key));
    return std::nullopt;
  }
  if (bytes->size() != crypto::kX25519KeySize) {
    crypto::Wipe(*bytes);
    *env.err << std::format("micnote-cli: {} isn't a host key (expected 32 bytes)\n",
                            keys.Where(keys.host_key));
    return std::nullopt;
  }
  auto key = recording::HostKey::FromPrivate(std::span(*bytes).first<crypto::kX25519KeySize>());
  crypto::Wipe(*bytes);
  return std::move(*key);
}

std::string Keys::Where(std::string_view name) const {
  // GCOVR_EXCL_BR_START: both sides run (the keychain and file tests); the
  // rest is gcc's exception cleanup of the temporaries.
  return in_keychain ? std::format("{} in the keychain", name) : (dir / name).string();
  // GCOVR_EXCL_BR_STOP
}

std::expected<Keys, int> OpenKeys(const std::optional<fs::path> &key_file, const Env &env) {
  if (key_file) {
    return Files(key_file->parent_path(), key_file->filename().string());
  }
  const fs::path config = env.default_key.parent_path();
  if (env.keychain != nullptr && env.keychain->Available()) {
    auto store = std::make_unique<secrets::KeychainSecrets>(*env.keychain);
    if (config.empty() || MoveIn(config, *store, env)) {
      // GCOVR_EXCL_BR_START: as in Files().
      return Keys{.store = std::move(store),
                  .host_key = std::string(secrets::kHostKey),
                  .dir = {},
                  .in_keychain = true};
      // GCOVR_EXCL_BR_STOP
    }
    *env.err << std::format("micnote-cli: using the key files in {} for now\n", config.string());
    return Files(config, env.default_key.filename().string());
  }
  if (env.default_key.empty()) {
    *env.err << "micnote-cli: no home directory for the host key; pass --key FILE\n";
    return std::unexpected(1);
  }
  if (env.keychain != nullptr) {
    *env.err << std::format(
        "micnote-cli: WARNING: no OS keychain is available (on Linux, no Secret Service such as "
        "GNOME Keyring or KWallet is running), so keys are plain files in {}, protected only by "
        "their permissions\n",
        config.string());
  }
  return Files(config, env.default_key.filename().string());
}

}  // namespace micnote::cli
