// The transcribe job ("transcribe.run@1", ref_id = the recording): decrypt
// the stored .mnr with this host's key, decode, transcribe in slices
// (asr/transcribe.hpp), store the transcript, and mark the recording
// transcribed. The engine comes from a factory the host provides (whisper in
// builds with micnote_asr); a library without one never queues this job.
#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>

#include "asr/transcribe.hpp"
#include "jobs/queue.hpp"
#include "secrets/secrets.hpp"

namespace micnote::jobs {

using EngineFactory =
    std::function<std::expected<std::shared_ptr<asr::AsrEngine>, asr::AsrError>()>;

struct TranscribeDeps {
  std::filesystem::path data_dir;
  secrets::Secrets *secrets = nullptr;
  EngineFactory engine;
  asr::AsrOptions options;
};

JobResult RunTranscribe(JobContext &ctx, const TranscribeDeps &deps);

}  // namespace micnote::jobs
