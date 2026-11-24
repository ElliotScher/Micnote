// micnote-cli's commands, run through cli::Run() against the fake puck: what
// each command prints and returns, every usage error, and every failure.
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "commands.hpp"
#include "crypto/pairing.hpp"
#include "fake_keychain.hpp"
#include "fake_puck.hpp"
#include "local.hpp"
#include "models.hpp"
#include "secrets/secrets.hpp"
#include "store/db.hpp"
#include "store/repos.hpp"

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

using micnote::test::Borrowed;

constexpr std::string_view kHostA = "host_a";

class CliTest : public ::testing::Test {
 public:
  CliTest() {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("micnote-cli-test-" + std::to_string(rd()));
    fs::create_directories(dir_);
    micnote::test::LoadVectors(puck_);
    WriteKey(dir_ / "host.key", kHostA);
    // host_a is paired with the fake puck, as the firmware would be.
    puck_.strict = true;
    puck_.hosts[HostId(kHostA)] = {.link_key = kLink, .name = "host_a"};
    WriteLink(dir_, kLink);
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
  static constexpr micnote::crypto::LinkKey kLink = [] {
    micnote::crypto::LinkKey k{};
    for (std::size_t i = 0; i < k.size(); ++i) {
      k.at(i) = static_cast<std::byte>(0x30 + i);
    }
    return k;
  }();

  static std::array<std::byte, 8> HostId(std::string_view name) {
    const auto key = micnote::test::KeyNamed(std::string(name));
    std::array<std::byte, 8> id{};
    std::ranges::copy(key.host_id(), id.begin());
    return id;
  }

  // The link key file the CLI reads for the fake puck (device 01..08), in
  // the pucks/ directory beside a host key in key_dir.
  static fs::path LinkFile(const fs::path &key_dir) {
    return key_dir / "pucks" / "0102030405060708.link";
  }
  static void WriteLink(const fs::path &key_dir, const micnote::crypto::LinkKey &key) {
    fs::create_directories(key_dir / "pucks");
    std::ofstream file(LinkFile(key_dir), std::ios::binary);
    file.write(reinterpret_cast<const char *>(key.data()),
               static_cast<std::streamsize>(key.size()));
  }

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
    const std::vector<std::string_view> views(args.begin(), args.end());
    return micnote::cli::Run(views, env());
  }

  // A fresh Env over this fixture (clears the captured output).
  Env env() {
    out_.str({});
    err_.str({});
    return Env{.out = &out_,
               .err = &err_,
               .open = open_,
               .now_unix_ms = [] { return std::uint64_t{1763800000000}; },
               .default_key = default_key_,
               .models_dir = {},
               .manifest_json = {},
               .device = {},
               .data_dir = data_dir_,
               .port_present = port_present_,
               .wait = wait_,
               .engine = engine_,
               .keychain = keychain_};
  }

  [[nodiscard]] std::string out() const { return out_.str(); }
  [[nodiscard]] std::string err() const { return err_.str(); }
  [[nodiscard]] const fs::path &dir() const { return dir_; }
  FakePuck &puck() { return puck_; }
  void set_default_key(fs::path p) { default_key_ = std::move(p); }
  void set_open(micnote::cli::Opener o) { open_ = std::move(o); }
  // For the commands on this machine's store (db, run).
  void set_data_dir(fs::path p) { data_dir_ = std::move(p); }
  void set_port_present(std::function<bool(const std::string &)> f) {
    port_present_ = std::move(f);
  }
  void set_wait(std::function<bool(std::chrono::milliseconds)> f) { wait_ = std::move(f); }
  void set_engine(micnote::jobs::EngineFactory f) { engine_ = std::move(f); }
  void set_keychain(micnote::secrets::KeychainBackend *k) { keychain_ = k; }

