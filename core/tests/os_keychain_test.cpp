// The real OS keychain. Opt-in, because it writes (then deletes) an item in
// the keychain of whoever runs it: set MICNOTE_TEST_OS_KEYCHAIN=1. CI sets it
// on the macOS and Windows runners. KeychainSecrets is tested over a fake.
#include "os_keychain.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <random>
#include <string>

#ifdef _MSC_VER
#pragma warning(disable : 4996)  // getenv, as in cli/main.cpp
#endif

namespace {

using micnote::cli::OsKeychain;
using Status = micnote::secrets::KeychainBackend::Status;

TEST(OsKeychain, RoundTrip) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): before any threads
  const char *opt_in = std::getenv("MICNOTE_TEST_OS_KEYCHAIN");
  if (opt_in == nullptr || *opt_in == '\0') {
    GTEST_SKIP() << "set MICNOTE_TEST_OS_KEYCHAIN=1 to use this machine's keychain";
  }
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
