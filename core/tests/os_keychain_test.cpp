// The real OS keychain, where there is one: a desktop session, the macOS
// and Windows runners. CI's Linux runners have no Secret Service, so there
// it skips (and KeychainSecrets is tested over a fake instead).
#include "os_keychain.hpp"

#include <gtest/gtest.h>

#include <random>
#include <string>

namespace {

using micnote::cli::OsKeychain;
using Status = micnote::secrets::KeychainBackend::Status;

TEST(OsKeychain, RoundTrip) {
  OsKeychain kc;
  if (!kc.Available()) {
    GTEST_SKIP() << "no OS keychain here";
  }
  std::random_device rd;
  const std::string name = "test-" + std::to_string(rd());
  std::string value;
  EXPECT_EQ(kc.Get(name, value), Status::kNotFound);
  ASSERT_EQ(kc.Set(name, "00ff10"), Status::kOk);
  ASSERT_EQ(kc.Get(name, value), Status::kOk);
  EXPECT_EQ(value, "00ff10");
  ASSERT_EQ(kc.Set(name, "abcd"), Status::kOk) << "replaces";
  ASSERT_EQ(kc.Get(name, value), Status::kOk);
  EXPECT_EQ(value, "abcd");
  EXPECT_EQ(kc.Remove(name), Status::kOk);
  EXPECT_EQ(kc.Get(name, value), Status::kNotFound);
}

}  // namespace
