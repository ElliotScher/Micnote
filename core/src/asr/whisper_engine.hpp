// The whisper.cpp engine (stage 2 §4.5), in the optional micnote_asr target
// (-DMICNOTE_ASR=ON). One loaded model per engine; Run() is not reentrant.
#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "asr/transcribe.hpp"

struct whisper_context;

namespace micnote::asr {

class WhisperEngine final : public AsrEngine {
 public:
  // Loads a ggml whisper model (ggml-*.bin). kModel if it isn't one.
  static std::expected<std::unique_ptr<WhisperEngine>, AsrError> Load(
      const std::filesystem::path &model);

  WhisperEngine(const WhisperEngine &) = delete;
  WhisperEngine &operator=(const WhisperEngine &) = delete;
  WhisperEngine(WhisperEngine &&) = delete;
  WhisperEngine &operator=(WhisperEngine &&) = delete;
  ~WhisperEngine() override;

  std::expected<std::vector<AsrSegment>, AsrError> Run(std::span<const float> pcm,
                                                       const AsrOptions &options) override;
  [[nodiscard]] std::string Model() const override { return model_; }
  [[nodiscard]] std::string Backend() const override;

 private:
  WhisperEngine(whisper_context *ctx, std::string model) : ctx_(ctx), model_(std::move(model)) {}
  whisper_context *ctx_;
  std::string model_;
};

}  // namespace micnote::asr
