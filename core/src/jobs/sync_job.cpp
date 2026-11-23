#include "jobs/sync_job.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "crypto/pairing.hpp"
#include "crypto/psa.hpp"
#include "protocol/client.hpp"
#include "protocol/session.hpp"
#include "recording/reader.hpp"
#include "store/repos.hpp"
#include "time/clock_fix.hpp"
#include <nlohmann/json.hpp>

namespace micnote::jobs {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using Kind = JobError::Kind;

JobError Fatal(std::string m) { return JobError{.kind = Kind::kFatal, .message = std::move(m)}; }
JobError Retry(std::string m) {
  return JobError{.kind = Kind::kRetryable, .message = std::move(m)};
}

// A client failure: the device refusing this host won't fix itself; a link
// that dropped or timed out may.
JobError FromClient(const std::string &what, const protocol::ClientError &e) {
  using CK = protocol::ClientError::Kind;
  const bool unpaired =
      e.kind == CK::kDevice && e.device == micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED;
  // GCOVR_EXCL_BR_START: gcc's exception-cleanup branches for the strings.
  std::string m = what + " failed";
  if (e.kind == CK::kDevice) {
    m += " (device error " + std::to_string(static_cast<int>(e.device)) + ")";
  }
  if (unpaired) {
    return Fatal(m + ": this host isn't paired with the puck");
  }
  return Retry(m);
  // GCOVR_EXCL_BR_STOP
}

std::string Hex16(std::uint64_t v) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string s(16, '0');
  for (std::size_t i = 0; i < 16; ++i) {
    s.at(15 - i) = kDigits.at((v >> (4U * i)) & 0xFU);
  }
  return s;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup of `s`

bool WriteAtomically(const fs::path &path, std::span<const std::byte> bytes) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) {
      return false;
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

// The secret `name` as exactly N bytes.
template <std::size_t N>
std::optional<std::array<std::byte, N>> Secret(secrets::Secrets &s, std::string_view name) {
  auto v = s.Get(name);
  if (!v || v->size() != N) {
    return std::nullopt;
  }
  std::array<std::byte, N> out{};
  std::ranges::copy(*v, out.begin());
  crypto::Wipe(*v);
  return out;
}

}  // namespace

