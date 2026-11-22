/*
 * Link framing: round trips, frames split and merged across transport
 * packets, and every way a hostile byte stream is rejected. Mirrors
 * core/tests/framing_test.cpp.
 *
 * Frames and framers are static: each is over 4 KB, more than a ztest
 * thread's stack.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "framing.h"

static struct mn_framer framer;
static micnote_v1_Frame frame;
static micnote_v1_Frame sent;
static uint8_t wire[3 * MN_FRAME_MAX_WIRE];

static void before_each(void *fixture)
{
    ARG_UNUSED(fixture);
    mn_framer_init(&framer);
    memset(&frame, 0, sizeof(frame));
    memset(&sent, 0, sizeof(sent));
}

ZTEST_SUITE(framing, NULL, NULL, before_each, NULL, NULL);

static void set_time_frame(micnote_v1_Frame *f, uint32_t req_id, uint64_t unix_ms)
{
    memset(f, 0, sizeof(*f));
    f->req_id = req_id;
    f->which_body = micnote_v1_Frame_set_time_tag;
    f->body.set_time.unix_ms = unix_ms;
    f->body.set_time.tz_offset_min = -300;
}

/* The largest message: a Chunk with a full payload. */
static void full_chunk_frame(micnote_v1_Frame *f)
{
    memset(f, 0, sizeof(*f));
    f->req_id = UINT32_MAX;
    f->which_body = micnote_v1_Frame_chunk_tag;
    f->body.chunk.recording_id = UINT64_MAX;
    f->body.chunk.seq = UINT32_MAX;
    f->body.chunk.payload.size = sizeof(f->body.chunk.payload.bytes);
    for (size_t i = 0; i < f->body.chunk.payload.size; i++) {
        f->body.chunk.payload.bytes[i] = (uint8_t)i;
    }
}

static size_t encode(const micnote_v1_Frame *f, uint8_t *buf, size_t cap)
{
    const int n = mn_frame_encode(f, buf, cap);

    zassert_true(n > 0, "encode failed: %d", n);
    return (size_t)n;
}

static void assert_set_time(const micnote_v1_Frame *f, uint32_t req_id, uint64_t unix_ms)
{
    zassert_equal(f->req_id, req_id);
    zassert_equal(f->which_body, micnote_v1_Frame_set_time_tag);
    zassert_equal(f->body.set_time.unix_ms, unix_ms);
    zassert_equal(f->body.set_time.tz_offset_min, -300);
}

ZTEST(framing, test_round_trip)
{
    set_time_frame(&sent, 7, 1763700000000ULL);
    const size_t n = encode(&sent, wire, sizeof(wire));

    zassert_equal(mn_framer_push(&framer, wire, n), n);
    zassert_equal(mn_framer_next(&framer, &frame), 1);
    assert_set_time(&frame, 7, 1763700000000ULL);
    zassert_equal(framer.len, 0);
}

ZTEST(framing, test_largest_frame_has_two_byte_prefix)
{
    full_chunk_frame(&sent);
    const size_t n = encode(&sent, wire, sizeof(wire));

    zassert_equal(n, MN_FRAME_MAX_WIRE);
    zassert_equal(mn_framer_push(&framer, wire, n), n);
    zassert_equal(mn_framer_next(&framer, &frame), 1);
    zassert_equal(frame.req_id, UINT32_MAX);
    zassert_equal(frame.which_body, micnote_v1_Frame_chunk_tag);
    zassert_equal(frame.body.chunk.recording_id, UINT64_MAX);
    zassert_equal(frame.body.chunk.seq, UINT32_MAX);
    zassert_equal(frame.body.chunk.payload.size, sent.body.chunk.payload.size);
    zassert_mem_equal(frame.body.chunk.payload.bytes, sent.body.chunk.payload.bytes,
                      sent.body.chunk.payload.size);
}

/* A zero-length frame is an empty Frame; rejecting it is the dispatcher's
 * job (Error{BAD_REQUEST}), not the framing's. */
ZTEST(framing, test_empty_frame_is_valid)
{
    const uint8_t bytes[] = {0x00};

    mn_framer_push(&framer, bytes, sizeof(bytes));
    zassert_equal(mn_framer_next(&framer, &frame), 1);
    zassert_equal(frame.which_body, 0);
}

ZTEST(framing, test_byte_at_a_time)
{
    full_chunk_frame(&sent);
    const size_t n = encode(&sent, wire, sizeof(wire));

    for (size_t i = 0; i + 1 < n; i++) {
        zassert_equal(mn_framer_push(&framer, &wire[i], 1), 1);
        zassert_equal(mn_framer_next(&framer, &frame), 0, "completed early at byte %zu", i);
    }
    mn_framer_push(&framer, &wire[n - 1], 1);
    zassert_equal(mn_framer_next(&framer, &frame), 1);
}

