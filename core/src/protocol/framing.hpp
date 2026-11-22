// Link framing (docs/protocol.md section 2): every message is a varint
// length followed by an encoded micnote.v1.Frame. The same byte stream runs
// over L2CAP, the GATT fallback and USB CDC, and a frame may arrive split
// across any number of transport packets, so decoding is incremental.
// The firmware's mirror is firmware/app/src/proto_link/framing.c.
#pragma once

#include <micnote.pb.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace micnote::protocol {

// The largest encoded Frame, from micnote.options.
inline constexpr std::size_t kMaxFrameSize = micnote_v1_Frame_size;
// Bytes in the length prefix of the largest frame. A longer prefix is
// rejected rather than parsed, so a hostile peer can't make the decoder
// shift past 32 bits.
inline constexpr std::size_t kMaxPrefixSize = 2;
static_assert(kMaxFrameSize < (1U << (7U * kMaxPrefixSize)),
              "kMaxPrefixSize no longer covers kMaxFrameSize");

enum class FrameError : std::uint8_t {
  // nanopb couldn't encode the Frame (e.g. a string without its NUL).
  kEncode,
  // The length prefix is longer than kMaxPrefixSize bytes.
  kBadPrefix,
  // The length prefix is larger than kMaxFrameSize.
  kTooLarge,
  // The frame's bytes aren't a valid Frame.
  kDecode,
};

// Returns the length prefix followed by the encoded frame.
std::expected<std::vector<std::byte>, FrameError> EncodeFrame(const micnote_v1_Frame &frame);

// Reassembles frames from a byte stream. One decoder per link: after any
// error the stream has lost sync, so every later Next() returns that same
// error and the caller should drop the link.
class FrameDecoder {
 public:
  // Appends bytes received from the transport.
  void Push(std::span<const std::byte> bytes);

  // The next complete frame; std::nullopt if more bytes are needed.
  std::expected<std::optional<micnote_v1_Frame>, FrameError> Next();

  // Bytes received but not yet returned as a frame.
  [[nodiscard]] std::size_t Buffered() const { return buffer_.size(); }

 private:
  std::unexpected<FrameError> Fail(FrameError error);

  std::vector<std::byte> buffer_;
  std::optional<FrameError> error_;
};

}  // namespace micnote::protocol