 private:
  fs::path dir_;
  FakePuck puck_;
  std::ostringstream out_;
  std::ostringstream err_;
  fs::path default_key_ = fs::temp_directory_path() / "unused";
  fs::path data_dir_;
  std::function<bool(const std::string &)> port_present_;
  std::function<bool(std::chrono::milliseconds)> wait_;
  micnote::jobs::EngineFactory engine_;
  micnote::secrets::KeychainBackend *keychain_ = nullptr;
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

// ---- authentication --------------------------------------------------------

TEST_F(CliTest, NeedsALinkKeyForThePuck) {
  fs::remove(LinkFile(dir()));
  EXPECT_EQ(Run({"status", "--usb", "fake", "--key", key()}), 1);
  EXPECT_NE(err().find("not paired with puck 0102030405060708"), std::string::npos) << err();
  EXPECT_NE(err().find("run: micnote-cli pair --usb fake"), std::string::npos) << err();
}

TEST_F(CliTest, RefusesAPuckWithTheWrongProof) {
  auto other = kLink;
  other[0] ^= std::byte{1};
  WriteLink(dir(), other);
  EXPECT_EQ(Run({"status", "--usb", "fake", "--key", key()}), 1);
  EXPECT_NE(err().find("didn't prove it's the one paired"), std::string::npos) << err();
  EXPECT_EQ(puck().last_set_time, 0U) << "nothing sent after a bad proof";
}

TEST_F(CliTest, AuthRefused) {
  puck().error_on = micnote_v1_Frame_auth_tag;
  EXPECT_EQ(Run({"status", "--usb", "fake", "--key", key()}), 1);
  EXPECT_NE(err().find("the puck refused this host"), std::string::npos) << err();
}

// ---- pairing ---------------------------------------------------------------

class PairTest : public CliTest {
 protected:
  PairTest() {
    // A second host, not yet paired, with its own key directory.
    fs::create_directories(dir() / "b");
    WriteKey(dir() / "b" / "host.key", "host_b");
    puck().pairing = true;
  }
  std::string b_key() { return (dir() / "b" / "host.key").string(); }
};

TEST_F(PairTest, PairsThenSyncs) {
  ASSERT_EQ(Run({"pair", "--usb", "fake", "--key", b_key(), "--name", "desk"}), 0) << err();
  EXPECT_NE(out().find("the puck is showing: "), std::string::npos) << out();
  EXPECT_NE(out().find("press the puck's button"), std::string::npos) << out();
  EXPECT_NE(out().find("paired with puck 0102030405060708 as host"), std::string::npos) << out();
  EXPECT_TRUE(fs::exists(LinkFile(dir() / "b")));
  EXPECT_EQ(fs::file_size(LinkFile(dir() / "b")), 32U);
  EXPECT_EQ(puck().hosts.at(HostId("host_b")).name, "desk");
  // The link key the CLI stored is the one the puck derived.
  std::ifstream f(LinkFile(dir() / "b"), std::ios::binary);
  std::array<char, 32> got{};
  f.read(got.data(), got.size());
  EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span(got)),
                                 puck().hosts.at(HostId("host_b")).link_key));

  // And it authenticates the next session.
  puck().pairing = false;
  ASSERT_EQ(Run({"hosts", "--usb", "fake", "--key", b_key()}), 0) << err();
  EXPECT_NE(out().find("desk  (this host)"), std::string::npos) << out();
  EXPECT_NE(out().find("host_a\n"), std::string::npos) << out();
}

TEST_F(PairTest, DefaultName) {
  ASSERT_EQ(Run({"pair", "--usb", "fake", "--key", b_key()}), 0) << err();
  EXPECT_EQ(puck().hosts.at(HostId("host_b")).name, "micnote-cli");
}

TEST_F(PairTest, NotInPairingMode) {
  puck().pairing = false;
  puck().strict = false;  // HelloAck for an unknown host, then PairBegin is refused
  EXPECT_EQ(Run({"pair", "--usb", "fake", "--key", b_key()}), 1);
  EXPECT_NE(err().find("isn't in pairing mode; hold its button for 5 s"), std::string::npos)
      << err();
}

TEST_F(PairTest, Failures) {
  puck().error_on = micnote_v1_Frame_pair_begin_tag;
  EXPECT_EQ(Run({"pair", "--usb", "fake", "--key", b_key()}), 1);
  EXPECT_NE(err().find("pairing failed: the puck said: busy"), std::string::npos) << err();

  puck().error_on = micnote_v1_Frame_hello_tag;
  EXPECT_EQ(Run({"pair", "--usb", "fake", "--key", b_key()}), 1);
  EXPECT_NE(err().find("hello failed"), std::string::npos) << err();
  puck().error_on.reset();

  // Nobody presses the button.
  puck().press_button = false;
  EXPECT_EQ(Run({"pair", "--usb", "fake", "--key", b_key()}), 1);
  EXPECT_NE(err().find("pairing not confirmed"), std::string::npos) << err();
  EXPECT_FALSE(fs::exists(LinkFile(dir() / "b")));
}

TEST_F(PairTest, InvalidDeviceKeyCancels) {
  puck().low_order_device_key = true;
  EXPECT_EQ(Run({"pair", "--usb", "fake", "--key", b_key()}), 1);
  EXPECT_NE(err().find("the puck sent an invalid key; pairing cancelled"), std::string::npos)
      << err();
  EXPECT_TRUE(puck().pair_cancelled);
  EXPECT_FALSE(puck().hosts.contains(HostId("host_b")));

  // A cancel the puck doesn't take is reported, and still a failure.
  puck().error_on = micnote_v1_Frame_pair_confirm_tag;
  EXPECT_EQ(Run({"pair", "--usb", "fake", "--key", b_key()}), 1);
  EXPECT_NE(err().find("didn't acknowledge the cancel"), std::string::npos) << err();
}

TEST_F(PairTest, CantWriteTheLinkKey) {
  // pucks/ is taken by a file.
  std::ofstream(dir() / "b" / "pucks") << "x";
  EXPECT_EQ(Run({"pair", "--usb", "fake", "--key", b_key()}), 1);
  EXPECT_NE(err().find("paired, but can't write"), std::string::npos) << err();
}

TEST_F(CliTest, HostsFails) {
  puck().error_on = micnote_v1_Frame_list_hosts_tag;
  EXPECT_EQ(Run({"hosts", "--usb", "fake", "--key", key()}), 1);
  EXPECT_NE(err().find("hosts failed"), std::string::npos) << err();
  EXPECT_EQ(Run({"hosts", "--usb", "nope", "--key", key()}), 1);
}

