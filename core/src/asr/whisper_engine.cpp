#include "asr/whisper_engine.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

#include <whisper.h>

namespace micnote::asr {

namespace {

// whisper.cpp logs to stderr by default; the CLI and app have their own.
void Quiet(enum ggml_log_level /*level*/, const char * /*text*/, void * /*user_data*/) {}

}  // namespace

std::expected<std::unique_ptr<WhisperEngine>, AsrError> WhisperEngine::Load(
    const std::filesystem::path &model) {
  whisper_log_set(Quiet, nullptr);
  whisper_context_params cparams = whisper_context_default_params();
  whisper_context *ctx = whisper_init_from_file_with_params(model.string().c_str(), cparams);
  if (ctx == nullptr) {
    return std::unexpected(AsrError::kModel);
  }
  return std::unique_ptr<WhisperEngine>(new WhisperEngine(ctx, model.stem().string()));
}

WhisperEngine::~WhisperEngine() { whisper_free(ctx_); }

std::string WhisperEngine::Backend() const {
  // The backends a build can have are its GGML_* options; this build's CPU
  // path is what runs unless one of them was compiled in.
  return "cpu";
}

std::expected<std::vector<AsrSegment>, AsrError> WhisperEngine::Run(std::span<const float> pcm,
                                                                    const AsrOptions &options) {
  const bool beam = options.beam_size > 1;
  whisper_full_params p =
      whisper_full_default_params(beam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
  if (beam) {
    p.beam_search.beam_size = options.beam_size;
  }
  p.n_threads = options.threads > 0
                    ? options.threads
                    : static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1U, 8U));
  p.language = options.language.c_str();
  p.detect_language = options.language == "auto";
  p.initial_prompt = options.initial_prompt.empty() ? nullptr : options.initial_prompt.c_str();
  p.token_timestamps = true;
  p.print_progress = false;
  p.print_realtime = false;
  p.print_timestamps = false;
  p.print_special = false;

  std::vector<AsrSegment> out;
  if (pcm.empty()) {
    return out;
  }
  if (whisper_full(ctx_, p, pcm.data(), static_cast<int>(pcm.size())) != 0) {
    return std::unexpected(AsrError::kFailed);
  }
  const int n = whisper_full_n_segments(ctx_);
  for (int i = 0; i < n; ++i) {
    double logp = 0;
    const int tokens = whisper_full_n_tokens(ctx_, i);
    for (int t = 0; t < tokens; ++t) {
      logp += static_cast<double>(std::log(std::max(whisper_full_get_token_p(ctx_, i, t), 1e-9F)));
    }
    std::string text = whisper_full_get_segment_text(ctx_, i);
    // whisper puts a leading space on each segment.
    text.erase(0, text.find_first_not_of(' '));
    // t0/t1 are in 10 ms units.
    out.push_back(AsrSegment{
        .start_ms = whisper_full_get_segment_t0(ctx_, i) * 10,
        .end_ms = whisper_full_get_segment_t1(ctx_, i) * 10,
        .text = std::move(text),
        .avg_logprob = tokens > 0 ? logp / tokens : 0,
        .no_speech_prob = static_cast<double>(whisper_full_get_segment_no_speech_prob(ctx_, i))});
  }
  return out;
}

}  // namespace micnote::asr
