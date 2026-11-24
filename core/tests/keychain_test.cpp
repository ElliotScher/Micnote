// The keychain store over a fake backend (hex encoding, errors, names), the
// file store's Remove, and moving the CLI's key files into the keychain.
#include "secrets/keychain.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "fake_keychain.hpp"

namespace {

namespace fs = std::filesystem;
using micnote::secrets::CallbackSecrets;
using micnote::secrets::FileSecrets;
using micnote::secrets::KeychainSecrets;
using micnote::secrets::MovedSecret;
using micnote::secrets::MoveFileSecrets;
using micnote::secrets::SecretError;
using micnote::test::FakeKeychain;
using Outcome = MovedSecret::Outcome;

std::vector<std::byte> Bytes(std::initializer_list<int> v) {
  std::vector<std::byte> out;
  for (const int b : v) {
    out.push_back(static_cast<std::byte>(b));
  }
  return out;
}

TEST(KeychainSecrets, StoresHexAndReadsItBack) {
  FakeKeychain kc;
  KeychainSecrets s(kc);
  ASSERT_TRUE(s.Set("host.key", Bytes({0x00, 0x1f, 0xa0, 0xff})));
  EXPECT_EQ(kc.items.at("host.key"), "001fa0ff");
  EXPECT_EQ(s.Get("host.key").value(), Bytes({0x00, 0x1f, 0xa0, 0xff}));
  EXPECT_EQ(s.Get("pucks/0102.link").error(), SecretError::kNotFound);
  ASSERT_TRUE(s.Set("empty", {}));
  EXPECT_TRUE(s.Get("empty").value().empty());
}

TEST(KeychainSecrets, RefusesWhatIsntAHexSecret) {
  FakeKeychain kc;
  KeychainSecrets s(kc);
  for (const char *bad : {"abc", "zz", "0g", "g0", "AB"}) {
    kc.items["k"] = bad;
    EXPECT_EQ(s.Get("k").error(), SecretError::kCorrupt) << bad;
  }
  kc.items["k"] = std::string((2 * 4096) + 2, '0');
  EXPECT_EQ(s.Get("k").error(), SecretError::kCorrupt) << "too long";
  EXPECT_EQ(s.Set("k", std::vector<std::byte>(4097)).error(), SecretError::kCorrupt);
}

TEST(KeychainSecrets, Errors) {
  FakeKeychain kc;
  KeychainSecrets s(kc);
  for (const char *bad : {"", "../x", "/abs"}) {
    EXPECT_EQ(s.Get(bad).error(), SecretError::kBadName);
    EXPECT_EQ(s.Set(bad, Bytes({1})).error(), SecretError::kBadName);
    EXPECT_EQ(s.Remove(bad).error(), SecretError::kBadName);
  }
  kc.fail_get = true;
  EXPECT_EQ(s.Get("k").error(), SecretError::kIo);
  kc.fail_set = true;
  EXPECT_EQ(s.Set("k", Bytes({1})).error(), SecretError::kIo);
  kc.fail_remove = true;
  EXPECT_EQ(s.Remove("k").error(), SecretError::kIo);
}

TEST(KeychainSecrets, Remove) {
  FakeKeychain kc;
  KeychainSecrets s(kc);
  ASSERT_TRUE(s.Set("k", Bytes({1})));
  EXPECT_TRUE(s.Remove("k"));
  EXPECT_TRUE(kc.items.empty());
  EXPECT_TRUE(s.Remove("k")) << "already gone is fine";
}

class Migration : public ::testing::Test {
 public:
  Migration() {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("micnote-migration-test-" + std::to_string(rd()));
    fs::create_directories(dir_ / "pucks");
  }
  Migration(const Migration &) = delete;
  Migration &operator=(const Migration &) = delete;
  Migration(Migration &&) = delete;
  Migration &operator=(Migration &&) = delete;
  ~Migration() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

 protected:
  void Put(const std::string &name, const std::string &bytes) {
    std::ofstream(dir_ / name, std::ios::binary) << bytes;
  }
  [[nodiscard]] const fs::path &dir() const { return dir_; }

