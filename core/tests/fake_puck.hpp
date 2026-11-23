// Test helpers shared by the protocol and CLI tests: reading
// proto/testvectors, and FakePuck, an in-memory Transport that plays a puck
// serving those recordings and can misbehave on demand.
#pragma once

#include <micnote.pb.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crypto/psa.hpp"
#include "protocol/client.hpp"
#include "protocol/framing.hpp"
#include "recording/mnr.hpp"
#include "recording/reader.hpp"
#include "transport/transport.hpp"
#include <nlohmann/json.hpp>

namespace micnote::test {

using protocol::FetchedRecording;
using transport::TransportError;
using Bytes = std::vector<std::byte>;

inline std::string VectorPath(const std::string &name) {
  return std::string(MICNOTE_TESTVECTORS_DIR) + "/" + name;
}

inline Bytes ReadFile(const std::string &name) {
  std::ifstream in(VectorPath(name), std::ios::binary);
  const std::vector<char> chars{std::istreambuf_iterator<char>(in), {}};
  Bytes out(chars.size());
  std::ranges::transform(chars, out.begin(), [](char c) { return static_cast<std::byte>(c); });
  return out;
}

inline nlohmann::json Vectors() {
  std::ifstream in(VectorPath("vectors.json"));
  return nlohmann::json::parse(in);
}

inline micnote::recording::HostKey KeyNamed(const std::string &name) {
  const std::string hex = Vectors()["keys"][name]["priv"].get<std::string>();
  std::array<std::byte, 32> priv{};
  for (std::size_t i = 0; i < priv.size(); ++i) {
    priv.at(i) = static_cast<std::byte>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
  }
  return std::move(micnote::recording::HostKey::FromPrivate(priv).value());
}

// ---- the fake puck ---------------------------------------------------------

struct StoredRecording {
  Bytes header;
  std::vector<Bytes> chunks;  // payloads: ciphertext + tag
};

// Splits a .mnr file into what the puck would send: header + chunk payloads.
inline StoredRecording Split(const Bytes &file) {
  const auto h = micnote::recording::ParseHeader(file).value();
  StoredRecording r{.header = Bytes(file.begin(), file.begin() + static_cast<long>(h.length)),
                    .chunks = {}};
  for (std::size_t off = h.length; off < file.size();) {
    const std::size_t ct_len = micnote::recording::LoadLe16(std::span(file).subspan(off + 4));
    const auto begin = file.begin() + static_cast<long>(off + 6);
    r.chunks.emplace_back(begin, begin + static_cast<long>(ct_len + 16));
    off += 6 + ct_len + 16;
  }
  return r;
}

inline micnote_v1_Frame Response(std::uint32_t req_id, pb_size_t tag) {
  micnote_v1_Frame f = micnote_v1_Frame_init_zero;
  f.req_id = req_id;
  f.which_body = tag;
  return f;
}

inline micnote_v1_Frame ErrorFrame(std::uint32_t req_id, micnote_v1_ErrorCode code) {
  micnote_v1_Frame f = Response(req_id, micnote_v1_Frame_error_tag);
  f.body.error.code = code;
  return f;
}

inline FetchedRecording Fetched(std::uint64_t id) {
  FetchedRecording rec;
  rec.id = id;
  return rec;
}

class FakePuck : public micnote::transport::Transport {
 public:
  // Behaviour knobs, set directly by tests.
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
  std::size_t page_size = 16;
  std::size_t bytes_per_receive = 512;
  // Close the link after sending this many chunks (to test resume).
  std::optional<std::size_t> drop_after_chunks;
  bool fail_sends = false;
  bool fail_receives = false;
  // Replaces the normal responses to the next request.
  std::function<std::vector<micnote_v1_Frame>(const micnote_v1_Frame &)> override_next;
  // Raw bytes sent ahead of the next responses.
  Bytes inject;
  // Sent unsolicited ahead of the next responses.
  bool status_first = false;
  // Requests with this body get Error{BUSY} instead of their response.
  std::optional<pb_size_t> error_on;
  // Listed as still recording.
  std::optional<std::uint64_t> incomplete_id;
  micnote_v1_ChargeSource status_charge = micnote_v1_ChargeSource_CHARGE_SOURCE_NONE;
  bool status_rtc_valid = false;

  std::map<std::uint64_t, StoredRecording> recordings;
  std::uint64_t last_set_time = 0;
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

  std::expected<void, TransportError> Send(std::span<const std::byte> bytes) override {
    if (fail_sends) {
      return std::unexpected(TransportError::kIo);
    }
    decoder_.Push(bytes);
    while (auto req = decoder_.Next().value()) {
      Handle(*req);
    }
    return {};
  }

  std::expected<std::size_t, TransportError> Receive(
      std::span<std::byte> buf, std::chrono::milliseconds /*timeout*/) override {
    if (fail_receives) {
      return std::unexpected(TransportError::kIo);
    }
    if (out_.empty()) {
      return closed_ ? std::expected<std::size_t, TransportError>(
                           std::unexpected(TransportError::kClosed))
                     : std::expected<std::size_t, TransportError>(0);
    }
    const std::size_t n = std::min({buf.size(), bytes_per_receive, out_.size()});
    std::copy_n(out_.begin(), n, buf.begin());
    out_.erase(out_.begin(), out_.begin() + static_cast<long>(n));
    return n;
  }

  // A new link: what the host sees after reconnecting.
  void Reconnect() {
    closed_ = false;
    out_.clear();
    decoder_ = {};
  }

 private:
  void Queue(const micnote_v1_Frame &f) {
    const auto bytes = micnote::protocol::EncodeFrame(f).value();
    out_.insert(out_.end(), bytes.begin(), bytes.end());
  }