TEST_F(CliTest, Unpair) {
  const std::string other = "c0c0c0c0c0c0c0c0";
  puck().hosts[{std::byte{0xC0}, std::byte{0xC0}, std::byte{0xC0}, std::byte{0xC0}, std::byte{0xC0},
                std::byte{0xC0}, std::byte{0xC0}, std::byte{0xC0}}] = {};
  ASSERT_EQ(Run({"unpair", "--usb", "fake", "--host", other, "--key", key()}), 0) << err();
  EXPECT_EQ(out(), "unpaired host " + other + "\n");
  EXPECT_EQ(puck().hosts.size(), 1U);
  EXPECT_TRUE(fs::exists(LinkFile(dir())));

  EXPECT_EQ(Run({"unpair", "--usb", "fake", "--host", other, "--key", key()}), 1);
  EXPECT_NE(err().find("unpair failed: the puck said: not found"), std::string::npos) << err();

  // This host itself: its link key goes too.
  const std::string self = "f35e5616160a30bf";
  ASSERT_EQ(Run({"unpair", "--usb", "fake", "--host", self, "--key", key()}), 0) << err();
  EXPECT_NE(out().find("that was this host"), std::string::npos) << out();
  EXPECT_FALSE(fs::exists(LinkFile(dir())));
}

TEST_F(CliTest, UnpairNeedsHost) {
  EXPECT_EQ(Run({"unpair", "--usb", "fake", "--key", key()}), 2);
  EXPECT_NE(err().find("usage:"), std::string::npos);
}

TEST_F(CliTest, UnpairChecksTheId) {
  for (const char *bad : {"f35e", "f35e5616160a30bz", "f35e5616160a30bf00"}) {
    EXPECT_EQ(Run({"unpair", "--usb", "fake", "--host", bad, "--key", key()}), 1) << bad;
    EXPECT_NE(err().find("isn't a host_id"), std::string::npos) << err();
  }
  EXPECT_EQ(Run({"unpair", "--usb", "nope", "--host", "f35e5616160a30bf", "--key", key()}), 1);
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
  // Each synced recording also comes out as a playable Ogg Opus file.
  for (const char *opus : {"0000000100000001.opus", "0000000200000001.opus"}) {
    std::ifstream f(out_dir / opus, std::ios::binary);
    std::string magic(4, '\0');
    f.read(magic.data(), 4);
    EXPECT_EQ(magic, "OggS") << opus;
  }
  EXPECT_NE(out().find("0000000100000001: -> "), std::string::npos) << out();
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

// The .opus is a convenience next to the .mnr: when it can't be made, sync
// says so and still acks.
TEST_F(CliTest, SyncAcksEvenWithoutOpus) {
  puck().recordings.erase(0x0000000100000002ULL);
  const fs::path o = dir() / "o";
  fs::create_directories(o / "0000000100000001.opus");
  EXPECT_EQ(Run({"sync", "--usb", "fake", "--out", o.string(), "--key", key()}), 0) << err();
  EXPECT_NE(err().find("0000000100000001: can't write"), std::string::npos) << err();
  EXPECT_FALSE(puck().recordings.contains(0x0000000100000001ULL)) << "acked";
}

TEST_F(CliTest, WriteOpusFileNeedsOpus) {
  micnote::recording::Recording rec;
  rec.header.codec = 2;
  EXPECT_FALSE(micnote::cli::WriteOpusFile(rec, dir() / "x.opus", "x", env()));
  EXPECT_NE(out().find("x: no .opus"), std::string::npos) << out();
  EXPECT_FALSE(fs::exists(dir() / "x.opus"));
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

// ---- models ----------------------------------------------------------------

namespace {

constexpr std::string_view kAbcSha =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

class CliModels : public ::testing::Test {
 public:
  CliModels() {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("micnote-cli-models-" + std::to_string(rd()));
    fs::create_directories(dir_);
  }
  CliModels(const CliModels &) = delete;
  CliModels &operator=(const CliModels &) = delete;
  CliModels(CliModels &&) = delete;
  CliModels &operator=(CliModels &&) = delete;
  ~CliModels() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

 protected:
  // Two small models, "a" and "b", both "abc".
  static std::string TwoModels() {
    const std::string sha(kAbcSha);
    return R"({"version":1,"models":[
      {"id":"a","kind":"asr","url":"https://x/a.bin","sha256":")" +
           sha + R"(","bytes":3,"classes":["desktop"]},
      {"id":"b","kind":"vad","url":"https://x/b.bin","sha256":")" +
           sha + R"(","bytes":3,"classes":["desktop"]}],
      "defaults":[{"class":"desktop","asr":"a"}]})";
  }
  int Run(const std::vector<std::string> &args, std::string manifest = "", bool with_dir = true,
          bool with_device = true) {
    out_.str({});
    err_.str({});
    const Env env{
        .out = &out_,
        .err = &err_,
        .open = {},
        .now_unix_ms = [] { return std::uint64_t{0}; },
        .default_key = {},
        .models_dir = with_dir ? dir_ : fs::path(),
        .manifest_json = std::move(manifest),
        .device = with_device ? std::optional(micnote::models::Device{
                                    .cls = micnote::models::DeviceClass::kDesktop, .ram_mb = 16384})
                              : std::nullopt,
        .data_dir = {},
        .port_present = {},
        .wait = {},
        .engine = {},
        .keychain = nullptr};
    const std::vector<std::string_view> views(args.begin(), args.end());
    return micnote::cli::Run(views, env);
  }
  void Put(const std::string &file, const std::string &content) {
    std::ofstream(dir_ / file, std::ios::binary) << content;
  }
  [[nodiscard]] const fs::path &dir() const { return dir_; }
  [[nodiscard]] std::string out() const { return out_.str(); }
  [[nodiscard]] std::string err() const { return err_.str(); }

 private:
  fs::path dir_;
  std::ostringstream out_;
  std::ostringstream err_;
};

}  // namespace

