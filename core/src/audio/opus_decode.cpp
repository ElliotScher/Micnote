#include "audio/opus_decode.hpp"

#include <memory>

#include <opus.h>

namespace micnote::audio {

namespace {

// 120 ms, the longest Opus frame, at 16 kHz.
constexpr int kMaxFrame = 1920;

struct DecoderDeleter {
  void operator()(OpusDecoder *d) const { opus_decoder_destroy(d); }
};

}  // namespace

std::expected<std::vector<float>, DecodeError> DecodeOpus(
    std::span<const std::vector<std::byte>> packets, DecodeStats *stats) {
  int err = OPUS_OK;
  const std::unique_ptr<OpusDecoder, DecoderDeleter> dec(opus_decoder_create(kAsrRate, 1, &err));
  // GCOVR_EXCL_START: creating a 16 kHz mono decoder only fails out of memory.
  if (err != OPUS_OK || dec == nullptr) {
    return std::unexpected(DecodeError::kInternal);
  }
  // GCOVR_EXCL_STOP
  std::vector<float> pcm;
  pcm.reserve(packets.size() * 320);
  std::vector<float> frame(kMaxFrame);
  for (const auto &p : packets) {
    const int n = opus_decode_float(dec.get(), reinterpret_cast<const unsigned char *>(p.data()),
                                    static_cast<opus_int32>(p.size()), frame.data(), kMaxFrame, 0);
    if (n < 0) {
      return std::unexpected(DecodeError::kBadPacket);
    }
    pcm.insert(pcm.end(), frame.begin(), frame.begin() + n);
  }
  if (stats != nullptr) {
    stats->packets = packets.size();
    stats->samples = pcm.size();
  }
  return pcm;
}

}  // namespace micnote::audio
