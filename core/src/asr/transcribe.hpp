// Speech to text, engine-independent (stage 2 §4.5 steps 4-6): long audio
// goes to the engine in slices of about 10 minutes with 2 s of overlap, which
// bounds memory and allows progress and cancellation; segments in the
// overlaps are de-duplicated and every timestamp is in recording time.
//
// The engine is whisper.cpp (whisper_engine.hpp, in the optional micnote_asr
// target); tests use a fake.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace micnote::asr {

struct AsrSegment {
  // Relative to the start of the audio the engine was given.
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::string text;
  double avg_logprob = 0;
  double no_speech_prob = 0;
};

struct AsrOptions {
  std::string language = "en";  // or "auto"
  // Names, repos, jargon: whisper's initial prompt (§4.5 step 5).
  std::string initial_prompt;
  // 1: greedy (mobile); >1: beam search of that width (desktop: 5).
  int beam_size = 5;
  int threads = 0;  // 0: the engine picks
};

enum class AsrError : std::uint8_t {
  kModel,      // the model can't be loaded or isn't a whisper model
  kBackend,    // that compute backend isn't in this build, or not on this machine
  kFailed,     // the engine failed on this audio
  kCancelled,  // the progress callback said stop
};

class AsrEngine {
 public:
  AsrEngine() = default;
  AsrEngine(const AsrEngine &) = delete;
  AsrEngine &operator=(const AsrEngine &) = delete;
  AsrEngine(AsrEngine &&) = delete;
  AsrEngine &operator=(AsrEngine &&) = delete;
  // GCOVR_EXCL_START: the deleting destructor only runs for an object whose
  // dynamic type is AsrEngine, which is abstract.
  virtual ~AsrEngine() = default;
  // GCOVR_EXCL_STOP

  // 16 kHz mono float PCM, at most a slice long.
  virtual std::expected<std::vector<AsrSegment>, AsrError> Run(std::span<const float> pcm,
                                                               const AsrOptions &options) = 0;
  [[nodiscard]] virtual std::string Model() const = 0;
  [[nodiscard]] virtual std::string Backend() const = 0;
};

struct SliceConfig {
  std::int64_t slice_ms = std::int64_t{10} * 60 * 1000;
  std::int64_t overlap_ms = 2000;
};

// Called after each slice with the percentage done; returning false cancels.
using Progress = std::function<bool(int pct)>;

std::expected<std::vector<AsrSegment>, AsrError> Transcribe(AsrEngine &engine,
                                                            std::span<const float> pcm16k,
                                                            const AsrOptions &options,
                                                            const Progress &progress = {},
                                                            const SliceConfig &slices = {});

}  // namespace micnote::asr
