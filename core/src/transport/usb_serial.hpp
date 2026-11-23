// The puck's USB CDC ACM data interface as a Transport (stage 1 section
// 4.8): a serial port in raw mode. /dev/ttyACM* on Linux, /dev/cu.usbmodem*
// on macOS, COMn on Windows. The baud rate means nothing over USB CDC.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

#include "transport/transport.hpp"

namespace micnote::transport {

class UsbSerial final : public Transport {
 public:
  static std::expected<std::unique_ptr<UsbSerial>, TransportError> Open(const std::string &path);

  UsbSerial(const UsbSerial &) = delete;
  UsbSerial &operator=(const UsbSerial &) = delete;
  UsbSerial(UsbSerial &&) = delete;
  UsbSerial &operator=(UsbSerial &&) = delete;
  ~UsbSerial() override;

  std::expected<void, TransportError> Send(std::span<const std::byte> bytes) override;
  std::expected<std::size_t, TransportError> Receive(std::span<std::byte> buf,
                                                     std::chrono::milliseconds timeout) override;

 private:
  // A file descriptor on POSIX, a HANDLE on Windows.
  explicit UsbSerial(std::intptr_t handle) : handle_(handle) {}

  std::intptr_t handle_;
};

}  // namespace micnote::transport
