// The model registry (stage 2 §4.5, stage 5b §4.4): which models Micnote
// knows, where each comes from, its SHA-256, and which one a device should
// use by default. The list is models/manifest.json, built into the library,
// so the pinned URLs and hashes always match the code; the files themselves
// live in a models directory (mn_config.models_dir), named as in their URLs.
//
// Manifest (version 1):
//   {"version": 1,
//    "models": [{"id", "kind": "asr"|"vad"|"llm", "url": "https://...",
//                "sha256": 64 hex digits, "bytes", "min_ram_mb" (optional),
//                "classes": ["mobile-small", "mobile", "desktop"],
//                "license" (optional)}],
//    "defaults": [{"class", "min_ram_mb" (optional),
//                  "gpu" (optional, true: only with one), "asr"/"vad"/"llm": id}]}
// Defaults are tried in order: the first for the device's class whose
// min_ram_mb it meets and that names a model of the kind wanted wins.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crypto/psa.hpp"

namespace micnote::models {

enum class Kind : std::uint8_t { kAsr, kVad, kLlm };
enum class DeviceClass : std::uint8_t { kMobileSmall, kMobile, kDesktop };

std::string_view ToString(Kind kind);
std::string_view ToString(DeviceClass cls);
std::optional<Kind> ParseKind(std::string_view s);
std::optional<DeviceClass> ParseClass(std::string_view s);

struct Model {
  std::string id;
  Kind kind = Kind::kAsr;
  std::string url;
  // The URL's last path component: the file's name in the models directory.
  std::string file;
  crypto::Sha256Digest sha256{};
  std::uint64_t bytes = 0;
  std::uint64_t min_ram_mb = 0;
  std::vector<DeviceClass> classes;
  std::string license;

  [[nodiscard]] bool RunsOn(DeviceClass cls) const;
};

struct Device {
  DeviceClass cls = DeviceClass::kDesktop;
  std::uint64_t ram_mb = 0;
  // Whether speech recognition can use a GPU here. ThisDevice() can't tell
  // (the core doesn't link the engine), so whoever has the engine sets it.
  bool gpu = false;
};

// Phones below this much RAM are mobile-small (a "6 GB" phone reports a
// little under 6144 MB).
inline constexpr std::uint64_t kMobileSmallBelowMb = 5120;

// The class rule: phone builds (Android, iOS) are mobile, or mobile-small
// below kMobileSmallBelowMb; everything else is desktop.
DeviceClass ClassFor(bool mobile, std::uint64_t ram_mb);
// This machine, with its physical RAM from the OS (0 if it won't say).
Device ThisDevice();

struct ManifestError {
  std::string message;
};

class Registry {
 public:
  static std::expected<Registry, ManifestError> Parse(std::string_view text);
  // models/manifest.json as this library was built with it.
  static std::expected<Registry, ManifestError> Builtin();

  [[nodiscard]] int version() const { return version_; }
  [[nodiscard]] const std::vector<Model> &models() const { return models_; }
  [[nodiscard]] const Model *Find(std::string_view id) const;
  // The manifest's choice of `kind` for `device`; null if it has none.
  [[nodiscard]] const Model *Default(Kind kind, const Device &device) const;

 private:
  struct Rule {
    DeviceClass cls = DeviceClass::kDesktop;
    std::uint64_t min_ram_mb = 0;
    bool gpu = false;
    std::array<std::string, 3> ids;  // by Kind; empty for none
  };
  Registry() = default;

  int version_ = 0;
  std::vector<Model> models_;
  std::vector<Rule> rules_;
};

enum class FileState : std::uint8_t { kMissing, kWrongSize, kPresent };
std::string_view ToString(FileState state);
// Cheap: whether the file is there with the manifest's size. Verify()
// checks what's in it.
FileState Check(const Model &model, const std::filesystem::path &dir);

enum class VerifyResult : std::uint8_t { kOk, kMissing, kMismatch, kCancelled };
std::string_view ToString(VerifyResult result);
// Called as the hash advances; returning false cancels.
using VerifyProgress = std::function<bool(std::uint64_t done, std::uint64_t total)>;
// Hashes the file (a 574 MB model takes a few seconds) against the manifest.
VerifyResult Verify(const Model &model, const std::filesystem::path &dir,
                    const VerifyProgress &progress = {});

}  // namespace micnote::models
