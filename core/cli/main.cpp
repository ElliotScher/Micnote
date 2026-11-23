// micnote-cli: the dev harness and hardware bring-up tool. The commands live
// in commands.cpp; this only connects them to the real world.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "commands.hpp"

namespace {

// NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any threads.
const char *GetEnv(const char *name) { return std::getenv(name); }

}  // namespace

int main(int argc, char **argv) try {
  const std::span all(argv, static_cast<std::size_t>(argc));
  const std::vector<std::string_view> args(all.begin() + 1, all.end());
  // GCOVR_EXCL_START: only gcc's exception cleanup of the temporaries below
  // is uncovered; every line here runs in every test.
  const micnote::cli::Env env{
      .out = &std::cout,
      .err = &std::cerr,
      .open = micnote::cli::UsbOpener(),
      .now_unix_ms = micnote::cli::SystemNowMs,
      .default_key = micnote::cli::DefaultKeyPath(GetEnv),
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
