// Runs the C ABI test (abi_test.c) and the ABI's C++ side.
#include <micnote.h>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <random>
#include <string>

extern "C" int mn_abi_c_test(const char *data_dir, char *msg, size_t cap);

namespace {

namespace fs = std::filesystem;

TEST(Abi, FromC) {
  std::random_device rd;
  const fs::path dir = fs::temp_directory_path() / ("micnote-abi-test-" + std::to_string(rd()));
  std::array<char, 256> msg{};
  EXPECT_EQ(mn_abi_c_test(dir.string().c_str(), msg.data(), msg.size()), 0) << msg.data();
  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace
