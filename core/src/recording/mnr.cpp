#include "recording/mnr.hpp"

#include <algorithm>

namespace micnote::recording {

namespace {

template <typename T>
T LoadLe(std::span<const std::byte> at) {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(static_cast<T>(std::to_integer<std::uint8_t>(at[i])) << (8U * i));
  }
  return value;
}

template <std::size_t N>
std::array<std::byte, N> Take(std::span<const std::byte> at) {
  std::array<std::byte, N> out{};
  std::ranges::copy(at.first(N), out.begin());
  return out;
}

constexpr std::array<std::byte, 4> kMagic{std::byte{'M'}, std::byte{'N'}, std::byte{'R'},
                                          std::byte{'1'}};
// Offsets, as in firmware/app/src/storage/mnr.c.
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffHdrLen = 5;
constexpr std::size_t kOffFlags = 7;
constexpr std::size_t kOffRecId = 8;
constexpr std::size_t kOffCodec = 16;
constexpr std::size_t kOffRateHz = 17;
constexpr std::size_t kOffFrameMs = 19;
constexpr std::size_t kOffStartBoot = 20;
constexpr std::size_t kOffStartUptime = 24;
constexpr std::size_t kOffStartUnix = 32;
constexpr std::size_t kOffNWraps = kCoreSize;
constexpr std::size_t kOffWraps = kCoreSize + 1;

}  // namespace

std::uint16_t LoadLe16(std::span<const std::byte> at) { return LoadLe<std::uint16_t>(at); }
std::uint32_t LoadLe32(std::span<const std::byte> at) { return LoadLe<std::uint32_t>(at); }
std::uint64_t LoadLe64(std::span<const std::byte> at) { return LoadLe<std::uint64_t>(at); }

std::expected<Header, FormatError> ParseHeader(std::span<const std::byte> file) {
  if (file.size() < kOffFlags) {
    return std::unexpected(FormatError::kTruncated);
  }
  if (!std::ranges::equal(file.first(kMagic.size()), kMagic)) {
    return std::unexpected(FormatError::kBadMagic);
  }
  if (std::to_integer<std::uint8_t>(file[kOffVersion]) != kVersion) {
    return std::unexpected(FormatError::kUnsupportedVersion);
  }
  const std::size_t length = LoadLe16(file.subspan(kOffHdrLen));
  if (length < kHeaderSize || length > kHeaderMax) {
    return std::unexpected(FormatError::kBadHeaderLength);
  }
  if (file.size() < length) {
    return std::unexpected(FormatError::kTruncated);
  }
  const std::size_t n_wraps = std::to_integer<std::uint8_t>(file[kOffNWraps]);
  if (n_wraps > kMaxWraps) {
    return std::unexpected(FormatError::kTooManyWraps);
  }

  Header h;
  h.flags = std::to_integer<std::uint8_t>(file[kOffFlags]);
  h.rec_id = LoadLe64(file.subspan(kOffRecId));
  h.codec = std::to_integer<std::uint8_t>(file[kOffCodec]);
  h.rate_hz = LoadLe16(file.subspan(kOffRateHz));
  h.frame_ms = std::to_integer<std::uint8_t>(file[kOffFrameMs]);
  h.start_boot = LoadLe32(file.subspan(kOffStartBoot));
  h.start_uptime_ms = LoadLe64(file.subspan(kOffStartUptime));
  h.start_unix_ms = LoadLe64(file.subspan(kOffStartUnix));
  h.length = length;
  for (std::size_t i = 0; i < n_wraps; ++i) {
    const auto w = file.subspan(kOffWraps + (i * kWrapSize), kWrapSize);
    h.wraps.push_back({.host_id = Take<8>(w),
                       .eph_pub = Take<32>(w.subspan(8)),
                       .wrapped_cek = Take<32>(w.subspan(40)),
                       .tag = Take<16>(w.subspan(72))});
  }
  return h;
}

std::optional<ChunkHeader> ParseChunkHeader(std::span<const std::byte, kChunkHeaderSize> bytes) {
  const std::uint16_t ct_len = LoadLe16(bytes.subspan(4));
  if (ct_len == 0 || ct_len > kMaxCiphertext) {
    return std::nullopt;
  }
  return ChunkHeader{.seq = LoadLe32(bytes), .ct_len = ct_len};
}

}  // namespace micnote::recording
