// Secrets: names, the file store (0600, atomic replace), the C ABI's
// callback store, and get-or-create.
#include "secrets/secrets.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using micnote::secrets::CallbackSecrets;
using micnote::secrets::FileSecrets;
using micnote::secrets::GetOrCreate;
using micnote::secrets::SecretCallbacks;
using micnote::secrets::SecretError;
using micnote::secrets::ValidName;

std::vector<std::byte> Bytes(std::size_t n, std::uint8_t v) {
  return std::vector<std::byte>(n, std::byte{v});
}

class SecretsTest : public ::testing::Test {
 public:
  SecretsTest() {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("micnote-secrets-test-" + std::to_string(rd()));
  }
  SecretsTest(const SecretsTest &) = delete;
  SecretsTest &operator=(const SecretsTest &) = delete;
  SecretsTest(SecretsTest &&) = delete;
  SecretsTest &operator=(SecretsTest &&) = delete;
  ~SecretsTest() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

 protected:
  [[nodiscard]] const fs::path &dir() const { return dir_; }

 private:
  fs::path dir_;
};

TEST(SecretNames, OnlyPlainRelativeNames) {
  EXPECT_TRUE(ValidName("db_key"));
  EXPECT_TRUE(ValidName("link_key/0102030405060708"));
  EXPECT_TRUE(ValidName("a-b.c"));
  EXPECT_TRUE(ValidName("Zz09_./-Aa"));
  EXPECT_FALSE(ValidName("~"));
  EXPECT_FALSE(ValidName("!"));
  EXPECT_FALSE(ValidName("["));
  EXPECT_FALSE(ValidName(":"));
  EXPECT_FALSE(ValidName(""));
  EXPECT_FALSE(ValidName("/etc/passwd"));
  EXPECT_FALSE(ValidName("../escape"));
  EXPECT_FALSE(ValidName("a/../b"));
  EXPECT_FALSE(ValidName("spa ce"));
  EXPECT_FALSE(ValidName(std::string(129, 'a')));
}

TEST_F(SecretsTest, FilesRoundTripOwnerOnly) {
  FileSecrets s(dir());
  EXPECT_EQ(s.Get("db_key").error(), SecretError::kNotFound);
  ASSERT_TRUE(s.Set("link_key/0102030405060708", Bytes(32, 7)));
  EXPECT_EQ(s.Get("link_key/0102030405060708").value(), Bytes(32, 7));
  ASSERT_TRUE(s.Set("link_key/0102030405060708", Bytes(32, 8)));
  EXPECT_EQ(s.Get("link_key/0102030405060708").value(), Bytes(32, 8));
#ifndef _WIN32
  const auto perms = fs::status(dir() / "link_key" / "0102030405060708").permissions();
  EXPECT_EQ(perms & (fs::perms::group_all | fs::perms::others_all), fs::perms::none);
#endif
  EXPECT_FALSE(fs::exists(dir() / "link_key" / "0102030405060708.tmp"));
}

TEST_F(SecretsTest, FileFailures) {
  FileSecrets s(dir());
  EXPECT_EQ(s.Get("../x").error(), SecretError::kBadName);
  EXPECT_EQ(s.Set("../x", Bytes(1, 1)).error(), SecretError::kBadName);
  EXPECT_EQ(s.Set("big", Bytes(5000, 1)).error(), SecretError::kCorrupt);

  // Too big on disk.
  fs::create_directories(dir());
  std::ofstream(dir() / "huge", std::ios::binary) << std::string(5000, 'x');
  EXPECT_EQ(s.Get("huge").error(), SecretError::kCorrupt);

  // A directory where the file would go: the rename fails.
  fs::create_directories(dir() / "taken" / "inner");
  EXPECT_EQ(s.Set("taken", Bytes(4, 1)).error(), SecretError::kIo);
  EXPECT_FALSE(fs::exists(dir() / "taken.tmp"));
  // The temporary can't be created: its directory is a file.
  std::ofstream(dir() / "file") << "x";
  EXPECT_EQ(s.Set("file/sub", Bytes(4, 1)).error(), SecretError::kIo);
}

TEST_F(SecretsTest, GetOrCreateMakesItOnceAndChecksItsSize) {
  FileSecrets s(dir());
  const auto a = GetOrCreate(s, "db_key", 32).value();
  EXPECT_EQ(a.size(), 32U);
  EXPECT_EQ(GetOrCreate(s, "db_key", 32).value(), a);
  EXPECT_EQ(GetOrCreate(s, "db_key", 16).error(), SecretError::kCorrupt);
  EXPECT_EQ(GetOrCreate(s, "../bad", 32).error(), SecretError::kBadName);
  // Can't store it: reported, nothing returned.
  std::ofstream(dir() / "blocker") << "x";
  EXPECT_EQ(GetOrCreate(s, "blocker/key", 32).error(), SecretError::kIo);
}

TEST(CallbackSecrets, UsesTheHostsStore) {
  std::map<std::string, std::vector<std::byte>> store;
  int fail = 0;
  CallbackSecrets s(SecretCallbacks{
      .get = [&](const char *name, std::uint8_t *out, std::size_t cap, std::size_t *len) -> int {
        if (fail != 0) {
          *len = cap + 1;
          return fail == 2 ? 0 : 5;
        }
        const auto it = store.find(name);
        if (it == store.end()) {
          return 1;
        }
        std::memcpy(out, it->second.data(), it->second.size());
        *len = it->second.size();
        return 0;
      },
      .set = [&](const char *name, const std::uint8_t *data, std::size_t len) -> int {
        if (fail != 0) {
          return 3;
        }
        const auto *b = reinterpret_cast<const std::byte *>(data);
        store[name] = std::vector<std::byte>(b, b + len);
        return 0;
      }});
  EXPECT_EQ(s.Get("db_key").error(), SecretError::kNotFound);
  const auto key = GetOrCreate(s, "db_key", 32).value();
  EXPECT_EQ(store.at("db_key"), key);
  EXPECT_EQ(s.Get("db_key").value(), key);
  EXPECT_EQ(s.Get("..").error(), SecretError::kBadName);
  EXPECT_EQ(s.Set("..", key).error(), SecretError::kBadName);

  fail = 1;  // the host says no
  EXPECT_EQ(s.Get("db_key").error(), SecretError::kProvider);
  EXPECT_EQ(s.Set("db_key", key).error(), SecretError::kProvider);
  EXPECT_EQ(GetOrCreate(s, "other", 32).error(), SecretError::kProvider);
  fail = 2;  // "success", with a length past the buffer
  EXPECT_EQ(s.Get("db_key").error(), SecretError::kProvider);
}

}  // namespace
