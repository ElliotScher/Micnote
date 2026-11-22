// Link framing: round trips, frames split and merged across transport
// packets, and every way a hostile byte stream can be rejected.
#include "protocol/framing.hpp"

#include <micnote.pb.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

namespace {

using micnote::protocol::EncodeFrame;
using micnote::protocol::FrameDecoder;
using micnote::protocol::FrameError;
using micnote::protocol::kMaxFrameSize;

std::vector<std::byte> Bytes(std::initializer_list<std::uint8_t> values) {
  std::vector<std::byte> out;
  for (const std::uint8_t v : values) {
    out.push_back(std::byte{v});
  }
  return out;
}

micnote_v1_Frame SetTimeFrame(std::uint32_t req_id, std::uint64_t unix_ms) {
  micnote_v1_Frame frame = micnote_v1_Frame_init_zero;
  frame.req_id = req_id;
  frame.which_body = micnote_v1_Frame_set_time_tag;
  frame.body.set_time.unix_ms = unix_ms;
  frame.body.set_time.tz_offset_min = -300;
  return frame;
}

// The largest message: a Chunk with a full payload.
micnote_v1_Frame FullChunkFrame() {
  micnote_v1_Frame frame = micnote_v1_Frame_init_zero;
  frame.req_id = UINT32_MAX;
  frame.which_body = micnote_v1_Frame_chunk_tag;
  frame.body.chunk.recording_id = UINT64_MAX;
  frame.body.chunk.seq = UINT32_MAX;
  frame.body.chunk.payload.size = sizeof frame.body.chunk.payload.bytes;
  pb_byte_t next = 0;
  for (pb_byte_t &b : frame.body.chunk.payload.bytes) {
    b = next++;
  }
  return frame;
}

std::vector<std::byte> Encode(const micnote_v1_Frame &frame) {
  auto encoded = EncodeFrame(frame);
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<std::byte>{});
}

using NextResult = std::expected<std::optional<micnote_v1_Frame>, FrameError>;

// The frame Next() returned, or nullptr if it returned an error or nothing.
const micnote_v1_Frame *FrameOf(const NextResult &got) {
  if (!got.has_value() || !got->has_value()) {
    return nullptr;
  }
  return &**got;
}

void ExpectSetTime(const NextResult &got, std::uint32_t req_id, std::uint64_t unix_ms) {
  const micnote_v1_Frame *frame = FrameOf(got);
  ASSERT_NE(frame, nullptr);
  const micnote_v1_Frame &f = *frame;
  EXPECT_EQ(f.req_id, req_id);
  ASSERT_EQ(f.which_body, micnote_v1_Frame_set_time_tag);
  EXPECT_EQ(f.body.set_time.unix_ms, unix_ms);
  EXPECT_EQ(f.body.set_time.tz_offset_min, -300);
}

}  // namespace

TEST(Framing, RoundTrip) {
  FrameDecoder decoder;
  decoder.Push(Encode(SetTimeFrame(7, 1'763'700'000'000)));
  const auto got = decoder.Next();
  ExpectSetTime(got, 7, 1'763'700'000'000);
  EXPECT_EQ(decoder.Buffered(), 0U);
}

TEST(Framing, LargestFrameRoundTripsWithTwoBytePrefix) {
  const micnote_v1_Frame sent = FullChunkFrame();
  const std::vector<std::byte> wire = Encode(sent);
  EXPECT_EQ(wire.size(), micnote::protocol::kMaxPrefixSize + kMaxFrameSize);

  FrameDecoder decoder;
  decoder.Push(wire);
  const auto got = decoder.Next();
  const micnote_v1_Frame *decoded = FrameOf(got);
  ASSERT_NE(decoded, nullptr);
  const micnote_v1_Frame &frame = *decoded;
  EXPECT_EQ(frame.req_id, UINT32_MAX);
  ASSERT_EQ(frame.which_body, micnote_v1_Frame_chunk_tag);
  EXPECT_EQ(frame.body.chunk.recording_id, UINT64_MAX);
  EXPECT_EQ(frame.body.chunk.seq, UINT32_MAX);
  ASSERT_EQ(frame.body.chunk.payload.size, sent.body.chunk.payload.size);
  EXPECT_TRUE(std::ranges::equal(frame.body.chunk.payload.bytes, sent.body.chunk.payload.bytes));
}

