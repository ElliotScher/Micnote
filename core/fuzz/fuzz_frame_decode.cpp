// Feeds arbitrary bytes to the link FrameDecoder, as a hostile puck, host or
// radio could. The first input byte picks a split point, so frames that
// arrive across two transport packets get exercised too.
#include <cstddef>
#include <cstdint>
#include <span>

#include "protocol/framing.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  const auto rest = std::as_bytes(std::span(data, size)).subspan(1);
  const std::size_t split = data[0] % size;  // 0 ... rest.size()

  micnote::protocol::FrameDecoder decoder;
  for (const auto part : {rest.first(split), rest.subspan(split)}) {
    decoder.Push(part);
    for (;;) {
      auto frame = decoder.Next();
      if (!frame || !*frame) {
        break;
      }
      // Anything the decoder accepts must encode again: strings came back
      // NUL-terminated and every size is within micnote.options.
      if (!micnote::protocol::EncodeFrame(**frame)) {
        __builtin_trap();
      }
    }
  }
  return 0;
}
