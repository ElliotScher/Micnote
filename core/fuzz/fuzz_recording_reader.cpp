// Feeds arbitrary bytes to the host's .mnr reader, as a hostile or broken
// puck could during sync. Decrypts as host_a from proto/testvectors, so with
// those files as the seed corpus the fuzzer starts from valid recordings:
//   fuzz_recording_reader corpus/ ../proto/testvectors
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "recording/reader.hpp"

namespace {

// host_a's private key (RFC 7748 section 6.1 "Bob").
constexpr std::array<std::uint8_t, 32> kHostA{
    0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
    0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd, 0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb};

const micnote::recording::HostKey &HostA() {
  static const auto kKey =
      micnote::recording::HostKey::FromPrivate(std::as_bytes(std::span(kHostA))).value();
  return kKey;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  const auto rec = micnote::recording::ReadRecording(std::as_bytes(std::span(data, size)), HostA());
  if (rec) {
    // Whatever decodes must be internally consistent.
    if (rec->trailer && rec->trailer->marker_count != rec->markers.size()) {
      __builtin_trap();
    }
  }
  return 0;
}