  void Handle(const micnote_v1_Frame &req) {
    out_.insert(out_.end(), inject.begin(), inject.end());
    inject.clear();
    if (status_first) {
      micnote_v1_Frame s = Response(0, micnote_v1_Frame_device_status_tag);
      s.body.device_status.battery_pct = 42;
      Queue(s);
      status_first = false;
    }
    if (override_next) {
      auto frames = std::exchange(override_next, nullptr)(req);
      for (const auto &f : frames) {
        Queue(f);
      }
      return;
    }
    const std::uint32_t id = req.req_id;
    if (error_on == req.which_body) {
      Queue(ErrorFrame(id, micnote_v1_ErrorCode_ERROR_CODE_BUSY));
      return;
    }
    switch (req.which_body) {
      case micnote_v1_Frame_hello_tag: {
        micnote_v1_Frame r = Response(id, micnote_v1_Frame_hello_ack_tag);
        r.body.hello_ack.has_device_info = true;
        r.body.hello_ack.device_info.proto_version = 1;
        std::ranges::copy(std::string_view("fake"), std::begin(r.body.hello_ack.device_info.board));
        Queue(r);
        return;
      }
      case micnote_v1_Frame_set_time_tag:
        last_set_time = req.body.set_time.unix_ms;
        Queue(Response(id, micnote_v1_Frame_ok_tag));
        return;
      case micnote_v1_Frame_get_status_tag: {
        micnote_v1_Frame r = Response(id, micnote_v1_Frame_device_status_tag);
        r.body.device_status.recording_count = static_cast<std::uint32_t>(recordings.size());
        r.body.device_status.charge_source = status_charge;
        r.body.device_status.rtc_valid = status_rtc_valid;
        r.body.device_status.battery_pct = 87;
        Queue(r);
        return;
      }
      case micnote_v1_Frame_list_recordings_tag: {
        micnote_v1_Frame r = Response(id, micnote_v1_Frame_recording_list_tag);
        auto &list = r.body.recording_list;
        for (auto it = recordings.upper_bound(req.body.list_recordings.after_id);
             it != recordings.end(); ++it) {
          if (list.entries_count == page_size) {
            list.more = true;
            break;
          }
          auto &e = std::span(list.entries)[list.entries_count++];
          e.id = it->first;
          e.complete = incomplete_id != it->first;
          e.last_seq = static_cast<std::uint32_t>(it->second.chunks.size() - 1);
        }
        Queue(r);
        return;
      }
      case micnote_v1_Frame_fetch_tag:
        HandleFetch(req);
        return;
      case micnote_v1_Frame_ack_tag:
        HandleAck(req);
        return;
      default:
        Queue(ErrorFrame(id, micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST));
    }
  }

  void HandleFetch(const micnote_v1_Frame &req) {
    const auto it = recordings.find(req.body.fetch.recording_id);
    if (it == recordings.end()) {
      Queue(ErrorFrame(req.req_id, micnote_v1_ErrorCode_ERROR_CODE_NOT_FOUND));
      return;
    }
    const StoredRecording &r = it->second;
    std::size_t sent = 0;
    for (std::size_t seq = req.body.fetch.from_seq; seq < r.chunks.size(); ++seq) {
      if (drop_after_chunks && sent == *drop_after_chunks) {
        drop_after_chunks.reset();
        closed_ = true;
        return;
      }
      micnote_v1_Frame c = Response(req.req_id, micnote_v1_Frame_chunk_tag);
      c.body.chunk.recording_id = it->first;
      c.body.chunk.seq = static_cast<std::uint32_t>(seq);
      c.body.chunk.payload.size = static_cast<pb_size_t>(r.chunks[seq].size());
      std::ranges::transform(r.chunks[seq], std::begin(c.body.chunk.payload.bytes),
                             [](std::byte b) { return std::to_integer<pb_byte_t>(b); });
      Queue(c);
      ++sent;
    }
    micnote_v1_Frame d = Response(req.req_id, micnote_v1_Frame_fetch_done_tag);
    d.body.fetch_done.recording_id = it->first;
    d.body.fetch_done.last_seq = static_cast<std::uint32_t>(r.chunks.size() - 1);
    d.body.fetch_done.header.size = static_cast<pb_size_t>(r.header.size());
    std::ranges::transform(r.header, std::begin(d.body.fetch_done.header.bytes),
                           [](std::byte b) { return std::to_integer<pb_byte_t>(b); });
    Queue(d);
  }

  void HandleAck(const micnote_v1_Frame &req) {
    const auto it = recordings.find(req.body.ack.recording_id);
    if (it == recordings.end()) {
      Queue(ErrorFrame(req.req_id, micnote_v1_ErrorCode_ERROR_CODE_NOT_FOUND));
      return;
    }
    FetchedRecording whole;
    whole.chunks = it->second.chunks;
    whole.header = it->second.header;
    const auto digest = micnote::crypto::Sha256(whole.File());
    if (!std::ranges::equal(digest, std::as_bytes(std::span(req.body.ack.sha256)))) {
      Queue(ErrorFrame(req.req_id, micnote_v1_ErrorCode_ERROR_CODE_HASH_MISMATCH));
      return;
    }
    recordings.erase(it);
    Queue(Response(req.req_id, micnote_v1_Frame_ok_tag));
  }

  micnote::protocol::FrameDecoder decoder_;
  std::deque<std::byte> out_;
  bool closed_ = false;
};

// Loads every test-vector recording into `puck`.
inline void LoadVectors(FakePuck &puck) {
  for (const auto &v : Vectors()["recordings"]) {
    const auto id = std::stoull(v["rec_id"].get<std::string>(), nullptr, 16);
    puck.recordings[id] = Split(ReadFile(v["file"].get<std::string>()));
  }
}

}  // namespace micnote::test
