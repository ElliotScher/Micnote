// `micnote-cli transcribe`, only in builds with -DMICNOTE_ASR=ON.
#pragma once

#include <span>
#include <string_view>

#include "commands.hpp"

namespace micnote::cli {

inline constexpr std::string_view kTranscribeUsage =
    "       micnote-cli transcribe --file FILE.mnr --model MODEL.bin [--key FILE] "
    "[--language LANG] [--beam N]\n";

// args without argv[0], args[0] being "transcribe". Exit code as Run().
int Transcribe(std::span<const std::string_view> args, const Env &env);

}  // namespace micnote::cli
