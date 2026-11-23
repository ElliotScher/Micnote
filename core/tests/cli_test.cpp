// micnote-cli's commands, run through cli::Run() against the fake puck: what
// each command prints and returns, every usage error, and every failure.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "commands.hpp"
#include "fake_puck.hpp"

#if !defined(_WIN32)
#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using micnote::cli::Env;
using micnote::protocol::ClientError;
using micnote::recording::ReadError;
using micnote::test::FakePuck;
using micnote::transport::Transport;
using micnote::transport::TransportError;

// Forwards to a FakePuck the test keeps, so the test can inspect it after
// the command has taken (and dropped) its transport.
class Borrowed final : public Transport {
 public:
  explicit Borrowed(Transport &to) : to_(&to) {}
  std::expected<void, TransportError> Send(std::span<const std::byte> bytes) override {
    return to_->Send(bytes);
  }
  std::expected<std::size_t, TransportError> Receive(std::span<std::byte> buf,
                                                     std::chrono::milliseconds timeout) override {
    return to_->Receive(buf, timeout);
  }

 private:
  Transport *to_;
};

constexpr std::string_view kHostA = "host_a";

class CliTest : public ::testing::Test {
 public:
  CliTest() {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("micnote-cli-test-" + std::to_string(rd()));
    fs::create_directories(dir_);
    micnote::test::LoadVectors(puck_);
    WriteKey(dir_ / "host.key", kHostA);
  }
  CliTest(const CliTest &) = delete;
  CliTest &operator=(const CliTest &) = delete;
  CliTest(CliTest &&) = delete;
  CliTest &operator=(CliTest &&) = delete;
  ~CliTest() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

 protected:
  static void WriteKey(const fs::path &path, std::string_view name) {
    const std::string hex =
        micnote::test::Vectors()["keys"][std::string(name)]["priv"].get<std::string>();
    std::ofstream file(path, std::ios::binary);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      file.put(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
  }

  // Runs a command line; the default key is dir/host.key unless overridden.
  int Run(std::vector<std::string> args) {
    out_.str({});
    err_.str({});
    const std::vector<std::string_view> views(args.begin(), args.end());
    const Env env{.out = &out_,
                  .err = &err_,
                  .open = open_,
                  .now_unix_ms = [] { return std::uint64_t{1763800000000}; },
                  .default_key = default_key_};
    return micnote::cli::Run(views, env);
  }

  [[nodiscard]] std::string out() const { return out_.str(); }
  [[nodiscard]] std::string err() const { return err_.str(); }
  [[nodiscard]] const fs::path &dir() const { return dir_; }
  FakePuck &puck() { return puck_; }
  void set_default_key(fs::path p) { default_key_ = std::move(p); }
  void set_open(micnote::cli::Opener o) { open_ = std::move(o); }

 private:
  fs::path dir_;
  FakePuck puck_;
  std::ostringstream out_;
  std::ostringstream err_;
  fs::path default_key_ = fs::temp_directory_path() / "unused";
  micnote::cli::Opener open_ =
      [this](const std::string &path) -> std::expected<std::unique_ptr<Transport>, TransportError> {
    if (path != "fake") {
      return std::unexpected(TransportError::kIo);
    }
    return std::make_unique<Borrowed>(puck_);
  };

 public:
  // --key for the fixture's host_a key.
  [[nodiscard]] std::string key() const { return (dir_ / "host.key").string(); }
};

}  // namespace

// ---- usage -----------------------------------------------------------------

TEST_F(CliTest, VersionAndHelp) {
  EXPECT_EQ(Run({"--version"}), 0);
  EXPECT_TRUE(out().starts_with("micnote-cli "));
  EXPECT_EQ(Run({"-V"}), 0);
  EXPECT_EQ(Run({"--help"}), 0);
  EXPECT_TRUE(out().starts_with("usage: micnote-cli"));
  EXPECT_EQ(Run({"-h"}), 0);
}

TEST_F(CliTest, UsageErrors) {
  for (const std::vector<std::string> &args : std::vector<std::vector<std::string>>{
           {},
           {"frobnicate"},
           {"--version", "--help"},
           {"keygen", "--usb", "x"},
           {"status"},
           {"list", "--key"},
           {"list", "--usb", "a", "--usb", "b"},
           {"list", "--usb", "a", "--no-ack"},
           {"sync", "--usb", "fake"},
           {"status", "--usb", "fake", "--out", "d"},
           {"sync", "--usb", "fake", "--out", "d", "--bogus"},
       }) {
    std::string joined;
    for (const auto &a : args) {
      joined += a + " ";
    }
    EXPECT_EQ(Run(args), 2) << joined;
    EXPECT_TRUE(err().starts_with("usage: micnote-cli")) << joined;
  }
}

