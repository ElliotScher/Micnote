// `micnote-cli transcribe`, only in builds with -DMICNOTE_ASR=ON.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "commands.hpp"
#include "jobs/transcribe_job.hpp"

namespace micnote::cli {

inline constexpr std::string_view kTranscribeUsage =
    "       micnote-cli transcribe --file FILE.mnr [--key FILE] [MODELS] [--language LANG] "
    "[--beam N]\n"
    "       micnote-cli transcribe --recording ID [--data DIR] [MODELS] [--language LANG] "
    "[--beam N]\n"
    "         MODELS: [--model MODEL.bin | --models DIR] [--vad SILERO.bin]\n"
    "                 [--backend auto|cpu|metal|vulkan|cuda]\n";

// args without argv[0], args[0] being "transcribe". Exit code as Run().
// --file transcribes a .mnr here and stores nothing; --recording runs a
// transcription job on a stored recording, as the app does, and prints the
// transcript it stored.
int Transcribe(std::span<const std::string_view> args, const Env &env);

// "" if `backend` (as --backend takes it) can run here, else why not.
std::string CheckBackend(std::string_view backend);

// For `run`: a speech engine for the registry's default model in
// models_dir, loaded on first use and kept; nullopt if it isn't downloaded.
std::optional<jobs::EngineFactory> RegistryEngine(const Env &env,
                                                  const std::filesystem::path &models_dir,
                                                  std::string_view backend = "auto");

}  // namespace micnote::cli
