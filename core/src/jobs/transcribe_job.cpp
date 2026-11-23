#include "jobs/transcribe_job.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "audio/opus_decode.hpp"
#include "crypto/psa.hpp"
#include "recording/reader.hpp"
#include "store/repos.hpp"
#include <nlohmann/json.hpp>

namespace micnote::jobs {

namespace {

using nlohmann::json;
using Kind = JobError::Kind;

JobError Fatal(std::string m) { return JobError{.kind = Kind::kFatal, .message = std::move(m)}; }

}  // namespace

JobResult RunTranscribe(JobContext &ctx, const TranscribeDeps &deps) {
  if (!ctx.job().ref_id) {
    return std::unexpected(Fatal("transcribe.run needs a recording"));
  }
  const std::int64_t id = *ctx.job().ref_id;
  std::optional<store::StoredRecording> rec;
  store::DbResult<void> db_ok;
  ctx.WithDb([&](store::Db &db) {
    auto r = store::GetRecording(db, id);
    if (!r) {
      db_ok = std::unexpected(r.error());
      return;
    }
    rec = std::move(*r);
    if (rec) {
      db_ok = store::SetRecordingState(db, id, store::RecordingState::kTranscribing);
    }
  });
  if (!db_ok) {
    return std::unexpected(
        JobError{.kind = Kind::kRetryable, .message = "store: " + db_ok.error().message});
  }
  if (!rec) {
    return std::unexpected(Fatal("no recording " + std::to_string(id)));
  }

  // From here on a failure marks the recording `error` (cancelling puts it
  // back to `synced`).
  const auto settle = [&](store::RecordingState state) {
    ctx.WithDb([&](store::Db &db) {
      [[maybe_unused]] const auto r = store::SetRecordingState(db, id, state);
    });
  };
  const auto fail = [&](std::string m) -> JobResult {
    settle(store::RecordingState::kError);
    return std::unexpected(Fatal(std::move(m)));
  };

  auto priv = deps.secrets->Get(secrets::kHostKey);
  if (!priv || priv->size() != crypto::kX25519KeySize) {
    return fail("no host key");
  }
  auto host = recording::HostKey::FromPrivate(std::span(*priv).first<crypto::kX25519KeySize>());
  crypto::Wipe(*priv);
  std::ifstream in(deps.data_dir / rec->mnr_path, std::ios::binary);
  if (!in) {
    return fail("can't read " + rec->mnr_path);
  }
  const std::vector<char> chars{std::istreambuf_iterator<char>(in), {}};
  std::vector<std::byte> file(chars.size());
  std::ranges::transform(chars, file.begin(), [](char c) { return static_cast<std::byte>(c); });
  const auto read = recording::ReadRecording(file, *host);
  if (!read) {
    return fail("can't decrypt " + rec->mnr_path);
  }
  const auto pcm = audio::DecodeOpus(read->packets);
  // GCOVR_EXCL_START: the packets are authenticated and came from the puck's
  // own encoder, and no test can write an .mnr (only the vector generator
  // does); DecodeOpus rejecting bad packets is covered in opus_decode_test.
  if (!pcm) {
    return fail("the audio doesn't decode");
  }
  // GCOVR_EXCL_STOP
  auto engine = deps.engine();
  if (!engine) {
    // The model may be downloaded yet; the recording isn't at fault.
    settle(store::RecordingState::kSynced);
    return std::unexpected(JobError{.kind = Kind::kRetryable, .message = "no speech model"});
  }

  const auto t0 = std::chrono::steady_clock::now();
  const auto segments = asr::Transcribe(**engine, *pcm, deps.options, [&](int pct) {
    ctx.Progress(pct);
    return !ctx.Cancelled();
  });
  if (!segments) {
    if (segments.error() == asr::AsrError::kCancelled) {
      settle(store::RecordingState::kSynced);
      return std::unexpected(JobError{.kind = Kind::kStopped, .message = "stopped"});
    }
    return fail("transcription failed");
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double audio_secs = static_cast<double>(pcm->size()) / audio::kAsrRate;

  store::Transcript t;
  t.recording = id;
  t.model = (*engine)->Model();
  t.backend = (*engine)->Backend();
  t.language = deps.options.language;
  t.created_at = ctx.Now();
  t.rtf = secs / std::max(audio_secs, 1e-3);
  // GCOVR_EXCL_BR_START: gcc's exception-cleanup branches for the copies.
  for (const auto &s : *segments) {
    t.segments.push_back(store::Segment{.start_ms = s.start_ms,
                                        .end_ms = s.end_ms,
                                        .text = s.text,
                                        .avg_logprob = s.avg_logprob,
                                        .no_speech_prob = s.no_speech_prob});
  }
  // GCOVR_EXCL_BR_STOP
  ctx.WithDb([&](store::Db &db) {
    auto saved = store::SaveTranscript(db, t);
    if (!saved) {
      db_ok = std::unexpected(saved.error());
      return;
    }
    db_ok = store::SetRecordingState(db, id, store::RecordingState::kTranscribed);
  });
  if (!db_ok) {
    settle(store::RecordingState::kSynced);
    return std::unexpected(
        JobError{.kind = Kind::kRetryable, .message = "store: " + db_ok.error().message});
  }
  ctx.Emit(
      json{{"type", "transcript.ready"}, {"recording", id}}.dump());  // GCOVR_EXCL_BR_LINE: json
  return {};
}

}  // namespace micnote::jobs