TEST_F(CliModels, Usage) {
  for (const std::vector<std::string> &bad :
       std::vector<std::vector<std::string>>{{"models"},
                                             {"models", "bogus"},
                                             {"models", "list", "--id", "a"},
                                             {"models", "list", "--models"},
                                             {"models", "list", "--models", "x", "--models", "y"},
                                             {"models", "verify", "--bogus", "x"}}) {
    EXPECT_EQ(Run(bad), 2) << bad.size();
    EXPECT_NE(err().find("micnote-cli models list"), std::string::npos);
  }
  EXPECT_EQ(Run({"--help"}), 0);
  EXPECT_NE(out().find("micnote-cli models verify"), std::string::npos);
}

TEST_F(CliModels, NeedsADirectoryAndAGoodManifest) {
  EXPECT_EQ(Run({"models", "list"}, "", false), 1);
  EXPECT_NE(err().find("pass --models DIR"), std::string::npos);
  EXPECT_EQ(Run({"models", "list"}, "{"), 1);
  EXPECT_NE(err().find("bad model manifest"), std::string::npos);
}

TEST_F(CliModels, ListsTheBuiltInModels) {
  EXPECT_EQ(Run({"models", "list"}), 0);
  const std::string o = out();
  EXPECT_NE(o.find("(this machine: desktop, 16384 MB of RAM)"), std::string::npos) << o;
  EXPECT_NE(o.find("whisper-large-v3-turbo-q5_0  asr  574.0 MB  missing     default"),
            std::string::npos)
      << o;
  EXPECT_NE(o.find("get it: curl -L -o " + (dir() / "ggml-silero-v6.2.0.bin").string() +
                   " https://huggingface.co/ggml-org/whisper-vad/resolve/"),
            std::string::npos)
      << o;
  // Without a device given, this machine's.
  EXPECT_EQ(Run({"models", "list", "--models", dir().string()}, "", false, false), 0);
  EXPECT_NE(out().find("(this machine: desktop, "), std::string::npos);
}

TEST_F(CliModels, ListShowsWhatsThere) {
  Put("a.bin", "abc");
  EXPECT_EQ(Run({"models", "list"}, TwoModels()), 0);
  const std::string o = out();
  EXPECT_NE(o.find("present     default"), std::string::npos) << o;
  EXPECT_EQ(o.find("x/a.bin"), std::string::npos) << "no download hint for a present model";
  EXPECT_NE(o.find("x/b.bin"), std::string::npos) << o;
}

TEST_F(CliModels, VerifyChecksEveryDownloadedModel) {
  Put("a.bin", "abc");
  EXPECT_EQ(Run({"models", "verify"}, TwoModels()), 0) << err();
  EXPECT_NE(out().find("a: ok"), std::string::npos);
  EXPECT_NE(out().find("b: not downloaded"), std::string::npos);
  EXPECT_NE(err().find("a: verifying... 100%"), std::string::npos);

  Put("b.bin", "abd");
  EXPECT_EQ(Run({"models", "verify"}, TwoModels()), 1);
  EXPECT_NE(out().find("b: mismatch"), std::string::npos);

  // A bad one first doesn't stop the rest being checked.
  Put("a.bin", "abd");
  Put("b.bin", "abc");
  EXPECT_EQ(Run({"models", "verify"}, TwoModels()), 1);
  EXPECT_NE(out().find("a: mismatch\nb: ok"), std::string::npos) << out();
}

TEST_F(CliModels, VerifyOneModel) {
  Put("a.bin", "abc");
  EXPECT_EQ(Run({"models", "verify", "--id", "a"}, TwoModels()), 0);
  EXPECT_EQ(out(), "a: ok\n");
  EXPECT_EQ(Run({"models", "verify", "--id", "b"}, TwoModels()), 1);
  EXPECT_EQ(out(), "b: missing\n");
  EXPECT_EQ(Run({"models", "verify", "--id", "zz"}, TwoModels()), 1);
  EXPECT_NE(err().find("no model \"zz\""), std::string::npos);
}

TEST(CliDefaultModelsDir, FollowsTheEnvironment) {
#if defined(_WIN32)
  EXPECT_EQ(micnote::cli::DefaultModelsDir([](const char *n) {
              return std::string_view(n) == "LOCALAPPDATA" ? "C:\\Users\\a\\L" : nullptr;
            }),
            fs::path("C:\\Users\\a\\L") / "micnote" / "models");
#else
  EXPECT_EQ(micnote::cli::DefaultModelsDir([](const char *n) {
              return std::string_view(n) == "XDG_DATA_HOME" ? "/d" : "/home/a";
            }),
            fs::path("/d/micnote/models"));
  EXPECT_EQ(micnote::cli::DefaultModelsDir([](const char *n) {
              return std::string_view(n) == "XDG_DATA_HOME" ? "" : "/home/a";
            }),
            fs::path("/home/a/.local/share/micnote/models"));
#endif
  EXPECT_EQ(micnote::cli::DefaultModelsDir([](const char *) -> const char * { return nullptr; }),
            fs::path());
}

// ---- the store: db and run ---------------------------------------------------

