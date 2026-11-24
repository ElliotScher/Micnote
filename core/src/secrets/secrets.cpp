#include "secrets/secrets.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <system_error>

#include "crypto/psa.hpp"

namespace micnote::secrets {

namespace {

// Secrets are small: keys and tokens.
constexpr std::size_t kMaxSecret = 4096;

}  // namespace

bool ValidName(std::string_view name) {
  if (name.empty() || name.size() > 128 || name.front() == '/' || name.contains("..")) {
    return false;
  }
  return std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '/' || c == '-';
  });
}

std::expected<void, SecretError> Secrets::Remove(std::string_view /*name*/) {
  return std::unexpected(SecretError::kProvider);
}

std::expected<std::vector<std::byte>, SecretError> GetOrCreate(Secrets &s, std::string_view name,
                                                               std::size_t size) {
  auto got = s.Get(name);
  if (got) {
    if (got->size() != size) {
      return std::unexpected(SecretError::kCorrupt);
    }
    return got;
  }
  if (got.error() != SecretError::kNotFound) {
    return got;
  }
  std::vector<std::byte> fresh(size);
  // GCOVR_EXCL_START: PSA's RNG fails only without an OS entropy source.
  if (crypto::Init() != PSA_SUCCESS ||
      psa_generate_random(reinterpret_cast<std::uint8_t *>(fresh.data()), fresh.size()) !=
          PSA_SUCCESS) {
    return std::unexpected(SecretError::kIo);
  }
  // GCOVR_EXCL_STOP
  if (auto set = s.Set(name, fresh); !set) {
    crypto::Wipe(fresh);
    return std::unexpected(set.error());
  }
  return fresh;
}

// ---- files -------------------------------------------------------------------

std::expected<std::vector<std::byte>, SecretError> FileSecrets::Get(std::string_view name) {
  if (!ValidName(name)) {
    return std::unexpected(SecretError::kBadName);
  }
  const auto path = dir_ / std::filesystem::path(std::string(name));
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::unexpected(SecretError::kNotFound);
  }
  std::array<char, kMaxSecret + 1> buf{};
  file.read(buf.data(), buf.size());
  const auto n = static_cast<std::size_t>(file.gcount());
  if (n > kMaxSecret) {
    return std::unexpected(SecretError::kCorrupt);
  }
  std::vector<std::byte> out(n);
  std::ranges::transform(std::span(buf).first(n), out.begin(),
                         [](char c) { return static_cast<std::byte>(c); });
  crypto::Wipe(std::as_writable_bytes(std::span(buf)));
  return out;
}

std::expected<void, SecretError> FileSecrets::Set(std::string_view name,
                                                  std::span<const std::byte> value) {
  if (!ValidName(name)) {
    return std::unexpected(SecretError::kBadName);
  }
  if (value.size() > kMaxSecret) {
    return std::unexpected(SecretError::kCorrupt);
  }
  namespace fs = std::filesystem;
  const fs::path path = dir_ / fs::path(std::string(name));
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  // Written beside, then renamed over: a crash never leaves half a key.
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    // Owner-only before any byte of the secret is written.
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, ec);
    file.write(reinterpret_cast<const char *>(value.data()),
               static_cast<std::streamsize>(value.size()));
    if (!file) {
      return std::unexpected(SecretError::kIo);
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return std::unexpected(SecretError::kIo);
  }
  return {};
}

std::expected<void, SecretError> FileSecrets::Remove(std::string_view name) {
  if (!ValidName(name)) {
    return std::unexpected(SecretError::kBadName);
  }
  std::error_code ec;
  std::filesystem::remove(dir_ / std::filesystem::path(std::string(name)), ec);
  if (ec) {
    return std::unexpected(SecretError::kIo);
  }
  return {};
}

// ---- callbacks -----------------------------------------------------------------

std::expected<std::vector<std::byte>, SecretError> CallbackSecrets::Get(std::string_view name) {
  if (!ValidName(name)) {
    return std::unexpected(SecretError::kBadName);
  }
  const std::string n(name);
  std::vector<std::byte> out(kMaxSecret);
  std::size_t len = 0;
  const int rc = cb_.get(n.c_str(), reinterpret_cast<std::uint8_t *>(out.data()), out.size(), &len);
  if (rc == 1) {
    return std::unexpected(SecretError::kNotFound);
  }
  if (rc != 0 || len > out.size()) {
    crypto::Wipe(out);
    return std::unexpected(SecretError::kProvider);
  }
  out.resize(len);
  return out;
}

std::expected<void, SecretError> CallbackSecrets::Set(std::string_view name,
                                                      std::span<const std::byte> value) {
  if (!ValidName(name)) {
    return std::unexpected(SecretError::kBadName);
  }
  const std::string n(name);
  if (cb_.set(n.c_str(), reinterpret_cast<const std::uint8_t *>(value.data()), value.size()) != 0) {
    return std::unexpected(SecretError::kProvider);
  }
  return {};
}

}  // namespace micnote::secrets
