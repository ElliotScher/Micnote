// A link to a puck: BLE (L2CAP or GATT), USB CDC, or the mobile byte-pipe
// bridge (stage 1 section 4.14). It carries the framed byte stream of
// docs/protocol.md section 2; packet boundaries mean nothing.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace micnote::transport {

enum class TransportError : std::uint8_t {
  // The link is gone (disconnected, unplugged, out of range).
  kClosed,
  // Any other OS-level failure.
  kIo,
};

class Transport {
 public:
  Transport() = default;
  Transport(const Transport &) = delete;
  Transport &operator=(const Transport &) = delete;
  Transport(Transport &&) = delete;
  Transport &operator=(Transport &&) = delete;
  // GCOVR_EXCL_START: the deleting destructor only runs for an object whose
  // dynamic type is Transport, which is abstract.
  virtual ~Transport() = default;
  // GCOVR_EXCL_STOP

  // Sends all of `bytes`.
  virtual std::expected<void, TransportError> Send(std::span<const std::byte> bytes) = 0;

  // Waits up to `timeout` for bytes and copies up to buf.size() of them.
  // Returns how many; 0 means the timeout passed with nothing received.
  virtual std::expected<std::size_t, TransportError> Receive(std::span<std::byte> buf,
                                                             std::chrono::milliseconds timeout) = 0;
};

}  // namespace micnote::transport