ZTEST(framing, test_several_frames_in_one_push)
{
    size_t n = 0;

    for (uint32_t id = 1; id <= 3; id++) {
        set_time_frame(&sent, id, id * 1000ULL);
        n += encode(&sent, &wire[n], sizeof(wire) - n);
    }
    /* Half of a fourth frame, which must stay buffered. */
    uint8_t fourth[MN_FRAME_MAX_WIRE];

    set_time_frame(&sent, 4, 4000);
    const size_t fourth_len = encode(&sent, fourth, sizeof(fourth));

    memcpy(&wire[n], fourth, 3);
    n += 3;

    mn_framer_push(&framer, wire, n);
    for (uint32_t id = 1; id <= 3; id++) {
        zassert_equal(mn_framer_next(&framer, &frame), 1);
        assert_set_time(&frame, id, id * 1000ULL);
    }
    zassert_equal(mn_framer_next(&framer, &frame), 0);
    zassert_equal(framer.len, 3);

    mn_framer_push(&framer, &fourth[3], fourth_len - 3);
    zassert_equal(mn_framer_next(&framer, &frame), 1);
    assert_set_time(&frame, 4, 4000);
}

ZTEST(framing, test_needs_more_bytes)
{
    zassert_equal(mn_framer_next(&framer, &frame), 0);

    const uint8_t half_prefix[] = {0x80};

    mn_framer_push(&framer, half_prefix, sizeof(half_prefix));
    zassert_equal(mn_framer_next(&framer, &frame), 0);
}

/* Firmware only: the buffer is fixed, so a push past one largest frame takes
 * what fits, and the caller pushes the rest after draining. */
ZTEST(framing, test_push_stops_when_full)
{
    full_chunk_frame(&sent);
    const size_t one = encode(&sent, wire, sizeof(wire));
    const size_t two = one + encode(&sent, &wire[one], sizeof(wire) - one);

    const size_t took = mn_framer_push(&framer, wire, two);

    zassert_equal(took, MN_FRAME_MAX_WIRE);
    zassert_equal(mn_framer_next(&framer, &frame), 1);
    zassert_equal(mn_framer_push(&framer, &wire[took], two - took), two - took);
    zassert_equal(mn_framer_next(&framer, &frame), 1);
    zassert_equal(framer.len, 0);
}

ZTEST(framing, test_rejects_overlong_prefix)
{
    /* A 3-byte varint, even one whose value would be small. */
    const uint8_t bytes[] = {0x80, 0x80, 0x00};

    mn_framer_push(&framer, bytes, sizeof(bytes));
    zassert_equal(mn_framer_next(&framer, &frame), -EPROTO);
}

ZTEST(framing, test_rejects_oversized_length)
{
    /* 0xFF 0x7F = 16383, past micnote_v1_Frame_size but within two bytes. */
    const uint8_t bytes[] = {0xFF, 0x7F};

    mn_framer_push(&framer, bytes, sizeof(bytes));
    zassert_equal(mn_framer_next(&framer, &frame), -EMSGSIZE);
    zassert_equal(framer.len, 0);
}

ZTEST(framing, test_rejects_garbage_body)
{
    /* Length 2: field 1 (req_id) as a varint, cut off mid-varint. */
    const uint8_t bytes[] = {0x02, 0x08, 0x80};

    mn_framer_push(&framer, bytes, sizeof(bytes));
    zassert_equal(mn_framer_next(&framer, &frame), -EBADMSG);
}

ZTEST(framing, test_errors_are_sticky)
{
    const uint8_t bad[] = {0xFF, 0x7F};

    mn_framer_push(&framer, bad, sizeof(bad));
    zassert_equal(mn_framer_next(&framer, &frame), -EMSGSIZE);

    /* A valid frame afterwards is not decoded: the stream has lost sync. */
    set_time_frame(&sent, 1, 1);
    const size_t n = encode(&sent, wire, sizeof(wire));

    mn_framer_push(&framer, wire, n);
    zassert_equal(mn_framer_next(&framer, &frame), -EMSGSIZE);
}

ZTEST(framing, test_encode_rejects_unterminated_string)
{
    sent.which_body = micnote_v1_Frame_error_tag;
    sent.body.error.code = micnote_v1_ErrorCode_ERROR_CODE_BUSY;
    memset(sent.body.error.detail, 'x', sizeof(sent.body.error.detail)); /* no NUL */
    zassert_equal(mn_frame_encode(&sent, wire, sizeof(wire)), -EINVAL);
}

ZTEST(framing, test_encode_rejects_small_buffer)
{
    set_time_frame(&sent, 1, 1);
    uint8_t small[4];

    zassert_equal(mn_frame_encode(&sent, small, sizeof(small)), -EINVAL);
}
