// micnote-cli: the dev harness and hardware bring-up tool. The commands live
// in commands.cpp; this only connects them to the real world.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "commands.hpp"
#include "local.hpp"
#include "models.hpp"
#if defined(MICNOTE_HAVE_KEYCHAIN)
#include "os_keychain.hpp"
#endif

namespace {

// Read once, before any threads. MSVC deprecates getenv (C4996) in favour of
// _dupenv_s, which allocates; the pointer is only read right away here, so
// getenv is fine on every platform.
// (warning(suppress) would only cover the next source line, which inside an
// #ifdef is the #endif, so push/disable/pop around the function instead.)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
const char *GetEnv(const char *name) {
  return std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// GCOVR_EXCL_START: only `run` and `transcribe --recording` wait, and the
// tests drive those with their own Env::wait; Ctrl-C can't be sent to a test.
std::atomic<bool> g_interrupted{false};

extern "C" void OnInterrupt(int /*signal*/) { g_interrupted = true; }

// Sleeps in short steps, so Ctrl-C stops a command within a tenth of a second.
bool WaitUnlessInterrupted(std::chrono::milliseconds total) {
  constexpr auto kStep = std::chrono::milliseconds(100);
  for (auto slept = std::chrono::milliseconds(0); slept < total && !g_interrupted; slept += kStep) {
    std::this_thread::sleep_for(kStep);
  }
  return !g_interrupted;
}
// GCOVR_EXCL_STOP

}  // namespace

int main(int argc, char **argv) try {
  const std::span all(argv, static_cast<std::size_t>(argc));
  const std::vector<std::string_view> args(all.begin() + 1, all.end());
  static_cast<void>(std::signal(SIGINT, OnInterrupt));
#if defined(MICNOTE_HAVE_KEYCHAIN)
  static micnote::cli::OsKeychain keychain;  // GCOVR_EXCL_BR_LINE: the static's guard
  micnote::secrets::KeychainBackend *const os_keychain = &keychain;
#else
  micnote::secrets::KeychainBackend *const os_keychain = nullptr;
#endif
  // GCOVR_EXCL_START: only gcc's exception cleanup of the temporaries below
  // is uncovered; every line here runs in every test.
  const micnote::cli::Env env{
      .out = &std::cout,
      .err = &std::cerr,
      .open = micnote::cli::UsbOpener(),
      .now_unix_ms = micnote::cli::SystemNowMs,
      .default_key = micnote::cli::DefaultKeyPath(GetEnv),
      .models_dir = micnote::cli::DefaultModelsDir(GetEnv),
      .manifest_json = {},
      .device = {},
      .data_dir = micnote::cli::DefaultDataDir(GetEnv),
      .port_present = {},
      .wait = WaitUnlessInterrupted,
      .engine = {},
      .llm = {},
      .keychain = os_keychain,
      .in = &std::cin,
      .http = {},
      .claude_base_url = {},
  };
  // GCOVR_EXCL_STOP
  const int code = micnote::cli::Run(args, env);
  std::cout.flush();
  return code;
}
// GCOVR_EXCL_START: needs a stream or the allocator to fail mid-command,
// which no test can arrange.
catch (...) {
  static_cast<void>(std::fputs("micnote-cli: failed\n", stderr));
  return 1;
}
// GCOVR_EXCL_STOP