namespace {

// Says the same words for any audio, a second at a time.
class WordsEngine final : public micnote::asr::AsrEngine {
 public:
  std::expected<std::vector<micnote::asr::AsrSegment>, micnote::asr::AsrError> Run(
      std::span<const float> pcm, const micnote::asr::AsrOptions & /*options*/) override {
    std::vector<micnote::asr::AsrSegment> out;
    if (!pcm.empty()) {
      out.push_back({.start_ms = 0,
                     .end_ms = 1000,
                     .text = "remember the budget",
                     .avg_logprob = -0.2,
                     .no_speech_prob = 0.01});
    }
    return out;
  }
  [[nodiscard]] std::string Model() const override { return "words"; }
  [[nodiscard]] std::string Backend() const override { return "test"; }
};

class CliStore : public CliTest {
 public:
  CliStore() {
    set_default_key(dir() / "host.key");
    set_data_dir(dir() / "data");
  }

  // Runs `run --usb fake` until `done(out)` or about 10 s; the port is
  // there whenever `present(out)` says so.
  int RunDaemon(const std::function<bool(const std::string &)> &present,
                const std::function<bool(const std::string &)> &done,
                std::vector<std::string> extra = {}) {
    std::vector<std::string> args = {"run", "--usb", "fake"};
    args.insert(args.end(), extra.begin(), extra.end());
    set_port_present([&, this](const std::string &port) {
      EXPECT_EQ(port, "fake");
      return present(Out());
    });
    int ticks = 0;
    set_wait([&, this](std::chrono::milliseconds) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return !done(Out()) && ++ticks < 2000;
    });
    // Not Run(): its env() would clear the output the lambdas read.
    const std::vector<std::string_view> views(args.begin(), args.end());
    env_ = env();
    const int code = micnote::cli::Run(views, *env_);
    return code;
  }
  std::string Out() const { return out(); }

  // A second connection to the store, with the key the CLI stored.
  micnote::store::Db Side() {
    micnote::secrets::FileSecrets secrets(dir());
    const auto key = secrets.Get("db_key").value();
    micnote::store::DbKey k{};
    std::ranges::copy(key, k.begin());
    return std::move(micnote::store::Db::Open(dir() / "data" / "micnote.db", k).value());
  }

  static int Count(const std::string &text, const std::string &what) {
    int n = 0;
    for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
      ++n;
    }
    return n;
  }

 private:
  std::optional<Env> env_;
};

const auto kAlways = [](const std::string &) { return true; };
const auto kSynced = [](const std::string &o) { return o.contains("sync finished"); };

}  // namespace

TEST_F(CliStore, AnEmptyStore) {
  EXPECT_EQ(Run({"db", "recordings"}), 0) << err();
  EXPECT_NE(out().find("no recordings in"), std::string::npos) << out();
  EXPECT_EQ(Run({"db", "transcripts"}), 0);
  EXPECT_EQ(out(), "no transcripts yet\n");
  EXPECT_EQ(Run({"db", "search", "budget"}), 0);
  EXPECT_EQ(out(), "no matches\n");
  // --data instead of the default.
  EXPECT_EQ(Run({"db", "recordings", "--data", (dir() / "other").string()}), 0);
  EXPECT_NE(out().find((dir() / "other").string()), std::string::npos);
}

TEST_F(CliStore, RunSyncsWhenThePuckAppearsThenDbShowsIt) {
  EXPECT_EQ(RunDaemon(kAlways, kSynced), 0) << err();
  const std::string o = Out();
  EXPECT_NE(o.find("watching fake for the puck"), std::string::npos) << o;
  EXPECT_NE(o.find("recordings are synced but not transcribed"), std::string::npos) << o;
  EXPECT_EQ(Count(o, "puck on fake: syncing"), 1) << "once per appearance, not per poll";
  EXPECT_EQ(Count(o, "stored recording "), 2) << o;
  EXPECT_TRUE(o.ends_with("stopped\n")) << o;
  // The fallback recording isn't this host's: it stays on the puck.
  EXPECT_EQ(puck().recordings.size(), 1U);

  EXPECT_EQ(Run({"db", "recordings"}), 0) << err();
  EXPECT_NE(out().find("RECORDING         STARTED (UTC)"), std::string::npos) << out();
  EXPECT_NE(out().find("0000000100000001"), std::string::npos) << out();
  EXPECT_NE(out().find("0:05"), std::string::npos) << out();
  EXPECT_EQ(Count(out(), "synced"), 2) << out();
  // Synced, not transcribed.
  EXPECT_EQ(Run({"db", "transcripts"}), 0);
  EXPECT_EQ(out(), "no transcripts yet\n");

  // A recording whose start time couldn't be worked out.
  {
    micnote::store::Db db = Side();
    micnote::store::StoredRecording r;
    r.device = 1;
    r.rec_id = 0x0000000900000001;
    r.duration_ms = 125000;
    r.mnr_path = "rec/x.mnr";
    r.created_at = 1;
    ASSERT_TRUE(micnote::store::InsertRecording(db, r));
  }
  EXPECT_EQ(Run({"db", "recordings"}), 0) << err();
  EXPECT_NE(out().find("0000000900000001  unknown           unknown       2:05  synced"),
            std::string::npos)
      << out();
}

