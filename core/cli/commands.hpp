// micnote-cli's commands, apart from main() so tests can run them against a
// fake puck: everything the outside world provides (output streams, opening
// a port, the clock, environment variables) comes in through Env.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

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

std::string Describe(const protocol::ClientError &error);
std::string Describe(recording::ReadError error);
std::string_view DeviceErrorName(micnote_v1_ErrorCode code);

}  // namespace micnote::cli
