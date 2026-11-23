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
#include "fake_puck.hpp"
#include "protocol/framing.hpp"
#include "recording/mnr.hpp"
#include "recording/reader.hpp"
#include "transport/transport.hpp"
#include <nlohmann/json.hpp>

namespace {

using micnote::protocol::Client;
using micnote::protocol::ClientError;
using micnote::test::Bytes;
using micnote::test::ErrorFrame;
using micnote::test::FakePuck;
using micnote::test::Fetched;
using micnote::test::FetchedRecording;
using micnote::test::KeyNamed;
using micnote::test::ReadFile;
using micnote::test::Response;
using micnote::test::Vectors;
using Kind = ClientError::Kind;

constexpr std::array<std::byte, 8> kHostId{};
constexpr std::array<std::byte, 16> kNonce{};

class ClientTest : public ::testing::Test {
 protected:
  ClientTest() { micnote::test::LoadVectors(puck_); }

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
