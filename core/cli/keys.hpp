// Where micnote-cli keeps its secrets: the host key, a link key per paired
// puck, and the store's db_key.
//
// - --key FILE: files. The host key is FILE; link keys are in pucks/ beside
//   it, and the store's key beside it too.
// - Otherwise, the OS keychain (stage 2 §4.6) when there is one, after moving
//   any key files left in the config directory into it (MoveFileSecrets).
// - With no keychain (a Linux machine with no Secret Service running, say),
//   files in the config directory, with a loud warning each time.
#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "commands.hpp"
#include "recording/reader.hpp"
#include "secrets/secrets.hpp"

namespace micnote::cli {

struct Keys {
  std::unique_ptr<secrets::Secrets> store;
  // The host key's name in store.
  std::string host_key;
  // The files' directory (with no keychain).
  std::filesystem::path dir;
  bool in_keychain = false;

  // Where the secret `name` is, for messages: a path, or "NAME in the keychain".
  [[nodiscard]] std::string Where(std::string_view name) const;
};

// The keys for this command: key_file is --key's value, if given. Explains a
// failure on env.err and returns exit code 1.
std::expected<Keys, int> OpenKeys(const std::optional<std::filesystem::path> &key_file,
                                  const Env &env);

// The host key from keys; nullopt after explaining (none yet, or not 32 bytes).
std::optional<recording::HostKey> LoadHostKey(const Keys &keys, const Env &env);

}  // namespace micnote::cli
