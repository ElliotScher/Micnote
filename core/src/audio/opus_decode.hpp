// Opus -> PCM (stage 2 §4.5 step 2): a recording's packets decoded at
// 16 kHz mono, whisper's input rate, as floats in [-1, 1].
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace micnote::audio {

inline constexpr int kAsrRate = 16000;

enum class DecodeError : std::uint8_t {
  // A packet libopus rejects (the stream was damaged before encryption:
  // decryption already authenticated every byte).
  kBadPacket,
  kInternal,
};

struct DecodeStats {
  std::size_t packets = 0;
  std::size_t samples = 0;
};

std::expected<std::vector<float>, DecodeError> DecodeOpus(
    std::span<const std::vector<std::byte>> packets, DecodeStats *stats = nullptr);

}  // namespace micnote::audio
