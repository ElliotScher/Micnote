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

#include "crypto/pairing.hpp"
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

  // Sessions and pairing (protocol sections 3 and 8).
  std::array<std::byte, 8> device_id{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                     std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  // Paired hosts: link key and name.
  struct Host {
    micnote::crypto::LinkKey link_key{};
    std::string name;
  };
  std::map<std::array<std::byte, 8>, Host> hosts;
  // As the firmware: unknown hosts get UNPAIRED outside the PAIRING window,
  // and only Hello, GetStatus, Auth and PairBegin work before Auth. Off for
  // the protocol-client tests, which don't authenticate.
  bool strict = false;
  // The PAIRING window is open.
  bool pairing = false;
  // The button gets pressed after PairConfirm{true}; if not, no answer.
  bool press_button = true;
  // PairChallenge carries an all-zero (low-order) device key.
  bool low_order_device_key = false;
  // Set when the host cancels with PairConfirm{false}.
  bool pair_cancelled = false;
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
    authed_ = false;
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
    const bool before_auth = req.which_body != micnote_v1_Frame_hello_tag &&
                             req.which_body != micnote_v1_Frame_get_status_tag &&
                             req.which_body != micnote_v1_Frame_auth_tag &&
                             req.which_body != micnote_v1_Frame_pair_begin_tag &&
                             req.which_body != micnote_v1_Frame_pair_confirm_tag;
    if (strict && !authed_ && before_auth) {
      Queue(ErrorFrame(id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE));
      return;
    }
    switch (req.which_body) {
      case micnote_v1_Frame_hello_tag:
        HandleHello(req);
        return;
      case micnote_v1_Frame_auth_tag:
        HandleAuth(req);
        return;
      case micnote_v1_Frame_pair_begin_tag:
        HandlePairBegin(req);
        return;
      case micnote_v1_Frame_pair_confirm_tag:
        HandlePairConfirm(req);
        return;
      case micnote_v1_Frame_list_hosts_tag: {
        micnote_v1_Frame r = Response(id, micnote_v1_Frame_host_list_tag);
        for (const auto &[host_id, host] : hosts) {
          auto &e = std::span(r.body.host_list.hosts)[r.body.host_list.hosts_count];
          std::ranges::transform(host_id, std::begin(e.host_id),
                                 [](std::byte b) { return std::to_integer<pb_byte_t>(b); });
          std::ranges::copy(host.name, std::begin(e.name));
          e.slot = r.body.host_list.hosts_count++;
        }
        Queue(r);
        return;
      }
      case micnote_v1_Frame_unpair_tag:
        Queue(hosts.erase(Id(req.body.unpair.host_id)) == 1
                  ? Response(id, micnote_v1_Frame_ok_tag)
                  : ErrorFrame(id, micnote_v1_ErrorCode_ERROR_CODE_NOT_FOUND));
        return;
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

  static std::array<std::byte, 8> Id(const pb_byte_t (&bytes)[8]) {
    std::array<std::byte, 8> id{};
    std::ranges::transform(bytes, id.begin(), [](pb_byte_t b) { return std::byte{b}; });
    return id;
  }

  void HandleHello(const micnote_v1_Frame &req) {
    host_id_ = Id(req.body.hello.host_id);
    std::ranges::transform(req.body.hello.host_nonce, n_h_.begin(),
                           [](pb_byte_t b) { return std::byte{b}; });
    for (std::size_t i = 0; i < n_d_.size(); ++i) {
      n_d_.at(i) = static_cast<std::byte>(0xD0 + i);
    }
    authed_ = false;
    const auto host = hosts.find(host_id_);
    if (strict && host == hosts.end() && !pairing) {
      Queue(ErrorFrame(req.req_id, micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED));
      return;
    }
    micnote_v1_Frame r = Response(req.req_id, micnote_v1_Frame_hello_ack_tag);
    auto &ack = r.body.hello_ack;
    ack.has_device_info = true;
    ack.device_info.proto_version = 1;
    std::ranges::copy(std::string_view("fake"), std::begin(ack.device_info.board));
    std::ranges::transform(device_id, std::begin(ack.device_info.device_id),
                           [](std::byte b) { return std::to_integer<pb_byte_t>(b); });
    std::ranges::transform(n_d_, std::begin(ack.device_nonce),
                           [](std::byte b) { return std::to_integer<pb_byte_t>(b); });
    if (host != hosts.end()) {
      const auto proof = micnote::crypto::MakeProof(host->second.link_key,
                                                    micnote::crypto::kProofPuck, n_h_, n_d_);
      std::ranges::transform(proof, std::begin(ack.device_proof.bytes),
                             [](std::byte b) { return std::to_integer<pb_byte_t>(b); });
      ack.device_proof.size = static_cast<pb_size_t>(proof.size());
    }
    Queue(r);
  }

  void HandleAuth(const micnote_v1_Frame &req) {
    const auto host = hosts.find(host_id_);
    if (host == hosts.end() ||
        !micnote::crypto::VerifyProof(host->second.link_key, micnote::crypto::kProofHost, n_h_,
                                      n_d_, std::as_bytes(std::span(req.body.auth.host_proof)))) {
      Queue(ErrorFrame(req.req_id, micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED));
      return;
    }
    authed_ = true;
    Queue(Response(req.req_id, micnote_v1_Frame_ok_tag));
  }

  void HandlePairBegin(const micnote_v1_Frame &req) {
    std::array<std::byte, 32> host_pub{};
    std::ranges::transform(req.body.pair_begin.host_pub, host_pub.begin(),
                           [](pb_byte_t b) { return std::byte{b}; });
    const auto digest = micnote::crypto::Sha256(host_pub);
    if (!pairing) {
      Queue(ErrorFrame(req.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE));
      return;
    }
    if (!std::ranges::equal(std::span(digest).first(8), host_id_)) {
      Queue(ErrorFrame(req.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST));
      return;
    }
    if (hosts.size() >= 4 && !hosts.contains(host_id_)) {
      Queue(ErrorFrame(req.req_id, micnote_v1_ErrorCode_ERROR_CODE_HOSTS_FULL));
      return;
    }
    // The device's pairing key: RFC 7748 section 6.1's Alice, fixed so tests
    // are repeatable. Only the link key is needed here, and HKDF's link-key
    // inputs don't depend on which public key comes first.
    std::array<std::byte, 32> d_priv{};
    constexpr std::string_view kAlice =
        "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
    for (std::size_t i = 0; i < d_priv.size(); ++i) {
      d_priv.at(i) =
          static_cast<std::byte>(std::stoul(std::string(kAlice.substr(2 * i, 2)), nullptr, 16));
    }
    const auto d = micnote::crypto::ImportX25519(d_priv).value();
    const auto d_pub = micnote::crypto::PublicKey(d).value();
    const auto derived =
        micnote::crypto::DerivePairing(d, d_pub, host_pub, host_id_, device_id, n_h_, n_d_).value();
    pending_ = Host{.link_key = derived.link_key,
                    .name = std::string(std::begin(req.body.pair_begin.host_name),
                                        std::ranges::find(req.body.pair_begin.host_name, '\0'))};
    micnote_v1_Frame r = Response(req.req_id, micnote_v1_Frame_pair_challenge_tag);
    if (!low_order_device_key) {
      std::ranges::transform(d_pub, std::begin(r.body.pair_challenge.device_pub),
                             [](std::byte b) { return std::to_integer<pb_byte_t>(b); });
    }
    std::ranges::transform(n_d_, std::begin(r.body.pair_challenge.device_nonce),
                           [](std::byte b) { return std::to_integer<pb_byte_t>(b); });
    Queue(r);
  }

  void HandlePairConfirm(const micnote_v1_Frame &req) {
    if (!pending_) {
      Queue(ErrorFrame(req.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE));
      return;
    }
    if (!req.body.pair_confirm.user_confirmed) {
      pending_.reset();
      pair_cancelled = true;
      Queue(Response(req.req_id, micnote_v1_Frame_ok_tag));
      return;
    }
    if (!press_button) {
      return;  // nobody presses it: the host times out
    }
    if (const auto pending = std::exchange(pending_, std::nullopt); pending) {
      hosts[host_id_] = *pending;
    }
    authed_ = true;
    micnote_v1_Frame r = Response(req.req_id, micnote_v1_Frame_pair_done_tag);
    r.body.pair_done.host_slot =
        static_cast<std::uint32_t>(std::distance(hosts.begin(), hosts.find(host_id_)));
    Queue(r);
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
  std::array<std::byte, 8> host_id_{};
  micnote::crypto::Nonce n_h_{};
  micnote::crypto::Nonce n_d_{};
  bool authed_ = false;
  std::optional<Host> pending_;
};

// Loads every test-vector recording into `puck`.
inline void LoadVectors(FakePuck &puck) {
  // Keep the JSON alive: a range-for over Vectors()["recordings"] would
  // iterate a temporary that's already gone before C++23's lifetime
  // extension (P2718), which gcc 14 and MSVC don't implement yet.
  const nlohmann::json vectors = Vectors();
  for (const auto &v : vectors["recordings"]) {
    const auto id = std::stoull(v["rec_id"].get<std::string>(), nullptr, 16);
    puck.recordings[id] = Split(ReadFile(v["file"].get<std::string>()));
  }
}

}  // namespace micnote::test
