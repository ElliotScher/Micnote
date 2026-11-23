// The whisper.cpp engine (stage 2 §4.5), in the optional micnote_asr target
// (-DMICNOTE_ASR=ON). One loaded model per engine; Run() is not reentrant.
//
// With a VAD model (whisper's Silero port, ggml-silero-*.bin), each slice goes
// through voice activity detection first (§4.5 step 3): whisper transcribes
// only the speech, so long silences cost nothing and can't turn into
// hallucinated text, and it maps segment times back to the slice's own
// timeline, so timestamps still line up with markers. Audio with no speech
// gives no segments.
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
  // Loads a ggml whisper model (ggml-*.bin) and, unless vad_model is empty,
  // turns on VAD with that model. kModel if either isn't one.
  static std::expected<std::unique_ptr<WhisperEngine>, AsrError> Load(
      const std::filesystem::path &model, const std::filesystem::path &vad_model = {});

  WhisperEngine(const WhisperEngine &) = delete;
  WhisperEngine &operator=(const WhisperEngine &) = delete;
  WhisperEngine(WhisperEngine &&) = delete;
  WhisperEngine &operator=(WhisperEngine &&) = delete;
  ~WhisperEngine() override;

  std::expected<std::vector<AsrSegment>, AsrError> Run(std::span<const float> pcm,
                                                       const AsrOptions &options) override;
  [[nodiscard]] std::string Model() const override { return model_; }
  [[nodiscard]] std::string Backend() const override;
  [[nodiscard]] bool Vad() const { return !vad_model_.empty(); }

 private:
  WhisperEngine(whisper_context *ctx, std::string model, std::string vad_model)
      : ctx_(ctx), model_(std::move(model)), vad_model_(std::move(vad_model)) {}
  whisper_context *ctx_;
  std::string model_;
  std::string vad_model_;  // whisper loads it on the first Run()
};

}  // namespace micnote::asr
