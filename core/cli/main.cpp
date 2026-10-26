// micnote-cli: the dev harness and hardware bring-up tool. Stage 0b only
// reports its version; stage 1 adds pair and sync, stages 2-3 transcribe and
// plan.
#include <micnote.h>

#include <cstdio>
#include <print>
#include <span>
#include <string_view>

namespace {

constexpr std::string_view kUsage = "usage: micnote-cli [--version | --help]\n";

}  // namespace

int main(int argc, char **argv) try {
  const std::span args(argv, static_cast<std::size_t>(argc));
  if (args.size() == 2) {
    const std::string_view arg = args.back();
    if (arg == "--version" || arg == "-V") {
      std::println("micnote-cli {}", mn_version());
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::print("{}", kUsage);
      return 0;
    }
  }
  std::print(stderr, "{}", kUsage);
  return 2;
}
// GCOVR_EXCL_START: needs stdout to fail mid-write, which no test can arrange.
catch (...) {
  // std::print throws on a write error (e.g. a closed pipe). If stderr is
  // gone too, there is nothing left to report to.
  static_cast<void>(std::fputs("micnote-cli: failed to write output\n", stderr));
  return 1;
}
// GCOVR_EXCL_STOP