TEST_F(CliStore, RunTranscribesWhatArrives) {
  set_engine([] {
    return std::expected<std::shared_ptr<micnote::asr::AsrEngine>, micnote::asr::AsrError>(
        std::make_shared<WordsEngine>());
  });
  EXPECT_EQ(RunDaemon(kAlways,
                      [](const std::string &o) { return Count(o, "transcribed recording ") == 2; }),
            0)
      << err();
  EXPECT_EQ(Out().find("not transcribed"), std::string::npos) << Out();

  EXPECT_EQ(Run({"db", "transcripts"}), 0) << err();
  EXPECT_EQ(Count(out(), ": words on test, RTF "), 2) << out();
  EXPECT_NE(out().find("  [00:00.000 --> 00:01.000] remember the budget"), std::string::npos)
      << out();
  // Still shown once its actions are planned (stage 3).
  {
    micnote::store::Db db = Side();
    ASSERT_TRUE(micnote::store::SetRecordingState(db, 1, micnote::store::RecordingState::kPlanned));
  }
  EXPECT_EQ(Run({"db", "transcripts"}), 0) << err();
  EXPECT_EQ(Count(out(), ": words on test, RTF "), 2) << out();
  EXPECT_EQ(Run({"db", "transcripts", "--recording", "1"}), 0) << err();
  EXPECT_EQ(Count(out(), "recording 1: words on test"), 1) << out();
  EXPECT_EQ(Run({"db", "search", "budget"}), 0) << err();
  EXPECT_EQ(Count(out(), "] remember the budget"), 2) << out();
  EXPECT_NE(out().find("recording 1 [00:00.000]"), std::string::npos) << out();
  EXPECT_EQ(Run({"db", "search", "\"open"}), 1) << "an FTS syntax error";
  EXPECT_NE(err().find("bad query"), std::string::npos) << err();
}

TEST_F(CliStore, RunSyncsAgainEachTimeThePuckComesBack) {
  // Present until the first sync ends, gone for one poll, then back.
  int phase = 0;
  EXPECT_EQ(RunDaemon(
                [&](const std::string &o) {
                  if (phase == 0 && Count(o, "sync finished") == 1) {
                    phase = 1;
                    return false;
                  }
                  if (phase == 1) {
                    phase = 2;
                  }
                  return true;
                },
                [](const std::string &o) { return Count(o, "sync finished") == 2; }),
            0)
      << err();
  EXPECT_EQ(Count(Out(), "puck on fake: syncing"), 2) << Out();
}

// The port disappears and comes back while a sync is still running: that's
// the same visit, not a new one.
TEST_F(CliStore, RunDoesntStackSyncs) {
  std::atomic<bool> release{false};
  set_open([&](const std::string &) -> std::expected<std::unique_ptr<Transport>, TransportError> {
    while (!release) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::make_unique<Borrowed>(puck());
  });
  int polls = 0;
  EXPECT_EQ(RunDaemon(
                [&](const std::string &) {
                  ++polls;
                  if (polls == 4) {
                    release = true;  // back, with the first sync still open
                  }
                  return polls != 2;
                },
                kSynced),
            0)
      << err();
  release = true;
  EXPECT_EQ(Count(Out(), "puck on fake: syncing"), 1) << Out();
}

TEST_F(CliStore, RunReportsFailedSyncs) {
  // The puck is busy: worth retrying.
  puck().error_on = micnote_v1_Frame_hello_tag;
  EXPECT_EQ(RunDaemon(kAlways, [](const std::string &o) { return o.contains("sync failed"); }), 0);
  EXPECT_NE(Out().find("(it will be retried)"), std::string::npos) << Out();
}

TEST_F(CliStore, RunReportsASyncThatWontBeRetried) {
  // No link key for this puck any more: pairing again is the only fix.
  fs::remove(dir() / "pucks" / "0102030405060708.link");
  EXPECT_EQ(RunDaemon(kAlways, [](const std::string &o) { return o.contains("sync failed"); }), 0);
  EXPECT_EQ(Out().find("will be retried"), std::string::npos) << Out();
}

TEST_F(CliStore, RunWithoutThePuck) {
  // No port_present: the port's file is checked, and nothing is there.
  set_wait([](std::chrono::milliseconds) { return false; });
  EXPECT_EQ(Run({"run", "--usb", (dir() / "no-such-port").string(), "--data",
                 (dir() / "elsewhere").string()}),
            0);
  EXPECT_TRUE(fs::exists(dir() / "elsewhere" / "micnote.db")) << "--data is where the store goes";
  EXPECT_EQ(out().find("syncing"), std::string::npos) << out();
  EXPECT_TRUE(out().ends_with("stopped\n"));
}

TEST_F(CliStore, RunCantQueueASync) {
  EXPECT_EQ(RunDaemon(
                [](const std::string &) {
                  micnote::store::Db::FailAfter(1);
                  return true;
                },
                kSynced),
            1);
  micnote::store::Db::FailAfter(0);
  EXPECT_NE(err().find("can't queue a sync"), std::string::npos) << err();
}