TEST(Framing, EmptyFrameIsValid) {
  // A zero-length frame decodes to a Frame with no body; rejecting it is the
  // dispatcher's job (Error{BAD_REQUEST}), not the framing's.
  FrameDecoder decoder;
  decoder.Push(Bytes({0x00}));
  const auto got = decoder.Next();
  const micnote_v1_Frame *frame = FrameOf(got);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->which_body, 0U);
}

TEST(Framing, ByteAtATime) {
  const std::vector<std::byte> wire = Encode(FullChunkFrame());
  FrameDecoder decoder;
  for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
    decoder.Push(std::span(wire).subspan(i, 1));
    const auto got = decoder.Next();
    ASSERT_TRUE(got.has_value());
    ASSERT_FALSE(got->has_value()) << "frame completed early at byte " << i;
  }
  decoder.Push(std::span(wire).last(1));
  const auto got = decoder.Next();
  ASSERT_TRUE(got.has_value());
  EXPECT_TRUE(got->has_value());
}

TEST(Framing, SeveralFramesInOnePush) {
  std::vector<std::byte> wire;
  for (std::uint32_t id = 1; id <= 3; ++id) {
    const std::vector<std::byte> one = Encode(SetTimeFrame(id, id * 1000ULL));
    wire.insert(wire.end(), one.begin(), one.end());
  }
  // Half of a fourth frame, which must stay buffered.
  const std::vector<std::byte> fourth = Encode(SetTimeFrame(4, 4000));
  wire.insert(wire.end(), fourth.begin(), fourth.begin() + 3);

  FrameDecoder decoder;
  decoder.Push(wire);
  for (std::uint32_t id = 1; id <= 3; ++id) {
    const auto got = decoder.Next();
    ExpectSetTime(got, id, id * 1000ULL);
  }
  const auto pending = decoder.Next();
  ASSERT_TRUE(pending.has_value());
  EXPECT_FALSE(pending->has_value());
  EXPECT_EQ(decoder.Buffered(), 3U);

  decoder.Push(std::span(fourth).subspan(3));
  const auto got = decoder.Next();
  ExpectSetTime(got, 4, 4000);
}

TEST(Framing, NeedsMoreBytes) {
  FrameDecoder decoder;
  const auto empty = decoder.Next();
  ASSERT_TRUE(empty.has_value());
  EXPECT_FALSE(empty->has_value());

  // Half a length prefix.
  decoder.Push(Bytes({0x80}));
  const auto half_prefix = decoder.Next();
  ASSERT_TRUE(half_prefix.has_value());
  EXPECT_FALSE(half_prefix->has_value());
}

TEST(Framing, RejectsOverlongPrefix) {
  // A 3-byte varint, even one whose value would be small.
  FrameDecoder decoder;
  decoder.Push(Bytes({0x80, 0x80, 0x00}));
  EXPECT_EQ(decoder.Next(), std::unexpected(FrameError::kBadPrefix));
}

TEST(Framing, RejectsOversizedLength) {
  FrameDecoder decoder;
  // 0xFF 0x7F = 16383, past kMaxFrameSize but within two prefix bytes.
  decoder.Push(Bytes({0xFF, 0x7F}));
  EXPECT_EQ(decoder.Next(), std::unexpected(FrameError::kTooLarge));
  EXPECT_EQ(decoder.Buffered(), 0U);
}

TEST(Framing, RejectsGarbageBody) {
  // Length 2: field 1 (req_id) as a varint, cut off mid-varint.
  FrameDecoder decoder;
  decoder.Push(Bytes({0x02, 0x08, 0x80}));
  EXPECT_EQ(decoder.Next(), std::unexpected(FrameError::kDecode));
}

TEST(Framing, ErrorsAreSticky) {
  FrameDecoder decoder;
  decoder.Push(Bytes({0xFF, 0x7F}));
  ASSERT_FALSE(decoder.Next().has_value());

  // A valid frame afterwards is not decoded: the stream has lost sync.
  decoder.Push(Encode(SetTimeFrame(1, 1)));
  EXPECT_EQ(decoder.Next(), std::unexpected(FrameError::kTooLarge));
}

TEST(Framing, EncodeRejectsUnterminatedString) {
  micnote_v1_Frame frame = micnote_v1_Frame_init_zero;
  frame.which_body = micnote_v1_Frame_error_tag;
  frame.body.error.code = micnote_v1_ErrorCode_ERROR_CODE_BUSY;
  std::ranges::fill(frame.body.error.detail, 'x');  // no NUL anywhere
  EXPECT_EQ(EncodeFrame(frame), std::unexpected(FrameError::kEncode));
}