// ---- keys ------------------------------------------------------------------

TEST_F(CliTest, KeygenWritesAPrivateKey) {
  const fs::path key = dir() / "new" / "dir" / "host.key";
  ASSERT_EQ(Run({"keygen", "--key", key.string()}), 0) << err();
  EXPECT_EQ(fs::file_size(key), 32U);
  EXPECT_NE(out().find("host_id "), std::string::npos);
#if !defined(_WIN32)
  EXPECT_EQ(fs::status(key).permissions(), fs::perms::owner_read | fs::perms::owner_write);
#endif
  EXPECT_EQ(Run({"keygen", "--key", key.string()}), 1);
  EXPECT_NE(err().find("already exists"), std::string::npos);
}

TEST_F(CliTest, KeygenUsesTheDefaultPath) {
  set_default_key(dir() / "default.key");
  ASSERT_EQ(Run({"keygen"}), 0) << err();
  EXPECT_TRUE(fs::exists(dir() / "default.key"));
}

TEST_F(CliTest, KeygenCantWrite) {
  // The "directory" is a file.
  const fs::path blocker = dir() / "file";
  std::ofstream(blocker) << "x";
  EXPECT_EQ(Run({"keygen", "--key", (blocker / "host.key").string()}), 1);
  EXPECT_NE(err().find("can't write"), std::string::npos);
}

TEST_F(CliTest, NoKeyPathAtAll) {
  set_default_key({});
  EXPECT_EQ(Run({"keygen"}), 1);
  EXPECT_NE(err().find("pass --key"), std::string::npos);
  EXPECT_EQ(Run({"status", "--usb", "fake"}), 1);
}

TEST_F(CliTest, BadKeyFiles) {
  EXPECT_EQ(Run({"status", "--usb", "fake", "--key", (dir() / "missing").string()}), 1);
  EXPECT_NE(err().find("run: micnote-cli keygen"), std::string::npos);
  const fs::path short_key = dir() / "short";
  std::ofstream(short_key) << "too short";
  EXPECT_EQ(Run({"status", "--usb", "fake", "--key", short_key.string()}), 1);
  EXPECT_NE(err().find("isn't a host key"), std::string::npos);
}

TEST(CliDefaultKeyPath, FollowsTheEnvironment) {
#if defined(_WIN32)
  const auto path = micnote::cli::DefaultKeyPath(
      [](const char *n) { return std::string_view(n) == "APPDATA" ? "C:\\Users\\a" : nullptr; });
  EXPECT_EQ(path, fs::path("C:\\Users\\a") / "micnote" / "host.key");
#else
  EXPECT_EQ(micnote::cli::DefaultKeyPath([](const char *n) {
              return std::string_view(n) == "XDG_CONFIG_HOME" ? "/x" : "/home/a";
            }),
            fs::path("/x/micnote/host.key"));
  EXPECT_EQ(micnote::cli::DefaultKeyPath([](const char *n) {
              return std::string_view(n) == "XDG_CONFIG_HOME" ? "" : "/home/a";
            }),
            fs::path("/home/a/.config/micnote/host.key"));
#endif
  EXPECT_EQ(micnote::cli::DefaultKeyPath([](const char *) -> const char * { return nullptr; }),
            fs::path());
}

// ---- connecting ------------------------------------------------------------

TEST_F(CliTest, CantOpenPort) {
  EXPECT_EQ(Run({"status", "--usb", "/dev/nothing", "--key", key()}), 1);
  EXPECT_NE(err().find("can't open /dev/nothing"), std::string::npos);
  EXPECT_EQ(Run({"list", "--usb", "/dev/nothing", "--key", key()}), 1);
}

TEST_F(CliTest, HelloOrClockFails) {
  puck().error_on = micnote_v1_Frame_hello_tag;
  EXPECT_EQ(Run({"status", "--usb", "fake", "--key", key()}), 1);
  EXPECT_NE(err().find("hello failed: the puck said: busy"), std::string::npos);
  puck().error_on = micnote_v1_Frame_set_time_tag;
  EXPECT_EQ(Run({"status", "--usb", "fake", "--key", key()}), 1);
  EXPECT_NE(err().find("setting the clock failed"), std::string::npos);
  EXPECT_EQ(puck().last_set_time, 0U);
}

