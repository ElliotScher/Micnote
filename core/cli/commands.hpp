// micnote-cli's commands, apart from main() so tests can run them against a
// fake puck: everything the outside world provides (output streams, opening
// a port, the clock, environment variables) comes in through Env.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include "models/registry.hpp"
#include "protocol/client.hpp"
#include "recording/reader.hpp"
#include "transport/transport.hpp"

namespace micnote::cli {

using Opener =
    std::function<std::expected<std::unique_ptr<transport::Transport>, transport::TransportError>(
        const std::string &path)>;

struct Env {
  std::ostream *out;  // never null
  std::ostream *err;  // never null
  Opener open;
  std::function<std::uint64_t()> now_unix_ms;
  // Where the host key lives unless --key says otherwise; empty if unknown.
  std::filesystem::path default_key;
  // Where models live unless --models says otherwise; empty if unknown.
  std::filesystem::path models_dir;
  // Tests: a manifest instead of the built-in one, and a device instead of
  // this machine.
  std::string manifest_json;
  std::optional<models::Device> device;
};

// Runs one command line (without argv[0]). Returns the exit code: 0 on
// success, 1 if the command failed, 2 for a usage error.
int Run(std::span<const std::string_view> args, const Env &env);

// $XDG_CONFIG_HOME/micnote/host.key, else ~/.config/micnote/host.key
// (%APPDATA%\micnote\host.key on Windows); empty if none of those is set.
std::filesystem::path DefaultKeyPath(const std::function<const char *(const char *)> &getenv);

// The real world, for main(): ports are USB serial devices; the clock is
// the system's.
Opener UsbOpener();
std::uint64_t SystemNowMs();

// Writes rec as Ogg Opus to path, reporting on env as sync does ("name: ->
// path"). False if it isn't Opus or can't be written. Used by sync; exposed
// for tests.
bool WriteOpusFile(const recording::Recording &rec, const std::filesystem::path &path,
                   std::string_view name, const Env &env);

std::string Describe(const protocol::ClientError &error);
std::string Describe(recording::ReadError error);
std::string_view DeviceErrorName(micnote_v1_ErrorCode code);

}  // namespace micnote::cli
