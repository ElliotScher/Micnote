// `micnote-cli models`: the registry's models (models/manifest.json), their
// files in the models directory, and which one this machine uses by default.
#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "commands.hpp"
#include "models/registry.hpp"

namespace micnote::cli {

inline constexpr std::string_view kModelsUsage =
    "       micnote-cli models list   [--models DIR]\n"
    "       micnote-cli models verify [--models DIR] [--id ID]\n";

// args without argv[0], args[0] being "models". Exit code as Run().
int Models(std::span<const std::string_view> args, const Env &env);

// $XDG_DATA_HOME/micnote/models, else ~/.local/share/micnote/models
// (%LOCALAPPDATA%\micnote\models on Windows); empty if none of those is set.
std::filesystem::path DefaultModelsDir(const std::function<const char *(const char *)> &getenv);

// This machine for choosing models: env.device if set (tests). Builds with
// speech recognition know whether it can use a GPU.
models::Device CliDevice(const Env &env);

// The registry the CLI uses: env.manifest_json if set (tests), else the
// built-in one. Reports a bad manifest on env.err.
std::expected<models::Registry, int> CliRegistry(const Env &env);

}  // namespace micnote::cli
