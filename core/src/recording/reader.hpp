// Reads a synced .mnr recording as a host: finds this host's key wrap,
// unwraps the CEK, authenticates and decrypts every chunk, and returns the
// Opus packets, item markers and trailer (stage 1 section 4.14).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "crypto/psa.hpp"
#include "recording/mnr.hpp"

namespace micnote::recording {

enum class ReadError : std::uint8_t {
  // The header doesn't parse (see FormatError).
  kFormat,
  // No key wrap for this host: it wasn't paired when the recording started.
  kNotARecipient,
  // This host's wrap doesn't authenticate (or names a low-order key).
  kUnwrap,
  // A chunk record is truncated, out of sequence or has a bad length.
  kChunkFraming,
  // A chunk doesn't authenticate: tampered, moved, or from another header.
  kChunkAuth,
  // A chunk decrypts but its contents are malformed.
  kPlaintext,
  // A PSA call failed for a reason other than authentication.
  kCrypto,
};

// A host install's X25519 key pair; host_id is SHA-256(pub)[:8].
class HostKey {
 public:
  static std::expected<HostKey, psa_status_t> FromPrivate(
      std::span<const std::byte, crypto::kX25519KeySize> priv);

  [[nodiscard]] const crypto::Key &key() const { return key_; }
  [[nodiscard]] const std::array<std::byte, crypto::kX25519KeySize> &pub() const { return pub_; }
  [[nodiscard]] const std::array<std::byte, 8> &host_id() const { return host_id_; }

 private:
  HostKey(crypto::Key key, const std::array<std::byte, crypto::kX25519KeySize> &pub);

  crypto::Key key_;
  std::array<std::byte, crypto::kX25519KeySize> pub_;
  std::array<std::byte, 8> host_id_{};
};

struct Marker {
  std::uint8_t type = 0;
  std::uint32_t offset_ms = 0;
};

struct Trailer {
  std::uint32_t duration_ms = 0;
  std::uint32_t marker_count = 0;
};

// Plaintext chunk layout (stage 1 section 4.2).
inline constexpr std::uint8_t kChunkAudio = 0x01;
inline constexpr std::uint8_t kChunkTrailer = 0x02;
inline constexpr std::uint16_t kMarkerSentinel = 0xFFFF;
inline constexpr std::size_t kMarkerSize = 7;
inline constexpr std::size_t kTrailerSize = 9;

struct Recording {
  Header header;
  std::vector<std::vector<std::byte>> packets;
  std::vector<Marker> markers;
  // Absent when a power cut ended the recording (the puck recovered it).
  std::optional<Trailer> trailer;
  std::uint32_t chunks = 0;

  // The trailer's duration, or the packets' after a power cut.
  [[nodiscard]] std::uint32_t DurationMs() const;
};

std::expected<Recording, ReadError> ReadRecording(std::span<const std::byte> file,
                                                  const HostKey &key);

// Stage 1 section 4.5: X25519, then HKDF-SHA256 into an AES-256-CCM KEK,
// then open this host's wrap of the CEK. Exposed for tests.
std::expected<std::array<std::byte, crypto::kAes256KeySize>, ReadError> UnwrapCek(
    const HostKey &host, std::uint64_t rec_id, const Wrap &w);

// One decrypted chunk's contents, appended to `into`. Exposed for tests.
std::expected<void, ReadError> ParsePlaintext(std::span<const std::byte> pt, Recording &into);

}  // namespace micnote::recording