// ---- status and list -------------------------------------------------------

TEST_F(CliTest, Status) {
  for (const auto &[source, text] : std::vector<std::pair<micnote_v1_ChargeSource, std::string>>{
           {micnote_v1_ChargeSource_CHARGE_SOURCE_NONE, "on battery"},
           {micnote_v1_ChargeSource_CHARGE_SOURCE_USB, "charging over USB"},
           {micnote_v1_ChargeSource_CHARGE_SOURCE_QI, "charging on Qi"}}) {
    puck().status_charge = source;
    ASSERT_EQ(Run({"status", "--usb", "fake", "--key", key()}), 0) << err();
    EXPECT_NE(out().find("battery   87%"), std::string::npos) << out();
    EXPECT_NE(out().find(text), std::string::npos) << out();
    EXPECT_NE(out().find("3 recordings"), std::string::npos) << out();
  }
  EXPECT_NE(out().find("clock     not set"), std::string::npos) << out();
  puck().status_rtc_valid = true;
  ASSERT_EQ(Run({"status", "--usb", "fake", "--key", key()}), 0) << err();
  EXPECT_NE(out().find("clock     set"), std::string::npos) << out();
  EXPECT_EQ(puck().last_set_time, 1763800000000U) << "the puck's clock was set";
  puck().error_on = micnote_v1_Frame_get_status_tag;
  EXPECT_EQ(Run({"status", "--usb", "fake", "--key", key()}), 1);
  EXPECT_NE(err().find("status failed"), std::string::npos);
}

TEST_F(CliTest, List) {
  puck().incomplete_id = 0x0000000200000001ULL;
  ASSERT_EQ(Run({"list", "--usb", "fake", "--key", key()}), 0) << err();
  EXPECT_NE(out().find("0000000100000001"), std::string::npos) << out();
  EXPECT_NE(out().find("0000000100000002"), std::string::npos) << out();
  EXPECT_NE(out().find("0000000200000001"), std::string::npos) << out();
  EXPECT_NE(out().find("(recording)"), std::string::npos) << out();

  puck().error_on = micnote_v1_Frame_list_recordings_tag;
  EXPECT_EQ(Run({"list", "--usb", "fake", "--key", key()}), 1);
  EXPECT_NE(err().find("list failed"), std::string::npos);

  puck().error_on.reset();
  puck().recordings.clear();
  ASSERT_EQ(Run({"list", "--usb", "fake", "--key", key()}), 0);
  EXPECT_EQ(out(), "no recordings\n");
}

// ---- sync ------------------------------------------------------------------

TEST_F(CliTest, SyncFetchesDecryptsWritesAndAcks) {
  const fs::path out_dir = dir() / "out";
  // The fallback recording isn't encrypted to host_a: it must stay on the
  // puck, and the command reports the partial sync.
  EXPECT_EQ(Run({"sync", "--usb", "fake", "--out", out_dir.string(), "--key", key()}), 1);
  EXPECT_NE(out().find("0000000100000002: can't decrypt (not encrypted to this host)"),
            std::string::npos)
      << out();
  EXPECT_NE(out().find("synced 2 of 3 recordings"), std::string::npos) << out();
  EXPECT_EQ(fs::file_size(out_dir / "0000000100000001.mnr"),
            micnote::test::ReadFile("rec_two_hosts.mnr").size());
  EXPECT_TRUE(fs::exists(out_dir / "0000000200000001.mnr"));
  EXPECT_FALSE(fs::exists(out_dir / "0000000100000002.mnr"));
  ASSERT_EQ(puck().recordings.size(), 1U);
  EXPECT_TRUE(puck().recordings.contains(0x0000000100000002ULL));

  // What's left is the fallback recording; with it gone too, all is synced.
  puck().recordings.clear();
  EXPECT_EQ(Run({"sync", "--usb", "fake", "--out", out_dir.string(), "--key", key()}), 0);
  EXPECT_NE(out().find("synced 0 of 0"), std::string::npos);
}

