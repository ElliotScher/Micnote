#include <string.h>

#include <zephyr/ztest.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "micnote.pb.h"

ZTEST_SUITE(proto, NULL, NULL, NULL, NULL, NULL);

ZTEST(proto, test_version_round_trip)
{
    const micnote_v1_Version in = {.major = 1, .minor = 2, .patch = 3};
    uint8_t buf[micnote_v1_Version_size];
    pb_ostream_t out = pb_ostream_from_buffer(buf, sizeof(buf));

    zassert_true(pb_encode(&out, micnote_v1_Version_fields, &in), "encode failed: %s",
                 PB_GET_ERROR(&out));

    micnote_v1_Version decoded = micnote_v1_Version_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf, out.bytes_written);

    zassert_true(pb_decode(&is, micnote_v1_Version_fields, &decoded), "decode failed: %s",
                 PB_GET_ERROR(&is));
    zassert_equal(decoded.major, 1);
    zassert_equal(decoded.minor, 2);
    zassert_equal(decoded.patch, 3);
}

// Exercises every field kind DeviceInfo uses: a nested message, fixed-length
// bytes and a bounded string, at their maximum sizes from micnote.options.
ZTEST(proto, test_device_info_round_trip_at_max_size)
{
    micnote_v1_DeviceInfo in = micnote_v1_DeviceInfo_init_zero;

    in.has_fw_version = true;
    in.fw_version = (micnote_v1_Version){.major = UINT32_MAX, .minor = UINT32_MAX,
                                         .patch = UINT32_MAX};
    in.proto_version = UINT32_MAX;
    memset(in.device_id, 0xA5, sizeof(in.device_id));
    memset(in.board, 'x', sizeof(in.board) - 1);

    uint8_t buf[micnote_v1_DeviceInfo_size];
    pb_ostream_t out = pb_ostream_from_buffer(buf, sizeof(buf));

    zassert_true(pb_encode(&out, micnote_v1_DeviceInfo_fields, &in), "encode failed: %s",
                 PB_GET_ERROR(&out));
    zassert_equal(out.bytes_written, micnote_v1_DeviceInfo_size,
                  "a maximal message should fill exactly the generated max size");

    micnote_v1_DeviceInfo decoded = micnote_v1_DeviceInfo_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf, out.bytes_written);

    zassert_true(pb_decode(&is, micnote_v1_DeviceInfo_fields, &decoded), "decode failed: %s",
                 PB_GET_ERROR(&is));
    zassert_true(decoded.has_fw_version);
    zassert_equal(decoded.fw_version.patch, UINT32_MAX);
    zassert_equal(decoded.proto_version, UINT32_MAX);
    zassert_mem_equal(decoded.device_id, in.device_id, sizeof(in.device_id));
    zassert_str_equal(decoded.board, in.board);
}

// A board name one byte too long for its max_size must be rejected, not truncated.
ZTEST(proto, test_device_info_rejects_oversized_board)
{
    // Hand-encoded field 4 (board), wire type 2, length 32: one past the 31
    // characters (plus NUL) that DeviceInfo.board can hold.
    uint8_t buf[2 + 32] = {(4 << 3) | 2, 32};

    memset(&buf[2], 'x', 32);

    micnote_v1_DeviceInfo decoded = micnote_v1_DeviceInfo_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf, sizeof(buf));

    zassert_false(pb_decode(&is, micnote_v1_DeviceInfo_fields, &decoded));
}