TEST_F(CliStore, Usage) {
  for (const std::vector<std::string> &bad :
       std::vector<std::vector<std::string>>{{"run"},
                                             {"run", "--usb"},
                                             {"run", "--bogus", "x"},
                                             {"run", "--usb", "a", "--usb", "b"},
                                             {"db"},
                                             {"db", "bogus"},
                                             {"db", "search"},
                                             {"db", "recordings", "--recording", "1"},
                                             {"db", "recordings", "--data"},
                                             {"db", "search", "q", "--recording", "1"},
                                             {"db", "transcripts", "--bogus", "x"},
                                             {"db", "recordings", "--data", "a", "--data", "b"}}) {
    EXPECT_EQ(Run(bad), 2) << bad.size();
  }
#if !defined(MICNOTE_HAVE_ASR)
  EXPECT_EQ(Run({"run", "--usb", "fake", "--models", "x"}), 2) << "no models without ASR";
#endif
  EXPECT_EQ(Run({"db", "transcripts", "--recording", "1x"}), 2);
  EXPECT_EQ(Run({"db", "transcripts", "--recording", "one"}), 2);
  EXPECT_NE(err().find("--recording takes a recording's ID"), std::string::npos);
  EXPECT_EQ(Run({"--help"}), 0);
  EXPECT_NE(out().find("micnote-cli db search QUERY"), std::string::npos);
  EXPECT_NE(out().find("micnote-cli run    --usb PORT"), std::string::npos);
}

TEST_F(CliStore, Failures) {
  EXPECT_EQ(Run({"db", "transcripts", "--recording", "99"}), 1);
  EXPECT_NE(err().find("no transcript for recording 99"), std::string::npos) << err();

  // Nowhere to keep the store.
  set_data_dir({});
  EXPECT_EQ(Run({"db", "recordings"}), 1);
  EXPECT_NE(err().find("pass --data DIR"), std::string::npos);
  set_data_dir(dir() / "data");
  set_default_key({});
  EXPECT_EQ(Run({"run", "--usb", "fake"}), 1);
  set_default_key(dir() / "host.key");
  // A data directory that's a file.
  std::ofstream(dir() / "afile") << "x";
  EXPECT_EQ(Run({"db", "recordings", "--data", (dir() / "afile").string()}), 1);
  EXPECT_NE(err().find("can't open the store in"), std::string::npos) << err();

  // The store failing at each of its calls: an error, never a crash.
  for (const char *what : {"recordings", "transcripts", "search"}) {
    for (int n = 1; n < 40; ++n) {
      micnote::store::Db::FailAfter(n);
      const int code =
          what == std::string_view("search") ? Run({"db", "search", "x"}) : Run({"db", what});
      const bool untouched = micnote::store::Db::FaultPending();
      micnote::store::Db::FailAfter(0);
      EXPECT_TRUE(code == 0 || code == 1) << what << " " << n;
      if (untouched) {
        EXPECT_EQ(code, 0) << what;
        break;
      }
    }
  }
}

TEST(CliDefaultDataDir, FollowsTheEnvironment) {
#if defined(_WIN32)
  EXPECT_EQ(micnote::cli::DefaultDataDir([](const char *n) {
              return std::string_view(n) == "LOCALAPPDATA" ? "C:\\Users\\a\\L" : nullptr;
            }),
            fs::path("C:\\Users\\a\\L") / "micnote");
#else
  EXPECT_EQ(micnote::cli::DefaultDataDir([](const char *n) {
              return std::string_view(n) == "XDG_DATA_HOME" ? "/d" : "/home/a";
            }),
            fs::path("/d/micnote"));
  EXPECT_EQ(micnote::cli::DefaultDataDir([](const char *n) {
              return std::string_view(n) == "XDG_DATA_HOME" ? "" : "/home/a";
            }),
            fs::path("/home/a/.local/share/micnote"));
#endif
  EXPECT_EQ(micnote::cli::DefaultDataDir([](const char *) -> const char * { return nullptr; }),
            fs::path());
}

TEST(CliDates, AreUtc) {
  EXPECT_EQ(micnote::cli::Date(0), "1970-01-01 00:00");
  EXPECT_EQ(micnote::cli::Date(1763800000000), "2025-11-22 08:26");
  EXPECT_EQ(micnote::cli::Clock(61001), "01:01.001");
}

// ---- the OS keychain ---------------------------------------------------------

namespace {

class CliKeychain : public CliTest {
 public:
  CliKeychain() {
    set_default_key(dir() / "host.key");
    set_keychain(&kc_);
  }

 protected:
  micnote::test::FakeKeychain &kc() { return kc_; }

 private:
  micnote::test::FakeKeychain kc_;
};

}  // namespace

TEST_F(CliKeychain, KeyFilesMoveInOnFirstUse) {
  ASSERT_EQ(Run({"status", "--usb", "fake"}), 0) << err();
  EXPECT_NE(err().find("moved " + (dir() / "host.key").string() + " into the keychain"),
            std::string::npos)
      << err();
  EXPECT_NE(err().find("0102030405060708.link into the keychain"), std::string::npos) << err();
  EXPECT_FALSE(fs::exists(dir() / "host.key"));
  EXPECT_FALSE(fs::exists(LinkFile(dir())));
  EXPECT_EQ(kc().items.count("host.key"), 1U);
  // From now on, straight from the keychain.
  ASSERT_EQ(Run({"status", "--usb", "fake"}), 0) << err();
  EXPECT_EQ(err(), "");
  EXPECT_EQ(Run({"keygen"}), 1);
  EXPECT_NE(err().find("host.key in the keychain already exists"), std::string::npos) << err();
  // --key still means files.
  EXPECT_EQ(Run({"keygen", "--key", (dir() / "k" / "host.key").string()}), 0) << err();
  EXPECT_TRUE(fs::exists(dir() / "k" / "host.key"));
}

