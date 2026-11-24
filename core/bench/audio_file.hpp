// Reads a WAV, FLAC or MP3 file as whisper wants it: 16 kHz mono floats.
#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace micnote::bench {

std::optional<std::vector<float>> ReadAudio(const std::filesystem::path &path);

}  // namespace micnote::bench