TEST_F(CliTest, SyncNoAckKeepsEverything) {
  puck().recordings.erase(0x0000000100000002ULL);
  ASSERT_EQ(
      Run({"sync", "--usb", "fake", "--out", (dir() / "o").string(), "--no-ack", "--key", key()}),
      0)
      << err();
  EXPECT_NE(out().find("kept on the puck (--no-ack)"), std::string::npos);
  EXPECT_EQ(puck().recordings.size(), 2U);
}

TEST_F(CliTest, SyncSkipsRecordingInProgress) {
  puck().recordings.erase(0x0000000100000002ULL);
  puck().incomplete_id = 0x0000000200000001ULL;
  EXPECT_EQ(Run({"sync", "--usb", "fake", "--out", (dir() / "o").string(), "--key", key()}), 1);
  EXPECT_NE(out().find("0000000200000001: still recording, skipped"), std::string::npos);
  EXPECT_TRUE(puck().recordings.contains(0x0000000200000001ULL));
}

TEST_F(CliTest, SyncFailures) {
  const std::string o = (dir() / "o").string();
  puck().error_on = micnote_v1_Frame_list_recordings_tag;
  EXPECT_EQ(Run({"sync", "--usb", "fake", "--out", o, "--key", key()}), 1);
  EXPECT_NE(err().find("list failed"), std::string::npos);

  puck().error_on = micnote_v1_Frame_fetch_tag;
  EXPECT_EQ(Run({"sync", "--usb", "fake", "--out", o, "--key", key()}), 1);
  EXPECT_NE(err().find("fetch failed"), std::string::npos);

  puck().error_on = micnote_v1_Frame_ack_tag;
  EXPECT_EQ(Run({"sync", "--usb", "fake", "--out", o, "--key", key()}), 1);
  EXPECT_NE(err().find("ack failed"), std::string::npos);
  EXPECT_EQ(puck().recordings.size(), 3U);

  EXPECT_EQ(Run({"sync", "--usb", "nope", "--out", o, "--key", key()}), 1);
  EXPECT_NE(err().find("can't open nope"), std::string::npos);
}

TEST_F(CliTest, SyncCantWriteOutput) {
  const fs::path blocker = dir() / "blocker";
  std::ofstream(blocker) << "x";
  EXPECT_EQ(Run({"sync", "--usb", "fake", "--out", blocker.string(), "--key", key()}), 1);
  EXPECT_NE(err().find("can't create"), std::string::npos);

  // The output file's name is taken by a directory.
  const fs::path o = dir() / "o";
  fs::create_directories(o / "0000000100000001.mnr");
  EXPECT_EQ(Run({"sync", "--usb", "fake", "--out", o.string(), "--key", key()}), 1);
  EXPECT_NE(err().find("0000000100000001: can't write"), std::string::npos);
  EXPECT_TRUE(puck().recordings.contains(0x0000000100000001ULL)) << "not acked";
}

// ---- messages and the real world -------------------------------------------

TEST(CliDescribe, EveryError) {
  using Kind = ClientError::Kind;
  for (const Kind k : {Kind::kTransport, Kind::kTimeout, Kind::kFraming, Kind::kProtocol}) {
    EXPECT_FALSE(micnote::cli::Describe(ClientError{.kind = k, .device = {}}).empty());
  }
  for (int code = 0; code <= 10; ++code) {
    EXPECT_FALSE(micnote::cli::DeviceErrorName(static_cast<micnote_v1_ErrorCode>(code)).empty());
  }
  for (const ReadError e :
       {ReadError::kFormat, ReadError::kNotARecipient, ReadError::kUnwrap, ReadError::kChunkFraming,
        ReadError::kChunkAuth, ReadError::kPlaintext, ReadError::kCrypto}) {
    EXPECT_FALSE(micnote::cli::Describe(e).empty());
  }
}

TEST(CliRealWorld, ClockIsNow) { EXPECT_GT(micnote::cli::SystemNowMs(), 1'700'000'000'000ULL); }

TEST(CliRealWorld, UsbOpener) {
  const auto opener = micnote::cli::UsbOpener();
  EXPECT_FALSE(opener("/nonexistent/ttyACM9").has_value());
#if !defined(_WIN32)
  const int master = posix_openpt(O_RDWR | O_NOCTTY);
  ASSERT_GE(master, 0);
  ASSERT_EQ(grantpt(master), 0);
  ASSERT_EQ(unlockpt(master), 0);
  EXPECT_TRUE(opener(ptsname(master)).has_value());  // NOLINT(concurrency-mt-unsafe)
  close(master);
#endif
}