TEST_F(CliKeychain, KeygenPairAndUnpairInTheKeychain) {
  fs::remove(dir() / "host.key");
  fs::remove(LinkFile(dir()));
  ASSERT_EQ(Run({"keygen"}), 0) << err();
  EXPECT_NE(out().find("wrote host key host.key in the keychain"), std::string::npos) << out();
  EXPECT_EQ(kc().items.at("host.key").size(), 64U);
  puck().strict = false;  // let the new host say hello, to find it has no link key
  EXPECT_EQ(Run({"status", "--usb", "fake"}), 1);
  EXPECT_NE(err().find("(no pucks/0102030405060708.link in the keychain)"), std::string::npos)
      << err();
  // Without a home directory, the keychain still works.
  set_default_key({});
  EXPECT_EQ(Run({"keygen"}), 1);
  EXPECT_NE(err().find("already exists"), std::string::npos);
}

TEST_F(CliKeychain, UnpairingThisHostForgetsItsLinkKey) {
  ASSERT_EQ(Run({"status", "--usb", "fake"}), 0) << err();  // moves the keys in
  ASSERT_EQ(kc().items.count("pucks/0102030405060708.link"), 1U);
  ASSERT_EQ(Run({"unpair", "--usb", "fake", "--host", "f35e5616160a30bf"}), 0) << err();
  EXPECT_EQ(kc().items.count("pucks/0102030405060708.link"), 0U);
  // One the keychain won't delete is reported.
  std::string link_hex;
  for (const std::byte b : kLink) {
    link_hex += std::format("{:02x}", std::to_integer<unsigned>(b));
  }
  kc().items["pucks/0102030405060708.link"] = link_hex;
  puck().hosts[HostId(kHostA)] = {.link_key = kLink, .name = "host_a"};
  kc().fail_remove = true;
  EXPECT_EQ(Run({"unpair", "--usb", "fake", "--host", "f35e5616160a30bf"}), 0) << err();
  EXPECT_NE(err().find("couldn't delete pucks/0102030405060708.link in the keychain"),
            std::string::npos)
      << err();
}

TEST_F(CliKeychain, TheStoresKeyGoesThereToo) {
  set_data_dir(dir() / "data");
  ASSERT_EQ(Run({"db", "recordings"}), 0) << err();
  EXPECT_EQ(kc().items.at("db_key").size(), 64U);
  EXPECT_FALSE(fs::exists(dir() / "db_key"));
}

TEST_F(CliKeychain, ADifferentKeyInTheKeychainWins) {
  kc().items["host.key"] = std::string(64, '1');
  Run({"status", "--usb", "fake"});
  EXPECT_NE(err().find("WARNING: " + (dir() / "host.key").string() +
                       " differs from host.key in the keychain"),
            std::string::npos)
      << err();
  EXPECT_TRUE(fs::exists(dir() / "host.key"));
  // The same bytes: the file just goes.
  std::ifstream f(dir() / "host.key", std::ios::binary);
  std::string bytes(32, '\0');
  f.read(bytes.data(), 32);
  f.close();
  std::string hex;
  for (const char c : bytes) {
    hex += std::format("{:02x}", static_cast<unsigned char>(c));
  }
  kc().items["host.key"] = hex;
  ASSERT_EQ(Run({"status", "--usb", "fake"}), 0) << err();
  EXPECT_NE(
      err().find("removed " + (dir() / "host.key").string() + ": the keychain already has it"),
      std::string::npos)
      << err();
  EXPECT_FALSE(fs::exists(dir() / "host.key"));
}

TEST_F(CliKeychain, IfAKeyWontMoveTheFilesAreUsed) {
  kc().fail_set = true;
  ASSERT_EQ(Run({"status", "--usb", "fake"}), 0) << err();
  EXPECT_NE(err().find("couldn't move"), std::string::npos) << err();
  EXPECT_NE(err().find("using the key files in " + dir().string() + " for now"), std::string::npos)
      << err();
  EXPECT_TRUE(fs::exists(dir() / "host.key"));
}

TEST_F(CliKeychain, KeysThatCantBeUsed) {
  // A keychain that won't answer.
  ASSERT_EQ(Run({"status", "--usb", "fake"}), 0);  // moves the keys in
  kc().fail_get = true;
  EXPECT_EQ(Run({"keygen"}), 1);
  EXPECT_NE(err().find("can't use host.key in the keychain for a host key"), std::string::npos)
      << err();
  kc().fail_get = false;
  // A --key name the store refuses.
  EXPECT_EQ(Run({"keygen", "--key", (dir() / "bad name").string()}), 1);
  EXPECT_NE(err().find("for a host key"), std::string::npos) << err();
  // A link key that isn't 32 bytes isn't one.
  kc().items["pucks/0102030405060708.link"] = "0011";
  EXPECT_EQ(Run({"status", "--usb", "fake"}), 1);
  EXPECT_NE(err().find("not paired with puck 0102030405060708"), std::string::npos) << err();
}

TEST_F(CliKeychain, NoKeychainMeansFilesAndAWarning) {
  kc().available = false;
  ASSERT_EQ(Run({"status", "--usb", "fake"}), 0) << err();
  EXPECT_NE(err().find("WARNING: no OS keychain is available"), std::string::npos) << err();
  EXPECT_TRUE(kc().items.empty());
  set_default_key({});
  EXPECT_EQ(Run({"keygen"}), 1);
  EXPECT_NE(err().find("pass --key FILE"), std::string::npos);
}
