// UsbSerial over a pseudo-terminal: the test holds the master end and plays
// the puck. POSIX only; the Windows build is checked by CI and by hand.
#if !defined(_WIN32)

#include "transport/usb_serial.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using micnote::transport::TransportError;
using micnote::transport::UsbSerial;
using std::chrono::milliseconds;

class Pty {
 public:
  Pty() : master_(posix_openpt(O_RDWR | O_NOCTTY)) {
    EXPECT_GE(master_, 0);
    EXPECT_EQ(grantpt(master_), 0);
    EXPECT_EQ(unlockpt(master_), 0);
    path_ = ptsname(master_);  // NOLINT(concurrency-mt-unsafe): one thread
  }
  Pty(const Pty &) = delete;
  Pty &operator=(const Pty &) = delete;
  Pty(Pty &&) = delete;
  Pty &operator=(Pty &&) = delete;
  ~Pty() { CloseMaster(); }

  [[nodiscard]] const std::string &path() const { return path_; }
  [[nodiscard]] int master() const { return master_; }
  void CloseMaster() {
    if (master_ >= 0) {
      close(master_);
      master_ = -1;
    }
  }

 private:
  int master_;
  std::string path_;
};

std::vector<std::byte> AllByteValues() {
  std::vector<std::byte> out(256);
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::byte>(i);
  }
  return out;
}

// Reads exactly n bytes from fd.
std::vector<std::byte> ReadExactly(int fd, std::size_t n) {
  std::vector<std::byte> out(n);
  std::size_t got = 0;
  while (got < n) {
    const ssize_t r = read(fd, &out[got], n - got);
    if (r <= 0) {
      break;
    }
    got += static_cast<std::size_t>(r);
  }
  out.resize(got);
  return out;
}

std::unique_ptr<UsbSerial> OpenOk(const std::string &path) {
  auto port = UsbSerial::Open(path);
  EXPECT_TRUE(port.has_value()) << path;
  return port ? std::move(*port) : nullptr;
}

}  // namespace

TEST(UsbSerial, OpenMissingPortFails) {
  const auto port = UsbSerial::Open("/nonexistent/ttyACM9");
  ASSERT_FALSE(port.has_value());
  EXPECT_EQ(port.error(), TransportError::kIo);
}

TEST(UsbSerial, OpenNonTerminalFails) {
  const auto port = UsbSerial::Open("/dev/null");
  ASSERT_FALSE(port.has_value());
  EXPECT_EQ(port.error(), TransportError::kIo);
}

// Raw mode: every byte value passes through untouched in both directions
// (no echo, no CR/LF translation, no ^C or ^D handling).
TEST(UsbSerial, CarriesEveryByteValueBothWays) {
  Pty pty;
  const auto port = OpenOk(pty.path());
  ASSERT_NE(port, nullptr);
  const std::vector<std::byte> all = AllByteValues();

  ASSERT_TRUE(port->Send(all).has_value());
  EXPECT_EQ(ReadExactly(pty.master(), all.size()), all);

  ASSERT_EQ(write(pty.master(), all.data(), all.size()), static_cast<ssize_t>(all.size()));
  std::vector<std::byte> got;
  std::array<std::byte, 64> buf{};
  while (got.size() < all.size()) {
    const auto n = port->Receive(buf, milliseconds(1000));
    ASSERT_TRUE(n.has_value());
    ASSERT_GT(*n, 0U) << "timed out with " << got.size() << " bytes";
    got.insert(got.end(), buf.begin(), buf.begin() + static_cast<long>(*n));
  }
  EXPECT_EQ(got, all);
}

TEST(UsbSerial, ReceiveTimesOutWithNothing) {
  Pty pty;
  const auto port = OpenOk(pty.path());
  ASSERT_NE(port, nullptr);
  std::array<std::byte, 8> buf{};
  const auto n = port->Receive(buf, milliseconds(20));
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 0U);
}

// The other end going away (the puck unplugged) reads as kClosed.
TEST(UsbSerial, ClosedOtherEnd) {
  Pty pty;
  const auto port = OpenOk(pty.path());
  ASSERT_NE(port, nullptr);
  pty.CloseMaster();

  std::array<std::byte, 8> buf{};
  const auto n = port->Receive(buf, milliseconds(1000));
  ASSERT_FALSE(n.has_value());
  EXPECT_EQ(n.error(), TransportError::kClosed);
  const std::array<std::byte, 1> one{};
  const auto sent = port->Send(one);
  ASSERT_FALSE(sent.has_value());
  EXPECT_EQ(sent.error(), TransportError::kClosed);
}

#endif  // !_WIN32
