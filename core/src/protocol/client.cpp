#include "protocol/client.hpp"

#include <algorithm>
#include <array>

#include "recording/mnr.hpp"

namespace micnote::protocol {

namespace {

using Kind = ClientError::Kind;

std::unexpected<ClientError> Fail(Kind kind) { return std::unexpected(ClientError{.kind = kind}); }

micnote_v1_Frame Request(pb_size_t tag) {
  micnote_v1_Frame f = micnote_v1_Frame_init_zero;
  f.which_body = tag;
  return f;
}

template <std::size_t N>
void CopyTo(std::span<const std::byte, N> from, pb_byte_t (&to)[N]) {
  std::ranges::transform(from, std::begin(to),
                         [](std::byte b) { return std::to_integer<pb_byte_t>(b); });
}

void PutLe(std::uint64_t value, std::size_t n, std::vector<std::byte> &out) {
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(static_cast<std::byte>(value >> (8U * i)));
  }
}

// A chunk payload is ciphertext (at least 1 byte) plus the 16-byte tag. The
// upper limit (4112) is Chunk.payload's max_size: the decoder enforces it.
constexpr std::size_t kMinPayload = 1 + recording::kTagSize;

}  // namespace

std::vector<std::byte> FetchedRecording::File() const {
  std::vector<std::byte> file = header;
  for (std::size_t seq = 0; seq < chunks.size(); ++seq) {
    PutLe(seq, 4, file);
    PutLe(chunks[seq].size() - recording::kTagSize, 2, file);
    file.insert(file.end(), chunks[seq].begin(), chunks[seq].end());
  }
  return file;
}  // GCOVR_EXCL_LINE: gcc puts the exception cleanup of `file` here

Client::Client(transport::Transport &link, std::chrono::milliseconds timeout)
    : link_(&link), timeout_(timeout) {}

Result<std::uint32_t> Client::Send(micnote_v1_Frame &request) {
  request.req_id = next_req_id_++;
  // GCOVR_EXCL_START: wrapping takes 2^32 requests on one link.
  if (next_req_id_ == 0) {
    next_req_id_ = 1;  // 0 marks unsolicited messages
  }
  // GCOVR_EXCL_STOP
  const auto bytes = EncodeFrame(request);
  // GCOVR_EXCL_START: requests are built here from fixed-size fields, so
  // encoding can't fail (strings are NUL-terminated by construction).
  if (!bytes) {
    return Fail(Kind::kProtocol);
  }
  // GCOVR_EXCL_STOP
  if (!link_->Send(*bytes)) {
    return Fail(Kind::kTransport);
  }
  return request.req_id;
}

Result<micnote_v1_Frame> Client::Receive(std::uint32_t req_id) {
  std::array<std::byte, 512> buf{};
  for (;;) {
    auto next = decoder_.Next();
    if (!next) {
      return Fail(Kind::kFraming);
    }
    if (!next->has_value()) {
      auto got = link_->Receive(buf, timeout_);
      if (!got) {
        return Fail(Kind::kTransport);
      }
      if (*got == 0) {
        return Fail(Kind::kTimeout);
      }
      decoder_.Push(std::span(buf).first(*got));
      continue;
    }
    const micnote_v1_Frame &frame = **next;
    if (frame.req_id == 0 && frame.which_body == micnote_v1_Frame_device_status_tag) {
      last_status_ = frame.body.device_status;
      continue;
    }
    if (frame.req_id != req_id) {
      return Fail(Kind::kProtocol);
    }
    if (frame.which_body == micnote_v1_Frame_error_tag) {
      return std::unexpected(ClientError{.kind = Kind::kDevice, .device = frame.body.error.code});
    }
    return frame;
  }
}

Result<micnote_v1_Frame> Client::Call(micnote_v1_Frame &request, pb_size_t tag) {
  auto req_id = Send(request);
  if (!req_id) {
    return std::unexpected(req_id.error());
  }
  auto response = Receive(*req_id);
  if (response && response->which_body != tag) {
    return Fail(Kind::kProtocol);
  }
  return response;
}

