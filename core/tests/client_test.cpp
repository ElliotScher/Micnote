// ProtocolClient against a fake puck that serves the real test-vector
// recordings over an in-memory transport: a whole sync (list, fetch, read,
// ack), resuming after a dropped link, and every way a puck can misbehave.
#include "protocol/client.hpp"

#include <micnote.pb.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
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
#include "protocol/framing.hpp"
#include "recording/mnr.hpp"
#include "recording/reader.hpp"
#include "transport/transport.hpp"
#include <nlohmann/json.hpp>

namespace {

using micnote::protocol::Client;
using micnote::protocol::ClientError;
using micnote::protocol::FetchedRecording;
using micnote::transport::TransportError;
using Kind = ClientError::Kind;
using Bytes = std::vector<std::byte>;

std::string VectorPath(const std::string &name) {
  return std::string(MICNOTE_TESTVECTORS_DIR) + "/" + name;
}

Bytes ReadFile(const std::string &name) {
  std::ifstream in(VectorPath(name), std::ios::binary);
  const std::vector<char> chars{std::istreambuf_iterator<char>(in), {}};
  Bytes out(chars.size());
  std::ranges::transform(chars, out.begin(), [](char c) { return static_cast<std::byte>(c); });
  return out;
}

nlohmann::json Vectors() {
  std::ifstream in(VectorPath("vectors.json"));
  return nlohmann::json::parse(in);
}

micnote::recording::HostKey KeyNamed(const std::string &name) {
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
StoredRecording Split(const Bytes &file) {
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

micnote_v1_Frame Response(std::uint32_t req_id, pb_size_t tag) {
  micnote_v1_Frame f = micnote_v1_Frame_init_zero;
  f.req_id = req_id;
  f.which_body = tag;
  return f;
}

micnote_v1_Frame ErrorFrame(std::uint32_t req_id, micnote_v1_ErrorCode code) {
  micnote_v1_Frame f = Response(req_id, micnote_v1_Frame_error_tag);
  f.body.error.code = code;
  return f;
}

FetchedRecording Fetched(std::uint64_t id) {
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
          e.complete = true;
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

constexpr std::array<std::byte, 8> kHostId{};
constexpr std::array<std::byte, 16> kNonce{};

class ClientTest : public ::testing::Test {
 protected:
  ClientTest() {
    for (const auto &v : Vectors()["recordings"]) {
      const auto id = std::stoull(v["rec_id"].get<std::string>(), nullptr, 16);
      puck_.recordings[id] = Split(ReadFile(v["file"].get<std::string>()));
    }
  }

  FakePuck &puck() { return puck_; }
  Client &client() { return client_; }

  ClientError Error(const auto &result) {
    EXPECT_FALSE(result.has_value());
    return result.has_value() ? ClientError{} : result.error();
  }

 private:
  FakePuck puck_;
  Client client_{puck_, std::chrono::milliseconds(10)};
};

}  // namespace

// ---- a whole sync ----------------------------------------------------------

TEST_F(ClientTest, SyncsEveryVectorEndToEnd) {
  puck().bytes_per_receive = 7;  // frames split across many reads
  const auto ack = client().Hello(kHostId, kNonce);
  ASSERT_TRUE(ack.has_value());
  EXPECT_EQ(std::string_view(std::begin(ack->device_info.board)), "fake");
  ASSERT_TRUE(client().SetTime(1763800000000ULL, -300).has_value());
  EXPECT_EQ(puck().last_set_time, 1763800000000ULL);

  const auto list = client().ListRecordings();
  ASSERT_TRUE(list.has_value());
  ASSERT_EQ(list->size(), 3U);

  const auto vectors = Vectors();
  for (const auto &info : *list) {
    FetchedRecording rec = Fetched(info.id);
    ASSERT_TRUE(client().Fetch(rec).has_value());
    ASSERT_TRUE(rec.complete);
    EXPECT_EQ(rec.chunks.size(), info.last_seq + 1);

    const Bytes file = rec.File();
    const auto v = std::ranges::find_if(vectors["recordings"], [&](const auto &r) {
      return std::stoull(r["rec_id"].template get<std::string>(), nullptr, 16) == info.id;
    });
    ASSERT_NE(v, vectors["recordings"].end());
    EXPECT_EQ(file, ReadFile((*v)["file"].get<std::string>())) << "reassembled byte for byte";

    const auto read =
        micnote::recording::ReadRecording(file, KeyNamed((*v)["recipients"][0].get<std::string>()));
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->packets.size(), (*v)["packets"].get<std::size_t>());

    ASSERT_TRUE(client().Ack(info.id, micnote::crypto::Sha256(file)).has_value());
  }
  EXPECT_TRUE(puck().recordings.empty()) << "every ack deleted its recording";
  const auto status = client().GetStatus();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->recording_count, 0U);
}

TEST_F(ClientTest, ListFollowsPages) {
  puck().page_size = 1;
  const auto list = client().ListRecordings();
  ASSERT_TRUE(list.has_value());
  ASSERT_EQ(list->size(), 3U);
  EXPECT_TRUE(std::ranges::is_sorted(*list, {}, &micnote_v1_RecordingInfo::id));
}

TEST_F(ClientTest, FetchResumesAfterDroppedLink) {
  const std::uint64_t id = puck().recordings.begin()->first;
  puck().drop_after_chunks = 2;
  FetchedRecording rec = Fetched(id);
  EXPECT_EQ(Error(client().Fetch(rec)).kind, Kind::kTransport);
  EXPECT_EQ(rec.chunks.size(), 2U);
  EXPECT_FALSE(rec.complete);

  puck().Reconnect();
  Client again(puck(), std::chrono::milliseconds(10));
  ASSERT_TRUE(again.Fetch(rec).has_value());
  EXPECT_TRUE(rec.complete);
  const auto &stored = puck().recordings.at(id);
  EXPECT_EQ(rec.chunks, stored.chunks);
}

TEST_F(ClientTest, UnsolicitedStatusIsRecordedAndSkipped) {
  puck().status_first = true;
  EXPECT_FALSE(client().last_status().has_value());
  ASSERT_TRUE(client().SetTime(1, 0).has_value());
  ASSERT_TRUE(client().last_status().has_value());
  EXPECT_EQ(client().last_status().value_or(micnote_v1_DeviceStatus{}).battery_pct, 42U);
}

// ---- puck and device errors ------------------------------------------------

TEST_F(ClientTest, DeviceErrorsArePassedOn) {
  FetchedRecording missing = Fetched(12345);
  EXPECT_EQ(
      Error(client().Fetch(missing)),
      (ClientError{.kind = Kind::kDevice, .device = micnote_v1_ErrorCode_ERROR_CODE_NOT_FOUND}));

  const std::uint64_t id = puck().recordings.begin()->first;
  EXPECT_EQ(Error(client().Ack(id, micnote::crypto::Sha256Digest{})).device,
            micnote_v1_ErrorCode_ERROR_CODE_HASH_MISMATCH);
  EXPECT_TRUE(puck().recordings.contains(id)) << "a bad hash deletes nothing";
}

TEST_F(ClientTest, DeviceErrorDuringList) {
  puck().override_next = [](const micnote_v1_Frame &req) {
    return std::vector{ErrorFrame(req.req_id, micnote_v1_ErrorCode_ERROR_CODE_STORAGE)};
  };
  EXPECT_EQ(Error(client().ListRecordings()).device, micnote_v1_ErrorCode_ERROR_CODE_STORAGE);
}

TEST_F(ClientTest, RejectsWrongReqId) {
  puck().override_next = [](const micnote_v1_Frame &req) {
    return std::vector{Response(req.req_id + 1, micnote_v1_Frame_ok_tag)};
  };
  EXPECT_EQ(Error(client().SetTime(1, 0)).kind, Kind::kProtocol);
}

// Only DeviceStatus may arrive unsolicited.
TEST_F(ClientTest, RejectsOtherUnsolicitedFrames) {
  puck().override_next = [](const micnote_v1_Frame &) {
    return std::vector{Response(0, micnote_v1_Frame_ok_tag)};
  };
  EXPECT_EQ(Error(client().SetTime(1, 0)).kind, Kind::kProtocol);
}

TEST_F(ClientTest, RejectsWrongResponseBody) {
  puck().override_next = [](const micnote_v1_Frame &req) {
    return std::vector{Response(req.req_id, micnote_v1_Frame_ok_tag)};
  };
  EXPECT_EQ(Error(client().GetStatus()).kind, Kind::kProtocol);
}

TEST_F(ClientTest, RejectsListThatDoesNotAdvance) {
  puck().override_next = [](const micnote_v1_Frame &req) {
    micnote_v1_Frame r = Response(req.req_id, micnote_v1_Frame_recording_list_tag);
    r.body.recording_list.more = true;  // "more", but no entries
    return std::vector{r};
  };
  EXPECT_EQ(Error(client().ListRecordings()).kind, Kind::kProtocol);

  puck().override_next = [](const micnote_v1_Frame &req) {
    micnote_v1_Frame r = Response(req.req_id, micnote_v1_Frame_recording_list_tag);
    r.body.recording_list.more = true;
    r.body.recording_list.entries_count = 1;
    r.body.recording_list.entries[0].id = 0;  // not after after_id = 0
    return std::vector{r};
  };
  EXPECT_EQ(Error(client().ListRecordings()).kind, Kind::kProtocol);
}

namespace {

// A Fetch response with a chunk the client must refuse.
auto BadChunk(void (*mutate)(micnote_v1_Chunk &)) {
  return [mutate](const micnote_v1_Frame &req) {
    micnote_v1_Frame c = Response(req.req_id, micnote_v1_Frame_chunk_tag);
    c.body.chunk.recording_id = req.body.fetch.recording_id;
    c.body.chunk.payload.size = 17;
    mutate(c.body.chunk);
    return std::vector{c};
  };
}

}  // namespace

TEST_F(ClientTest, RejectsBadChunks) {
  const std::uint64_t id = puck().recordings.begin()->first;
  const std::vector<void (*)(micnote_v1_Chunk &)> mutations{
      [](micnote_v1_Chunk &c) { c.recording_id += 1; },
      [](micnote_v1_Chunk &c) { c.seq = 1; },
      [](micnote_v1_Chunk &c) { c.payload.size = 16; },
  };
  for (const auto &mutate : mutations) {
    puck().Reconnect();
    Client fresh(puck(), std::chrono::milliseconds(10));
    puck().override_next = BadChunk(mutate);
    FetchedRecording rec = Fetched(id);
    EXPECT_EQ(Error(fresh.Fetch(rec)).kind, Kind::kProtocol);
  }
}

TEST_F(ClientTest, RejectsBadFetchEnd) {
  const std::uint64_t id = puck().recordings.begin()->first;
  const auto respond = [](pb_size_t tag, std::uint64_t rec_id, std::uint32_t last_seq) {
    return [=](const micnote_v1_Frame &req) {
      micnote_v1_Frame c = Response(req.req_id, micnote_v1_Frame_chunk_tag);
      c.body.chunk.recording_id = req.body.fetch.recording_id;
      c.body.chunk.payload.size = 17;
      micnote_v1_Frame end = Response(req.req_id, tag);
      end.body.fetch_done.recording_id = rec_id;
      end.body.fetch_done.last_seq = last_seq;
      return std::vector{c, end};
    };
  };
  // Not a Chunk or FetchDone; FetchDone for another recording; wrong last_seq.
  for (auto r : {respond(micnote_v1_Frame_ok_tag, id, 0),
                 respond(micnote_v1_Frame_fetch_done_tag, id + 1, 0),
                 respond(micnote_v1_Frame_fetch_done_tag, id, 5)}) {
    puck().Reconnect();
    Client fresh(puck(), std::chrono::milliseconds(10));
    puck().override_next = r;
    FetchedRecording rec = Fetched(id);
    EXPECT_EQ(Error(fresh.Fetch(rec)).kind, Kind::kProtocol);
  }
}

// A recording still being written may have no chunks yet: FetchDone then
// carries just the header, and last_seq means nothing.
TEST_F(ClientTest, FetchWithNoChunksYet) {
  const std::uint64_t id = puck().recordings.begin()->first;
  puck().override_next = [id](const micnote_v1_Frame &req) {
    micnote_v1_Frame end = Response(req.req_id, micnote_v1_Frame_fetch_done_tag);
    end.body.fetch_done.recording_id = id;
    end.body.fetch_done.header.size = 3;
    return std::vector{end};
  };
  FetchedRecording rec = Fetched(id);
  ASSERT_TRUE(client().Fetch(rec).has_value());
  EXPECT_TRUE(rec.complete);
  EXPECT_EQ(rec.header.size(), 3U);
  EXPECT_EQ(rec.File(), rec.header);
}

// ---- link failures ---------------------------------------------------------

TEST_F(ClientTest, Timeout) {
  puck().override_next = [](const micnote_v1_Frame &) { return std::vector<micnote_v1_Frame>{}; };
  EXPECT_EQ(Error(client().GetStatus()).kind, Kind::kTimeout);
}

TEST_F(ClientTest, FramingErrorIsFatal) {
  puck().inject = {std::byte{0xFF}, std::byte{0x7F}};  // an oversized length
  EXPECT_EQ(Error(client().GetStatus()).kind, Kind::kFraming);
  EXPECT_EQ(Error(client().GetStatus()).kind, Kind::kFraming) << "the stream stays lost";
}

TEST_F(ClientTest, TransportFailures) {
  puck().fail_sends = true;
  EXPECT_EQ(Error(client().GetStatus()).kind, Kind::kTransport);
  FetchedRecording rec = Fetched(1);
  EXPECT_EQ(Error(client().Fetch(rec)).kind, Kind::kTransport);
  puck().fail_sends = false;
  puck().fail_receives = true;
  EXPECT_EQ(Error(client().GetStatus()).kind, Kind::kTransport);
}

TEST(ClientError, Equality) {
  const ClientError busy{.kind = Kind::kDevice, .device = micnote_v1_ErrorCode_ERROR_CODE_BUSY};
  EXPECT_EQ(busy, busy);
  EXPECT_NE(busy, (ClientError{.kind = Kind::kTimeout, .device = busy.device}));
  EXPECT_NE(busy, (ClientError{.kind = Kind::kDevice,
                               .device = micnote_v1_ErrorCode_ERROR_CODE_STORAGE}));
}
