// The .mnr recording format (docs/protocol.md section 6), parse side: the
// host's own implementation of what firmware/app/src/storage/mnr.c writes.
// proto/testvectors checks that the two agree. All integers little-endian.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace micnote::recording {

inline constexpr std::size_t kCoreSize = 40;
inline constexpr std::size_t kWrapSize = 88;
inline constexpr std::size_t kMaxWraps = 4;
inline constexpr std::size_t kHeaderSize = kCoreSize + 1 + (kMaxWraps * kWrapSize);  // 393
inline constexpr std::size_t kHeaderMax = 512;
inline constexpr std::size_t kChunkHeaderSize = 6;
inline constexpr std::size_t kTagSize = 16;
inline constexpr std::size_t kMaxCiphertext = 4096;
inline constexpr std::uint8_t kVersion = 1;

struct Wrap {
  std::array<std::byte, 8> host_id{};
  std::array<std::byte, 32> eph_pub{};
  std::array<std::byte, 32> wrapped_cek{};
  std::array<std::byte, 16> tag{};
};

struct Header {
  std::uint8_t flags = 0;
  std::uint64_t rec_id = 0;
  std::uint8_t codec = 0;
  std::uint16_t rate_hz = 0;
  std::uint8_t frame_ms = 0;
  std::uint32_t start_boot = 0;
  std::uint64_t start_uptime_ms = 0;
  std::uint64_t start_unix_ms = 0;
  std::vector<Wrap> wraps;
  // hdr_len: where the first chunk record starts.
  std::size_t length = 0;
};

enum class FormatError : std::uint8_t {
  kTruncated,
  kBadMagic,
  kUnsupportedVersion,
  kBadHeaderLength,
  kTooManyWraps,
};

std::expected<Header, FormatError> ParseHeader(std::span<const std::byte> file);

struct ChunkHeader {
  std::uint32_t seq = 0;
  std::uint16_t ct_len = 0;
};

// std::nullopt if ct_len is 0 or above kMaxCiphertext.
std::optional<ChunkHeader> ParseChunkHeader(std::span<const std::byte, kChunkHeaderSize> bytes);

// Little-endian reads from a span the caller has bounds-checked.
std::uint16_t LoadLe16(std::span<const std::byte> at);
std::uint32_t LoadLe32(std::span<const std::byte> at);
std::uint64_t LoadLe64(std::span<const std::byte> at);

}  // namespace micnote::recording
