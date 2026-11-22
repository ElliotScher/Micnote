#include "protocol/framing.hpp"

#include <pb_decode.h>
#include <pb_encode.h>

#include <cstdint>

namespace micnote::protocol {

std::expected<std::vector<std::byte>, FrameError> EncodeFrame(const micnote_v1_Frame &frame) {
  std::vector<std::byte> out(kMaxPrefixSize + kMaxFrameSize);
  pb_ostream_t stream =
      pb_ostream_from_buffer(reinterpret_cast<pb_byte_t *>(out.data()), out.size());
  // PB_ENCODE_DELIMITED writes the varint length prefix first.
  if (!pb_encode_ex(&stream, micnote_v1_Frame_fields, &frame, PB_ENCODE_DELIMITED)) {
    return std::unexpected(FrameError::kEncode);
  }
  out.resize(stream.bytes_written);
  return out;
}

void FrameDecoder::Push(std::span<const std::byte> bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::expected<std::optional<micnote_v1_Frame>, FrameError> FrameDecoder::Next() {
  if (error_) {
    return std::unexpected(*error_);
  }

  // The varint length prefix: 7 bits per byte, low bits first, high bit set
  // on every byte but the last.
  std::size_t length = 0;
  std::size_t prefix = 0;
  for (;; ++prefix) {
    if (prefix == buffer_.size()) {
      return std::nullopt;
    }
    if (prefix == kMaxPrefixSize) {
      return Fail(FrameError::kBadPrefix);
    }
    const auto byte = std::to_integer<std::size_t>(buffer_.at(prefix));
    length |= (byte & 0x7FU) << (7U * prefix);
    if ((byte & 0x80U) == 0U) {
      ++prefix;
      break;
    }
  }
  if (length > kMaxFrameSize) {
    return Fail(FrameError::kTooLarge);
  }
  if (buffer_.size() - prefix < length) {
    return std::nullopt;
  }

  micnote_v1_Frame frame = micnote_v1_Frame_init_zero;
  pb_istream_t stream =
      pb_istream_from_buffer(reinterpret_cast<const pb_byte_t *>(buffer_.data()) + prefix, length);
  if (!pb_decode(&stream, micnote_v1_Frame_fields, &frame)) {
    return Fail(FrameError::kDecode);
  }
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(prefix + length));
  return frame;
}

std::unexpected<FrameError> FrameDecoder::Fail(FrameError error) {
  error_ = error;
  buffer_.clear();
  return std::unexpected(error);
}

}  // namespace micnote::protocol
