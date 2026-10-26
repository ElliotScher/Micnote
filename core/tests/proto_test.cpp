// Checks the committed nanopb output against the pinned runtime: a round
// trip, and the fixed field sizes from micnote.options holding on decode.
#include <micnote.pb.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

micnote_v1_DeviceInfo SampleDeviceInfo() {
  micnote_v1_DeviceInfo info = micnote_v1_DeviceInfo_init_zero;
  info.has_fw_version = true;
  info.fw_version = {.major = 1, .minor = 2, .patch = 3};
  info.proto_version = 1;
  constexpr std::array<pb_byte_t, 8> kId{0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04};
  std::ranges::copy(kId, std::begin(info.device_id));
  // board is zeroed above, so the copy stays NUL-terminated.
  constexpr std::string_view kBoard = "thingy53";
  std::ranges::copy(kBoard, std::begin(info.board));
  return info;
}

bool Decode(const std::vector<pb_byte_t> &bytes, micnote_v1_DeviceInfo &out) {
  out = micnote_v1_DeviceInfo_init_zero;
  pb_istream_t in = pb_istream_from_buffer(bytes.data(), bytes.size());
  return pb_decode(&in, micnote_v1_DeviceInfo_fields, &out);
}

}  // namespace

TEST(Proto, DeviceInfoRoundTrips) {
  const micnote_v1_DeviceInfo sent = SampleDeviceInfo();

  std::array<pb_byte_t, micnote_v1_DeviceInfo_size> buf{};
  pb_ostream_t out = pb_ostream_from_buffer(buf.data(), buf.size());
  ASSERT_TRUE(pb_encode(&out, micnote_v1_DeviceInfo_fields, &sent)) << PB_GET_ERROR(&out);

  micnote_v1_DeviceInfo got{};
  ASSERT_TRUE(
      Decode({buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(out.bytes_written)}, got));
  EXPECT_TRUE(got.has_fw_version);
  EXPECT_EQ(got.fw_version.major, 1U);
  EXPECT_EQ(got.fw_version.minor, 2U);
  EXPECT_EQ(got.fw_version.patch, 3U);
  EXPECT_EQ(got.proto_version, 1U);
  EXPECT_TRUE(std::ranges::equal(got.device_id, sent.device_id));
  EXPECT_TRUE(std::ranges::equal(got.board, sent.board));
}

// The puck's messages arrive over the air: a field longer than its options
// max_size must fail to decode, never overflow the fixed buffer.
TEST(Proto, RejectsOversizedBoard) {
  constexpr std::uint8_t kBoardTag = (4U << 3U) | 2U;  // field 4, length-delimited
  std::vector<pb_byte_t> bytes{kBoardTag, 40};
  bytes.insert(bytes.end(), 40, 'a');
  micnote_v1_DeviceInfo got{};
  EXPECT_FALSE(Decode(bytes, got));
}

TEST(Proto, RejectsWrongLengthDeviceId) {
  constexpr std::uint8_t kIdTag = (3U << 3U) | 2U;  // field 3, length-delimited
  const std::vector<pb_byte_t> bytes{kIdTag, 4, 1, 2, 3, 4};
  micnote_v1_DeviceInfo got{};
  EXPECT_FALSE(Decode(bytes, got));
}
