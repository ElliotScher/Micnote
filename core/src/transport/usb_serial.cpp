#include "transport/usb_serial.hpp"

#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>

#include <windows.h>
#else
#include <cerrno>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace micnote::transport {

#if defined(_WIN32)

// Windows: not built or run by the coverage job (Linux); exercised by the
// Windows CI build and by hand against a puck.

namespace {

HANDLE H(std::intptr_t handle) { return reinterpret_cast<HANDLE>(handle); }

// A disconnected USB CDC device reports one of these.
bool IsDisconnect(DWORD error) {
  return error == ERROR_ACCESS_DENIED || error == ERROR_BAD_COMMAND ||
         error == ERROR_OPERATION_ABORTED || error == ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace

std::expected<std::unique_ptr<UsbSerial>, TransportError> UsbSerial::Open(const std::string &path) {
  // COM10 and above only open through the device namespace; it works for
  // every port, so always use it for bare COMn names.
  const std::string name = path.starts_with("COM") ? "\\\\.\\" + path : path;
  HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return std::unexpected(TransportError::kIo);
  }
  DCB dcb{};
  dcb.DCBlength = sizeof(dcb);
  if (GetCommState(h, &dcb) == 0) {
    CloseHandle(h);
    return std::unexpected(TransportError::kIo);
  }
  dcb.BaudRate = CBR_115200;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fParity = FALSE;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  dcb.fRtsControl = RTS_CONTROL_ENABLE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  if (SetCommState(h, &dcb) == 0) {
    CloseHandle(h);
    return std::unexpected(TransportError::kIo);
  }
  return std::unique_ptr<UsbSerial>(new UsbSerial(reinterpret_cast<std::intptr_t>(h)));
}

UsbSerial::~UsbSerial() { CloseHandle(H(handle_)); }

std::expected<void, TransportError> UsbSerial::Send(std::span<const std::byte> bytes) {
  // No write timeouts: WriteFile returns once the driver has the bytes.
  COMMTIMEOUTS t{};
  SetCommTimeouts(H(handle_), &t);
  while (!bytes.empty()) {
    DWORD written = 0;
    const auto n = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1U << 20U));
    if (WriteFile(H(handle_), bytes.data(), n, &written, nullptr) == 0) {
      return std::unexpected(IsDisconnect(GetLastError()) ? TransportError::kClosed
                                                          : TransportError::kIo);
    }
    bytes = bytes.subspan(written);
  }
  return {};
}

std::expected<std::size_t, TransportError> UsbSerial::Receive(std::span<std::byte> buf,
                                                              std::chrono::milliseconds timeout) {
  // MAXDWORD / MAXDWORD / constant: return at once with whatever has
  // arrived, or wait up to the constant for the first byte.
  COMMTIMEOUTS t{};
  t.ReadIntervalTimeout = MAXDWORD;
  t.ReadTotalTimeoutMultiplier = MAXDWORD;
  t.ReadTotalTimeoutConstant =
      static_cast<DWORD>(std::clamp<std::int64_t>(timeout.count(), 1, MAXDWORD - 1));
  if (SetCommTimeouts(H(handle_), &t) == 0) {
    return std::unexpected(TransportError::kIo);
  }
  DWORD got = 0;
  const auto n = static_cast<DWORD>(std::min<std::size_t>(buf.size(), 1U << 20U));
  if (ReadFile(H(handle_), buf.data(), n, &got, nullptr) == 0) {
    return std::unexpected(IsDisconnect(GetLastError()) ? TransportError::kClosed
                                                        : TransportError::kIo);
  }
  return got;
}

#else

std::expected<std::unique_ptr<UsbSerial>, TransportError> UsbSerial::Open(const std::string &path) {
  // O_NOCTTY: never become the port's controlling terminal.
  const int fd = open(path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);  // NOLINT(*-vararg)
  if (fd < 0) {
    return std::unexpected(TransportError::kIo);
  }
  termios tio{};
  if (tcgetattr(fd, &tio) != 0) {
    // Not a terminal at all.
    close(fd);
    return std::unexpected(TransportError::kIo);
  }
  // Raw bytes: no echo, no line editing, no CR/LF translation, no signals.
  cfmakeraw(&tio);
  tio.c_cflag |= CLOCAL | CREAD;
  cfsetispeed(&tio, B115200);
  cfsetospeed(&tio, B115200);
  // GCOVR_EXCL_START: tcsetattr fails only on a device that accepted
  // tcgetattr but refuses raw mode, which no test terminal does.
  if (tcsetattr(fd, TCSANOW, &tio) != 0) {
    close(fd);
    return std::unexpected(TransportError::kIo);
  }
  // GCOVR_EXCL_STOP
  // The constructor is private (no make_unique), and only stores an int, so
  // new's "constructor threw" cleanup branch can't run.
  return std::unique_ptr<UsbSerial>(new UsbSerial(fd));  // GCOVR_EXCL_BR_LINE
}

UsbSerial::~UsbSerial() { close(static_cast<int>(handle_)); }

namespace {

// EIO is what a terminal returns once its device is gone: an unplugged USB
// port, or a pseudo-terminal whose other end closed.
// No test can make a terminal fail with anything but EIO.
TransportError FromErrno() {
  return errno == EIO ? TransportError::kClosed : TransportError::kIo;  // GCOVR_EXCL_BR_LINE
}

}  // namespace

std::expected<void, TransportError> UsbSerial::Send(std::span<const std::byte> bytes) {
  const int fd = static_cast<int>(handle_);
  while (!bytes.empty()) {
    const ssize_t n = write(fd, bytes.data(), bytes.size());
    if (n < 0) {
      return std::unexpected(FromErrno());
    }
    bytes = bytes.subspan(static_cast<std::size_t>(n));
  }
  return {};
}

std::expected<std::size_t, TransportError> UsbSerial::Receive(std::span<std::byte> buf,
                                                              std::chrono::milliseconds timeout) {
  const int fd = static_cast<int>(handle_);
  pollfd p{.fd = fd, .events = POLLIN, .revents = 0};
  const int ready = poll(&p, 1, static_cast<int>(timeout.count()));
  // GCOVR_EXCL_START: poll on a valid descriptor fails only on a signal or
  // out of memory, which no test can arrange.
  if (ready < 0) {
    return std::unexpected(TransportError::kIo);
  }
  // GCOVR_EXCL_STOP
  if (ready == 0) {
    return 0;
  }
  const ssize_t n = read(fd, buf.data(), buf.size());
  if (n <= 0) {
    // 0 is end of file; both mean the device went away (see FromErrno).
    // Linux reports a closed pseudo-terminal as EIO; macOS and some USB
    // drivers as end of file.
    return std::unexpected(n == 0 ? TransportError::kClosed : FromErrno());  // GCOVR_EXCL_BR_LINE
  }
  return static_cast<std::size_t>(n);
}

#endif

}  // namespace micnote::transport