JobResult RunSync(JobContext &ctx, const SyncDeps &deps) {
  const json params = json::parse(ctx.job().params_json, nullptr, false);
  if (!params.is_object() || !params.contains("usb") || !params["usb"].is_string()) {
    return std::unexpected(Fatal(R"(sync.run needs {"usb": PORT})"));
  }
  auto priv = Secret<crypto::kX25519KeySize>(*deps.secrets, secrets::kHostKey);
  if (!priv) {
    return std::unexpected(Fatal("no host key: pair this host with the puck first"));
  }
  auto host = recording::HostKey::FromPrivate(*priv);
  crypto::Wipe(*priv);

  const std::string port = params["usb"].get<std::string>();
  auto link = deps.open(port);
  if (!link) {
    return std::unexpected(Retry("can't open " + port));
  }
  protocol::Client client(**link, deps.timeout);
  crypto::Nonce n_h{};
  // GCOVR_EXCL_START: PSA's RNG fails only without an OS entropy source.
  if (crypto::Init() != PSA_SUCCESS ||
      psa_generate_random(reinterpret_cast<std::uint8_t *>(n_h.data()), n_h.size()) !=
          PSA_SUCCESS) {
    return std::unexpected(Fatal("no random number source"));
  }
  // GCOVR_EXCL_STOP
  const auto hello = client.Hello(host->host_id(), n_h);
  if (!hello) {
    return std::unexpected(FromClient("Hello", hello.error()));
  }
  const auto device_id = protocol::DeviceId(*hello);
  const std::string device_hex = protocol::DeviceIdHex(device_id);
  const auto link_key =
      Secret<crypto::kLinkKeySize>(*deps.secrets, secrets::LinkKeyName(device_hex));
  if (!link_key) {
    return std::unexpected(Fatal("not paired with puck " + device_hex));
  }
  if (auto auth = protocol::Authenticate(client, n_h, *hello, *link_key); !auth) {
    if (auth.error().kind == protocol::AuthError::Kind::kWrongPuck) {
      return std::unexpected(Fatal("puck " + device_hex + " didn't prove it's the one paired"));
    }
    return std::unexpected(FromClient("Auth", auth.error().client));
  }
  const std::int64_t now = ctx.Now();
  if (auto set = client.SetTime(static_cast<std::uint64_t>(now), 0); !set) {
    return std::unexpected(FromClient("SetTime", set.error()));
  }
  const auto status = client.GetStatus();
  if (!status) {
    return std::unexpected(FromClient("GetStatus", status.error()));
  }

  // The device and what its clock says now.
  std::optional<std::int64_t> device;
  store::DbResult<void> stored;
  ctx.WithDb([&](store::Db &db) {
    auto known = store::FindDevice(db, device_id);
    if (!known) {
      stored = std::unexpected(known.error());
      return;
    }
    store::Device d;
    if (*known) {
      d = **known;
    } else {
      d.paired_at = now;
    }
    d.device_id = device_id;
    d.last_seen = now;
    d.fw_version = std::to_string(status->fw_version.major) + "." +
                   std::to_string(status->fw_version.minor) + "." +
                   std::to_string(status->fw_version.patch);
    auto id = store::UpsertDevice(db, d);
    if (!id) {
      stored = std::unexpected(id.error());
      return;
    }
    device = *id;
    const store::ClockObservation obs{.boot_count = status->boot_count,
                                      .uptime_ms = static_cast<std::int64_t>(status->uptime_ms),
                                      .unix_ms = now,
                                      .observed_at = now};
    stored = store::AddClockObservation(db, *id, obs);
    if (!stored) {
      return;
    }
    // Recordings from this boot that had no time can have one now.
    auto undated = store::UndatedRecordings(db, *id, status->boot_count);
    if (!undated) {
      stored = std::unexpected(undated.error());
      return;
    }
    for (const auto &r : *undated) {
      const auto fix = time::RepairStart(0, r.start_boot, r.start_uptime_ms, std::span(&obs, 1));
      stored = store::SetRecordingTime(db, r.id, fix.fixed_unix_ms, fix.confidence);
      if (!stored) {
        return;
      }
    }
  });
  if (!stored) {
    return std::unexpected(Retry("store: " + stored.error().message));
  }

  const auto list = client.ListRecordings();
  if (!list) {
    return std::unexpected(FromClient("ListRecordings", list.error()));
  }
  std::size_t done = 0;
  for (const auto &info : *list) {
    if (ctx.Cancelled()) {
      return std::unexpected(JobError{.kind = Kind::kStopped, .message = "stopped"});
    }
    ctx.Progress(static_cast<int>(done * 100 / list->size()));
    ++done;
    if (!info.complete) {
      continue;  // still recording: next time
    }
    const auto rec_id = static_cast<std::int64_t>(info.id);

    // Stored but not acked (a crash between the two): ack it again.
    store::DbResult<std::optional<std::vector<std::byte>>> existing;
    ctx.WithDb([&](store::Db &db) { existing = store::FindRecordingSha(db, *device, rec_id); });
    if (!existing) {
      return std::unexpected(Retry("store: " + existing.error().message));
    }
    if (*existing) {
      crypto::Sha256Digest sha{};
      std::ranges::copy(std::span(**existing).first(std::min(sha.size(), (*existing)->size())),
                        sha.begin());
      if (auto acked = client.Ack(info.id, sha); !acked) {
        return std::unexpected(FromClient("Ack", acked.error()));
      }
      continue;
    }

    protocol::FetchedRecording fetched;
    fetched.id = info.id;
    if (auto ok = client.Fetch(fetched); !ok) {
      return std::unexpected(FromClient("Fetch", ok.error()));
    }
    const std::vector<std::byte> file = fetched.File();
    const auto rec = recording::ReadRecording(file, *host);
    if (!rec) {
      // Not for this host (made before it paired, on another host's key):
      // left on the puck for the host that can read it.
      continue;
    }
    const std::string rel = "rec/" + device_hex + "/" + Hex16(info.id) + ".mnr";
    if (!WriteAtomically(deps.data_dir / rel, file)) {
      return std::unexpected(Retry("can't write " + rel));
    }
    const crypto::Sha256Digest sha = crypto::Sha256(file);
    std::int64_t row = 0;
    ctx.WithDb([&](store::Db &db) {
      auto obs = store::ClockObservations(db, *device, rec->header.start_boot);
      if (!obs) {
        stored = std::unexpected(obs.error());
        return;
      }
      const auto fix = time::RepairStart(  // GCOVR_EXCL_LINE: gcc's exception cleanup
          static_cast<std::int64_t>(rec->header.start_unix_ms), rec->header.start_boot,
          static_cast<std::int64_t>(rec->header.start_uptime_ms), *obs);
      // GCOVR_EXCL_BR_START: gcc's exception-cleanup branches for the copies.
      store::StoredRecording r;
      r.device = *device;
      r.rec_id = rec_id;
      r.start_boot = rec->header.start_boot;
      r.start_uptime_ms = static_cast<std::int64_t>(rec->header.start_uptime_ms);
      r.start_unix_ms = static_cast<std::int64_t>(rec->header.start_unix_ms);
      r.start_unix_ms_fixed = fix.fixed_unix_ms;
      r.time_confidence = fix.confidence;
      r.duration_ms = rec->DurationMs();
      r.mnr_path = rel;
      r.sha256.assign(sha.begin(), sha.end());
      r.created_at = now;
      for (const auto &m : rec->markers) {
        r.markers.push_back(store::StoredMarker{.offset_ms = m.offset_ms, .kind = m.type});
      }
      // GCOVR_EXCL_BR_STOP
      auto id = store::InsertRecording(db, r);
      if (!id) {
        stored = std::unexpected(id.error());
        return;
      }
      row = *id;
    });
    if (!stored) {
      return std::unexpected(Retry("store: " + stored.error().message));
    }
    ctx.Emit(json{{"type", "recording.new"}, {"id", row}}.dump());  // GCOVR_EXCL_BR_LINE: as above
    if (deps.on_recording) {
      deps.on_recording(row);
    }
    if (auto acked = client.Ack(info.id, sha); !acked) {
      return std::unexpected(FromClient("Ack", acked.error()));
    }
  }
  ctx.Progress(100);
  return {};
}

}  // namespace micnote::jobs
