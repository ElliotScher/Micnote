// Feeds arbitrary bytes to the nanopb DeviceInfo decoder, as a hostile puck
// or radio could.
#include <micnote.pb.h>
#include <pb_decode.h>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  micnote_v1_DeviceInfo info = micnote_v1_DeviceInfo_init_zero;
  pb_istream_t in = pb_istream_from_buffer(data, size);
  if (pb_decode(&in, micnote_v1_DeviceInfo_fields, &info)) {
    // A decoded board name is always NUL-terminated within its buffer.
    if (info.board[sizeof info.board - 1] != '\0') {
      __builtin_trap();
    }
  }
  return 0;
}
