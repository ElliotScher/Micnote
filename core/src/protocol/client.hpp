// The host side of the control protocol (docs/protocol.md sections 3-5):
// blocking request/response calls over one Transport, one request at a time.
// Runs on a worker thread; the app gets an async wrapper later (stage 2).
#pragma once

#include <micnote.pb.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "crypto/pairing.hpp"
#include "crypto/psa.hpp"
#include "protocol/framing.hpp"
#include "transport/transport.hpp"

namespace micnote::protocol {

inline constexpr std::uint32_t kProtoVersion = 1;

struct ClientError {
  enum class Kind : std::uint8_t {
    // The transport failed or closed.
    kTransport,
    // No complete response within the timeout.
    kTimeout,
    // The byte stream lost sync (see FrameError): drop the link.
    kFraming,
    // A response that doesn't fit the request (wrong req_id, body, recording
    // or sequence).
    kProtocol,
    // The puck answered Error{device}.
    kDevice,
  };
  Kind kind = Kind::kProtocol;
  micnote_v1_ErrorCode device = micnote_v1_ErrorCode_ERROR_CODE_UNSPECIFIED;

  bool operator==(const ClientError &) const = default;
};

template <typename T>
using Result = std::expected<T, ClientError>;

// A recording as fetched so far. Fetch() appends to it, so after a dropped
// link the same object resumes where it stopped.
struct FetchedRecording {
  std::uint64_t id = 0;
  // Chunk payloads (ciphertext + tag) by seq.
  std::vector<std::vector<std::byte>> chunks;
  // From FetchDone; empty until the fetch completes.
  std::vector<std::byte> header;
  bool complete = false;

  // The .mnr file exactly as the puck stores it: header, then every chunk
  // record. ReadRecording() parses this, and its SHA-256 is Ack's hash.
  [[nodiscard]] std::vector<std::byte> File() const;
};

class Client {
 public:
  explicit Client(transport::Transport &link,
                  std::chrono::milliseconds timeout = std::chrono::seconds(5));

  Result<micnote_v1_HelloAck> Hello(std::span<const std::byte, 8> host_id,
                                    std::span<const std::byte, 16> host_nonce);
  // Proves this host holds the link key (protocol section 3).
  Result<void> Auth(const crypto::Proof &host_proof);
  Result<void> SetTime(std::uint64_t unix_ms, std::int32_t tz_offset_min);
  Result<micnote_v1_DeviceStatus> GetStatus();
  // Every recording on the puck, following pagination.
  Result<std::vector<micnote_v1_RecordingInfo>> ListRecordings();
  // Streams the chunks `into` doesn't have yet, then the header.
  Result<void> Fetch(FetchedRecording &into);
  // Tells the puck the host has it all; the puck deletes it on a match.
  Result<void> Ack(std::uint64_t recording_id, const crypto::Sha256Digest &sha256);

  // Pairing (protocol section 8). PairConfirm waits up to `wait` for the
  // user to press the puck's button, and returns the host's slot.
  Result<micnote_v1_PairChallenge> PairBegin(
      std::span<const std::byte, crypto::kX25519KeySize> host_pub, std::string_view host_name);
  Result<std::uint32_t> PairConfirm(std::chrono::milliseconds wait);
  Result<void> PairCancel();
  Result<std::vector<micnote_v1_HostInfo>> ListHosts();
  Result<void> Unpair(std::span<const std::byte, 8> host_id);

  // The latest DeviceStatus the puck sent unsolicited (req_id 0), if any.
  [[nodiscard]] const std::optional<micnote_v1_DeviceStatus> &last_status() const {
    return last_status_;
  }

 private:
  // Sends `request` with a fresh req_id; returns its req_id.
  Result<std::uint32_t> Send(micnote_v1_Frame &request);
  // The next frame answering `req_id`, recording unsolicited ones on the way;
  // waits up to `timeout` for each read.
  Result<micnote_v1_Frame> Receive(std::uint32_t req_id, std::chrono::milliseconds timeout);
  // Send + Receive, expecting one response with body `tag`.
  Result<micnote_v1_Frame> Call(micnote_v1_Frame &request, pb_size_t tag);
  Result<micnote_v1_Frame> Call(micnote_v1_Frame &request, pb_size_t tag,
                                std::chrono::milliseconds timeout);

  transport::Transport *link_;  // never null; a pointer so Client stays assignable
  std::chrono::milliseconds timeout_;
  FrameDecoder decoder_;
  std::uint32_t next_req_id_ = 1;
  std::optional<micnote_v1_DeviceStatus> last_status_;
};

}  // namespace micnote::protocol