Result<micnote_v1_HelloAck> Client::Hello(std::span<const std::byte, 8> host_id,
                                          std::span<const std::byte, 16> host_nonce) {
  micnote_v1_Frame f = Request(micnote_v1_Frame_hello_tag);
  f.body.hello.proto_version = kProtoVersion;
  CopyTo(host_id, f.body.hello.host_id);
  CopyTo(host_nonce, f.body.hello.host_nonce);
  return Call(f, micnote_v1_Frame_hello_ack_tag).transform([](const micnote_v1_Frame &r) {
    return r.body.hello_ack;
  });
}

Result<void> Client::SetTime(std::uint64_t unix_ms, std::int32_t tz_offset_min) {
  micnote_v1_Frame f = Request(micnote_v1_Frame_set_time_tag);
  f.body.set_time.unix_ms = unix_ms;
  f.body.set_time.tz_offset_min = tz_offset_min;
  return Call(f, micnote_v1_Frame_ok_tag).transform([](const micnote_v1_Frame &) {});
}

Result<micnote_v1_DeviceStatus> Client::GetStatus() {
  micnote_v1_Frame f = Request(micnote_v1_Frame_get_status_tag);
  return Call(f, micnote_v1_Frame_device_status_tag).transform([](const micnote_v1_Frame &r) {
    return r.body.device_status;
  });
}

Result<std::vector<micnote_v1_RecordingInfo>> Client::ListRecordings() {
  std::vector<micnote_v1_RecordingInfo> all;
  for (std::uint64_t after = 0;;) {
    micnote_v1_Frame f = Request(micnote_v1_Frame_list_recordings_tag);
    f.body.list_recordings.after_id = after;
    auto page = Call(f, micnote_v1_Frame_recording_list_tag);
    if (!page) {
      return std::unexpected(page.error());
    }
    const micnote_v1_RecordingList &list = page->body.recording_list;
    // The decoder guarantees entries_count <= the array size.
    const auto entries = std::span(list.entries).first(list.entries_count);
    // Each page must move forward, or a confused puck could loop us forever.
    if (list.more && (entries.empty() || entries.back().id <= after)) {
      return Fail(Kind::kProtocol);
    }
    all.insert(all.end(), entries.begin(), entries.end());
    if (!list.more) {
      return all;
    }
    after = entries.back().id;
  }
}

Result<void> Client::Fetch(FetchedRecording &into) {
  micnote_v1_Frame f = Request(micnote_v1_Frame_fetch_tag);
  f.body.fetch.recording_id = into.id;
  f.body.fetch.from_seq = static_cast<std::uint32_t>(into.chunks.size());
  auto req_id = Send(f);
  if (!req_id) {
    return std::unexpected(req_id.error());
  }
  for (;;) {
    auto frame = Receive(*req_id);
    if (!frame) {
      return std::unexpected(frame.error());
    }
    if (frame->which_body == micnote_v1_Frame_chunk_tag) {
      const micnote_v1_Chunk &c = frame->body.chunk;
      if (c.recording_id != into.id || c.seq != into.chunks.size() ||
          c.payload.size < kMinPayload) {
        return Fail(Kind::kProtocol);
      }
      const auto payload = std::as_bytes(std::span(c.payload.bytes).first(c.payload.size));
      into.chunks.emplace_back(payload.begin(), payload.end());
      continue;
    }
    if (frame->which_body != micnote_v1_Frame_fetch_done_tag) {
      return Fail(Kind::kProtocol);
    }
    const micnote_v1_FetchDone &done = frame->body.fetch_done;
    // last_seq is the seq of the last chunk; it only means something once
    // there is one.
    if (done.recording_id != into.id ||
        (!into.chunks.empty() && done.last_seq + 1 != into.chunks.size())) {
      return Fail(Kind::kProtocol);
    }
    const auto header = std::as_bytes(std::span(done.header.bytes).first(done.header.size));
    into.header.assign(header.begin(), header.end());
    into.complete = true;
    return {};
  }
}

Result<void> Client::Ack(std::uint64_t recording_id, const crypto::Sha256Digest &sha256) {
  micnote_v1_Frame f = Request(micnote_v1_Frame_ack_tag);
  f.body.ack.recording_id = recording_id;
  CopyTo(std::span<const std::byte, crypto::kSha256Size>(sha256), f.body.ack.sha256);
  return Call(f, micnote_v1_Frame_ok_tag).transform([](const micnote_v1_Frame &) {});
}

}  // namespace micnote::protocol