 private:
  fs::path dir_;
};

TEST_F(Migration, MovesEveryKeyFileAndRemovesIt) {
  Put("host.key", "HHHH");
  Put("db_key", "DDDD");
  Put("pucks/0102030405060708.link", "LLLL");
  Put("pucks/notes.txt", "not a key");
  Put("other", "not a key either");
  fs::create_directories(dir() / "pucks" / "dir.link");
  FakeKeychain kc;
  KeychainSecrets s(kc);
  const auto moved = MoveFileSecrets(dir(), s);
  ASSERT_EQ(moved.size(), 3U);
  EXPECT_EQ(moved[0].name, "db_key");
  EXPECT_EQ(moved[1].name, "host.key");
  EXPECT_EQ(moved[2].name, "pucks/0102030405060708.link");
  for (const auto &m : moved) {
    EXPECT_EQ(m.outcome, Outcome::kMoved) << m.name;
    EXPECT_FALSE(fs::exists(dir() / m.name)) << m.name;
  }
  EXPECT_EQ(kc.items.at("host.key"), "48484848");
  EXPECT_EQ(kc.items.at("pucks/0102030405060708.link"), "4c4c4c4c");
  EXPECT_TRUE(fs::exists(dir() / "pucks" / "notes.txt"));
  EXPECT_TRUE(fs::exists(dir() / "other"));
  // Again: nothing left to move.
  EXPECT_TRUE(MoveFileSecrets(dir(), s).empty());
}

TEST_F(Migration, NothingThere) {
  FakeKeychain kc;
  KeychainSecrets s(kc);
  EXPECT_TRUE(MoveFileSecrets(dir() / "nowhere", s).empty());
}

TEST_F(Migration, SameBytesAlreadyThereJustRemovesTheFile) {
  Put("host.key", "HHHH");
  FakeKeychain kc;
  kc.items["host.key"] = "48484848";
  KeychainSecrets s(kc);
  const auto moved = MoveFileSecrets(dir(), s);
  ASSERT_EQ(moved.size(), 1U);
  EXPECT_EQ(moved[0].outcome, Outcome::kDuplicate);
  EXPECT_FALSE(fs::exists(dir() / "host.key"));
}

TEST_F(Migration, DifferentBytesKeepBoth) {
  Put("host.key", "HHHH");
  FakeKeychain kc;
  kc.items["host.key"] = "00000000";
  KeychainSecrets s(kc);
  const auto moved = MoveFileSecrets(dir(), s);
  ASSERT_EQ(moved.size(), 1U);
  EXPECT_EQ(moved[0].outcome, Outcome::kConflict);
  EXPECT_TRUE(fs::exists(dir() / "host.key"));
  EXPECT_EQ(kc.items.at("host.key"), "00000000");
}

TEST_F(Migration, AFileIsKeptUnlessTheKeychainProvablyHasIt) {
  Put("host.key", "HHHH");
  FakeKeychain kc;
  KeychainSecrets s(kc);
  // The keychain can't be read, or written, or doesn't keep what it's given.
  kc.fail_get = true;
  EXPECT_EQ(MoveFileSecrets(dir(), s).at(0).outcome, Outcome::kFailed);
  kc.fail_get = false;
  kc.fail_set = true;
  EXPECT_EQ(MoveFileSecrets(dir(), s).at(0).outcome, Outcome::kFailed);
  kc.fail_set = false;
  kc.garble_sets = true;
  EXPECT_EQ(MoveFileSecrets(dir(), s).at(0).outcome, Outcome::kFailed);
  kc.garble_sets = false;
  kc.items.clear();
  // It stores, then can't read back.
  class ForgetfulKeychain final : public micnote::secrets::KeychainBackend {
   public:
    bool Available() override { return true; }
    Status Get(const std::string & /*name*/, std::string & /*value*/) override {
      return Status::kNotFound;
    }
    Status Set(const std::string & /*name*/, const std::string & /*value*/) override {
      return Status::kOk;
    }
    Status Remove(const std::string & /*name*/) override { return Status::kOk; }
  } forgetful;
  KeychainSecrets f(forgetful);
  EXPECT_EQ(MoveFileSecrets(dir(), f).at(0).outcome, Outcome::kFailed);
  EXPECT_TRUE(fs::exists(dir() / "host.key"));
  // A key file too big to be a secret is left alone.
  Put("db_key", std::string(5000, 'x'));
  const auto moved = MoveFileSecrets(dir(), s);
  ASSERT_EQ(moved.size(), 2U);
  EXPECT_EQ(moved[0].outcome, Outcome::kFailed);
  EXPECT_TRUE(fs::exists(dir() / "db_key"));
  EXPECT_EQ(moved[1].outcome, Outcome::kMoved);
}

TEST(SecretsRemove, FilesAndCallbacks) {
  std::random_device rd;
  const fs::path dir = fs::temp_directory_path() / ("micnote-remove-test-" + std::to_string(rd()));
  FileSecrets files(dir);
  ASSERT_TRUE(files.Set("pucks/a.link", Bytes({1, 2})));
  EXPECT_TRUE(files.Remove("pucks/a.link"));
  EXPECT_EQ(files.Get("pucks/a.link").error(), SecretError::kNotFound);
  EXPECT_TRUE(files.Remove("pucks/a.link")) << "already gone is fine";
  EXPECT_EQ(files.Remove("../x").error(), SecretError::kBadName);
  // A directory where the file would be can't be removed as one.
  fs::create_directories(dir / "full" / "x");
  EXPECT_EQ(files.Remove("full").error(), SecretError::kIo);
  // The C ABI's callbacks can't delete.
  CallbackSecrets cb({.get = {}, .set = {}});
  EXPECT_EQ(cb.Remove("k").error(), SecretError::kProvider);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace
