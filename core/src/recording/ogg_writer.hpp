// Ogg Opus output (RFC 7845 over RFC 3533 Ogg): turns a decrypted recording
// into a .opus file any player opens, without re-encoding. The puck's packets
// go in as they are.
//
// - OpusHead: 1 channel, input rate 16 kHz, pre-skip 312 (libopus's encoder
//   lookahead at 16 kHz, 104 samples, in 48 kHz units), mapping family 0.
// - OpusTags: vendor "micnote", the recording ID and start time, and each
//   item marker as a chapter (CHAPTERnnn / CHAPTERnnnNAME, the Vorbis
//   chapter extension), which players such as VLC and foobar2000 show.
// - Audio pages hold up to 1 s of packets (50 at 20 ms), so a player can
//   seek to the second. Granule positions count 48 kHz samples, pre-skip
//   included; the last page's is the end of the audio.
// - The stream serial is the recording ID folded to 32 bits, so the same
//   recording always gives the same bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "recording/reader.hpp"

namespace micnote::recording {

// Opus at 48 kHz, whatever the input rate (RFC 7845 section 4).
inline constexpr std::uint32_t kOggGranuleRate = 48000;
inline constexpr std::uint16_t kOpusPreSkip = 312;
inline constexpr std::size_t kOggMaxPacketsPerPage = 50;

enum class OggError : std::uint8_t {
  // The header's codec isn't Opus.
  kUnsupportedCodec,
  // A packet is empty, has a TOC byte that isn't valid Opus, or is bigger
  // than one Ogg page can hold (65024 bytes; Opus packets are at most 1275).
  kBadPacket,
};

// The CRC Ogg puts in every page: CRC-32, polynomial 0x04C11DB7, not
// reflected, initial value 0, no final XOR. Exposed for tests.
std::uint32_t OggCrc(std::span<const std::byte> data);

std::expected<std::vector<std::byte>, OggError> WriteOggOpus(const Recording &rec);

// The OpusTags comments WriteOggOpus() writes, as "KEY=value". Exposed for
// tests.
std::vector<std::string> OpusComments(const Recording &rec);

}  // namespace micnote::recording
