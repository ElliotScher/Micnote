// The sync job (stage 2 §4.8, "sync.run@1", params {"usb": PORT}): one
// session with a puck, storing every finished recording it holds.
//
// 1. Hello, then the link-key challenge-response (protocol/session.hpp)
//    with the link key stored for that puck; SetTime; GetStatus.
// 2. Records the device (last seen, firmware) and a clock observation,
//    and repairs the start time of earlier recordings from the same boot.
// 3. For each finished recording not stored yet: Fetch, check it decrypts
//    with this host's key, write the .mnr under data_dir/rec/, store its
//    row (time repaired from the observations), Ack it, and call
//    on_recording (which queues its transcription). One already stored but
//    not acked (a crash in between) is just acked again.
#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "jobs/queue.hpp"
#include "secrets/secrets.hpp"
#include "transport/transport.hpp"

namespace micnote::jobs {

using Opener =
    std::function<std::expected<std::unique_ptr<transport::Transport>, transport::TransportError>(
        const std::string &port)>;

struct SyncDeps {
  std::filesystem::path data_dir;
  secrets::Secrets *secrets = nullptr;
  Opener open;
  std::chrono::milliseconds timeout = std::chrono::seconds(5);
  // A new recording was stored (its row id).
  std::function<void(std::int64_t recording)> on_recording;
};

JobResult RunSync(JobContext &ctx, const SyncDeps &deps);

}  // namespace micnote::jobs
