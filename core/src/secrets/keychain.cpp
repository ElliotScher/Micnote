#include "secrets/keychain.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <system_error>

#include "crypto/psa.hpp"

namespace micnote::secrets {

namespace {

namespace fs = std::filesystem;

// As FileSecrets: secrets are small.
constexpr std::size_t kMaxSecret = 4096;
constexpr std::string_view kDigits = "0123456789abcdef";

void WipeText(std::string &s) {
  crypto::Wipe(std::as_writable_bytes(std::span(s.data(), s.size())));
  s.clear();
}

std::optional<std::uint8_t> Nibble(char c) {
  const auto at = kDigits.find(c);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(at);
}

// Overwrites a file of `size` bytes with zeros, then removes it.
bool Shred(const fs::path &path, std::size_t size) {
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    const std::vector<char> zeros(size);
    f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  }
  std::error_code ec;
  return fs::remove(path, ec);
}

}  // namespace

std::expected<std::vector<std::byte>, SecretError> KeychainSecrets::Get(std::string_view name) {
  if (!ValidName(name)) {
    return std::unexpected(SecretError::kBadName);
  }
  std::string hex;
  const auto status = backend_->Get(std::string(name), hex);
  if (status == KeychainBackend::Status::kNotFound) {
    return std::unexpected(SecretError::kNotFound);
  }
  if (status != KeychainBackend::Status::kOk) {
    return std::unexpected(SecretError::kIo);
  }
  if (hex.size() % 2 != 0 || hex.size() > 2 * kMaxSecret) {
    WipeText(hex);
    return std::unexpected(SecretError::kCorrupt);
  }
  std::vector<std::byte> out(hex.size() / 2);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const auto hi = Nibble(hex[2 * i]);
    const auto lo = Nibble(hex[(2 * i) + 1]);
    if (!hi || !lo) {
      WipeText(hex);
      crypto::Wipe(out);
      return std::unexpected(SecretError::kCorrupt);
    }
    out[i] = static_cast<std::byte>((static_cast<unsigned>(*hi) << 4U) | *lo);
  }
  WipeText(hex);
  return out;
}

std::expected<void, SecretError> KeychainSecrets::Set(std::string_view name,
                                                      std::span<const std::byte> value) {
  if (!ValidName(name)) {
    return std::unexpected(SecretError::kBadName);
  }
  if (value.size() > kMaxSecret) {
    return std::unexpected(SecretError::kCorrupt);
  }
  std::string hex;
  hex.reserve(value.size() * 2);
  for (const std::byte b : value) {
    hex += kDigits[std::to_integer<unsigned>(b) >> 4U];
    hex += kDigits[std::to_integer<unsigned>(b) & 0xFU];
  }
  const auto status = backend_->Set(std::string(name), hex);
  WipeText(hex);
  if (status != KeychainBackend::Status::kOk) {
    return std::unexpected(SecretError::kIo);
  }
  return {};
}

std::expected<void, SecretError> KeychainSecrets::Remove(std::string_view name) {
  if (!ValidName(name)) {
    return std::unexpected(SecretError::kBadName);
  }
  const auto status = backend_->Remove(std::string(name));
  if (status == KeychainBackend::Status::kFailed) {
    return std::unexpected(SecretError::kIo);
  }
  return {};
}

std::vector<MovedSecret> MoveFileSecrets(const fs::path &dir, Secrets &to) {
  std::vector<std::string> names;
  std::error_code ec;
  for (const std::string_view n : {kHostKey, kDbKey}) {
    if (fs::is_regular_file(dir / n, ec)) {
      names.emplace_back(n);
    }
  }
  for (const auto &entry : fs::directory_iterator(dir / "pucks", ec)) {
    const fs::path &p = entry.path();
    // Both a .link file and other entries are tested; the rest is the cleanup
    // of the extension's temporary path.
    if (entry.is_regular_file(ec) && p.extension() == ".link") {  // GCOVR_EXCL_BR_LINE
      names.push_back("pucks/" + p.filename().string());
    }
  }
  std::ranges::sort(names);

  FileSecrets files(dir);
  std::vector<MovedSecret> out;
  for (const std::string &name : names) {
    MovedSecret moved{.name = name,
                      .outcome = MovedSecret::Outcome::kFailed};  // GCOVR_EXCL_BR_LINE: cleanup
    auto bytes = files.Get(name);
    auto there = to.Get(name);
    if (!bytes) {
      out.push_back(moved);  // a file we can't read, or a bad name: left alone
      continue;
    }
    if (there) {
      moved.outcome =
          *there == *bytes ? MovedSecret::Outcome::kDuplicate : MovedSecret::Outcome::kConflict;
    } else if (there.error() == SecretError::kNotFound && to.Set(name, *bytes)) {
      auto back = to.Get(name);
      if (back && *back == *bytes) {
        moved.outcome = MovedSecret::Outcome::kMoved;
      }
      if (back) {
        crypto::Wipe(*back);
      }
    }
    const bool safe = moved.outcome == MovedSecret::Outcome::kMoved ||
                      moved.outcome == MovedSecret::Outcome::kDuplicate;
    if (safe && !Shred(dir / name, bytes->size())) {  // GCOVR_EXCL_BR_LINE: see below
      moved.outcome = MovedSecret::Outcome::kFailed;  // GCOVR_EXCL_LINE: it was just read
    }
    crypto::Wipe(*bytes);
    if (there) {
      crypto::Wipe(*there);
    }
    out.push_back(moved);
  }
  return out;
}

}  // namespace micnote::secrets
