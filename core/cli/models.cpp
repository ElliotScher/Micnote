#include "models.hpp"

#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace micnote::cli {

namespace {

namespace fs = std::filesystem;

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsageError = 2;

std::string Size(std::uint64_t bytes) {
  constexpr double kMB = 1000.0 * 1000.0;
  return std::format("{:.1f} MB", static_cast<double>(bytes) / kMB);
}

int List(const models::Registry &reg, const fs::path &dir, const Env &env) {
  const models::Device device = env.device ? *env.device : models::ThisDevice();
  *env.out << std::format("models in {} (this machine: {}, {} MB of RAM)\n", dir.string(),
                          models::ToString(device.cls), device.ram_mb);
  for (const models::Model &m : reg.models()) {
    const models::FileState state = models::Check(m, dir);
    const bool is_default = reg.Default(m.kind, device) == &m;
    *env.out << std::format("  {:<28} {:<3} {:>9}  {:<10}{}\n", m.id, models::ToString(m.kind),
                            Size(m.bytes), models::ToString(state), is_default ? "  default" : "");
    if (state != models::FileState::kPresent) {
      *env.out << std::format("      get it: curl -L -o {} {}\n", (dir / m.file).string(), m.url);
    }
  }
  return kOk;
}

int Verify(const models::Registry &reg, const fs::path &dir, const std::string *only,
           const Env &env) {
  if (only != nullptr && reg.Find(*only) == nullptr) {
    *env.err << std::format("micnote-cli: no model \"{}\" in the manifest\n", *only);
    return kFailed;
  }
  bool all_ok = true;
  for (const models::Model &m : reg.models()) {
    if (only != nullptr && m.id != *only) {
      continue;
    }
    const auto result = models::Verify(m, dir, [&](std::uint64_t done, std::uint64_t total) {
      *env.err << std::format("\r{}: verifying... {}%", m.id, done * 100 / total) << std::flush;
      return true;
    });
    if (result != models::VerifyResult::kMissing) {
      *env.err << "\n";
    }
    if (result == models::VerifyResult::kMissing && only == nullptr) {
      *env.out << std::format("{}: not downloaded\n", m.id);
      continue;
    }
    *env.out << std::format("{}: {}\n", m.id, models::ToString(result));
    all_ok = all_ok && result == models::VerifyResult::kOk;
  }
  return all_ok ? kOk : kFailed;
}

}  // namespace

std::expected<models::Registry, int> CliRegistry(const Env &env) {
  auto reg = env.manifest_json.empty() ? models::Registry::Builtin()
                                       : models::Registry::Parse(env.manifest_json);
  if (!reg) {
    *env.err << std::format("micnote-cli: bad model manifest: {}\n", reg.error().message);
    return std::unexpected(kFailed);
  }
  return std::move(*reg);
}

int Models(std::span<const std::string_view> args, const Env &env) {
  const auto usage = [&] {
    *env.err << "usage:\n" << kModelsUsage;
    return kUsageError;
  };
  if (args.size() < 2 || (args[1] != "list" && args[1] != "verify")) {
    return usage();
  }
  const bool verify = args[1] == "verify";
  std::map<std::string, std::string, std::less<>> opts;
  for (std::size_t i = 2; i < args.size(); i += 2) {
    const std::string_view o = args[i];
    const bool known = o == "--models" || (verify && o == "--id");
    if (!known || i + 1 == args.size() || !opts.emplace(o, args[i + 1]).second) {
      return usage();
    }
  }
  fs::path dir = env.models_dir;
  if (const auto it = opts.find("--models"); it != opts.end()) {
    dir = it->second;
  }
  if (dir.empty()) {
    *env.err << "micnote-cli: no home directory for models; pass --models DIR\n";
    return kFailed;
  }
  const auto reg = CliRegistry(env);
  if (!reg) {
    return reg.error();
  }
  if (!verify) {
    return List(*reg, dir, env);
  }
  const auto id = opts.find("--id");
  return Verify(*reg, dir, id != opts.end() ? &id->second : nullptr, env);
}

std::filesystem::path DefaultModelsDir(const std::function<const char *(const char *)> &getenv) {
  const auto set = [&](const char *name) -> std::optional<fs::path> {
    const char *value = getenv(name);
    if (value == nullptr || *value == '\0') {
      return std::nullopt;
    }
    return fs::path(value);
  };
#if defined(_WIN32)
  if (const auto local = set("LOCALAPPDATA")) {
    return *local / "micnote" / "models";
  }
#else
  if (const auto xdg = set("XDG_DATA_HOME")) {
    return *xdg / "micnote" / "models";
  }
  if (const auto home = set("HOME")) {
    return *home / ".local" / "share" / "micnote" / "models";
  }
#endif
  return {};
}

}  // namespace micnote::cli
