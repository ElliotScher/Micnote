/*
 * The .mnr format: the exact byte layout (the host's RecordingReader must
 * agree with it byte for byte), round trips, and malformed input.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "mnr.h"

static struct mn_mnr_header hdr;
static struct mn_mnr_header parsed;
static uint8_t buf[MN_MNR_HEADER_MAX + 16];

static void sample_header(struct mn_mnr_header *h, uint8_t n_wraps)
{
    memset(h, 0, sizeof(*h));
    h->flags = MN_MNR_FLAG_STEREO;
    h->rec_id = 0x0000000700000003ULL; /* boot 7, recording 3 */
    h->codec = MN_MNR_CODEC_OPUS;
    h->rate_hz = 16000;
    h->frame_ms = 20;
    h->start_boot = 7;
    h->start_uptime_ms = 123456789ULL;
    h->start_unix_ms = 1763800000000ULL;
    h->n_wraps = n_wraps;
    for (uint8_t i = 0; i < n_wraps; i++) {
        memset(h->wraps[i].host_id, 0x10 + i, sizeof(h->wraps[i].host_id));
        memset(h->wraps[i].eph_pub, 0x20 + i, sizeof(h->wraps[i].eph_pub));
        memset(h->wraps[i].wrapped_cek, 0x30 + i, sizeof(h->wraps[i].wrapped_cek));
        memset(h->wraps[i].tag, 0x40 + i, sizeof(h->wraps[i].tag));
    }
}

static void before_each(void *fixture)
{
    ARG_UNUSED(fixture);
    memset(buf, 0xEE, sizeof(buf));
    memset(&parsed, 0xEE, sizeof(parsed));
}

ZTEST_SUITE(mnr, NULL, NULL, before_each, NULL, NULL);

ZTEST(mnr, test_header_size_is_constant)
{
    zassert_equal(MN_MNR_HEADER_SIZE, 393);
    for (uint8_t n = 0; n <= MN_MNR_MAX_WRAPS; n++) {
        sample_header(&hdr, n);
        zassert_equal(mn_mnr_header_write(&hdr, buf, sizeof(buf)), MN_MNR_HEADER_SIZE, "n_wraps %u",
                      n);
    }
}

/* Pins every offset in docs/protocol.md section 6. */
ZTEST(mnr, test_golden_layout)
{
    sample_header(&hdr, 2);
    zassert_equal(mn_mnr_header_write(&hdr, buf, sizeof(buf)), MN_MNR_HEADER_SIZE);

    zassert_mem_equal(&buf[0], "MNR1", 4);
    zassert_equal(buf[4], 1, "version");
    zassert_equal(sys_get_le16(&buf[5]), 393, "hdr_len");
    zassert_equal(buf[7], MN_MNR_FLAG_STEREO, "flags");
    /* rec_id, little-endian: low byte first. */
    static const uint8_t rec_id[8] = {0x03, 0, 0, 0, 0x07, 0, 0, 0};

    zassert_mem_equal(&buf[8], rec_id, sizeof(rec_id));
    zassert_equal(buf[16], MN_MNR_CODEC_OPUS, "codec");
    zassert_equal(sys_get_le16(&buf[17]), 16000, "rate_hz");
    zassert_equal(buf[19], 20, "frame_ms");
    zassert_equal(sys_get_le32(&buf[20]), 7, "start_boot");
    zassert_equal(sys_get_le64(&buf[24]), 123456789ULL, "start_uptime_ms");
    zassert_equal(sys_get_le64(&buf[32]), 1763800000000ULL, "start_unix_ms");
    zassert_equal(buf[40], 2, "n_wraps");

    /* Wrap 1 starts at 41 + 88: host_id, eph_pub, wrapped_cek, tag. */
    const uint8_t *w1 = &buf[41 + 88];

    zassert_equal(w1[0], 0x11);
    zassert_equal(w1[8], 0x21);
    zassert_equal(w1[40], 0x31);
    zassert_equal(w1[72], 0x41);
    zassert_equal(w1[87], 0x41);

    /* Unused slots 2 and 3 are zero, and nothing past the header is written. */
    for (size_t i = 41 + 2 * 88; i < MN_MNR_HEADER_SIZE; i++) {
        zassert_equal(buf[i], 0, "byte %zu", i);
    }
    zassert_equal(buf[MN_MNR_HEADER_SIZE], 0xEE);
}

ZTEST(mnr, test_round_trip_every_wrap_count)
{
    for (uint8_t n = 0; n <= MN_MNR_MAX_WRAPS; n++) {
        sample_header(&hdr, n);
        mn_mnr_header_write(&hdr, buf, sizeof(buf));
        zassert_equal(mn_mnr_header_parse(buf, MN_MNR_HEADER_SIZE, &parsed), MN_MNR_HEADER_SIZE);
        /* sample_header zeroes the whole struct, so it compares as bytes. */
        zassert_mem_equal(&parsed, &hdr, sizeof(hdr), "n_wraps %u", n);
    }
}

ZTEST(mnr, test_write_rejects_too_many_wraps)
{
    sample_header(&hdr, MN_MNR_MAX_WRAPS);
    hdr.n_wraps = MN_MNR_MAX_WRAPS + 1;
    zassert_equal(mn_mnr_header_write(&hdr, buf, sizeof(buf)), -EINVAL);
}

ZTEST(mnr, test_write_rejects_small_buffer)
{
    sample_header(&hdr, 1);
    zassert_equal(mn_mnr_header_write(&hdr, buf, MN_MNR_HEADER_SIZE - 1), -ENOSPC);
    zassert_equal(buf[0], 0xEE, "nothing written");
}

ZTEST(mnr, test_parse_rejects_short_input)
{
    sample_header(&hdr, 1);
    mn_mnr_header_write(&hdr, buf, sizeof(buf));
    zassert_equal(mn_mnr_header_parse(buf, 6, &parsed), -ENODATA, "no hdr_len yet");
    zassert_equal(mn_mnr_header_parse(buf, MN_MNR_HEADER_SIZE - 1, &parsed), -ENODATA);
}

ZTEST(mnr, test_parse_rejects_bad_magic)
{
    sample_header(&hdr, 1);
    mn_mnr_header_write(&hdr, buf, sizeof(buf));
    buf[3] = '2';
    zassert_equal(mn_mnr_header_parse(buf, MN_MNR_HEADER_SIZE, &parsed), -EBADMSG);
}

ZTEST(mnr, test_parse_rejects_other_version)
{
    sample_header(&hdr, 1);
    mn_mnr_header_write(&hdr, buf, sizeof(buf));
    buf[4] = 2;
    zassert_equal(mn_mnr_header_parse(buf, MN_MNR_HEADER_SIZE, &parsed), -ENOTSUP);
}

ZTEST(mnr, test_parse_rejects_bad_hdr_len)
{
    sample_header(&hdr, 1);
    mn_mnr_header_write(&hdr, buf, sizeof(buf));

    sys_put_le16(MN_MNR_HEADER_SIZE - 1, &buf[5]);
    zassert_equal(mn_mnr_header_parse(buf, sizeof(buf), &parsed), -EBADMSG, "too small");
    sys_put_le16(MN_MNR_HEADER_MAX + 1, &buf[5]);
    zassert_equal(mn_mnr_header_parse(buf, sizeof(buf), &parsed), -EBADMSG, "too large");
}

/* A later v1 writer may append fields after the wrap table: a reader skips
 * them, and the first chunk starts at hdr_len. */
ZTEST(mnr, test_parse_skips_appended_fields)
{
    sample_header(&hdr, 1);
    mn_mnr_header_write(&hdr, buf, sizeof(buf));
    sys_put_le16(MN_MNR_HEADER_MAX, &buf[5]);
    zassert_equal(mn_mnr_header_parse(buf, MN_MNR_HEADER_MAX, &parsed), MN_MNR_HEADER_MAX);
    zassert_equal(parsed.rec_id, hdr.rec_id);
    zassert_equal(mn_mnr_header_parse(buf, MN_MNR_HEADER_MAX - 1, &parsed), -ENODATA);
}

ZTEST(mnr, test_parse_rejects_too_many_wraps)
{
    sample_header(&hdr, 1);
    mn_mnr_header_write(&hdr, buf, sizeof(buf));
    buf[40] = MN_MNR_MAX_WRAPS + 1;
    zassert_equal(mn_mnr_header_parse(buf, MN_MNR_HEADER_SIZE, &parsed), -EBADMSG);
}

ZTEST(mnr, test_chunk_hdr_round_trip)
{
    const struct mn_mnr_chunk_hdr in = {.seq = 0x01020304, .ct_len = MN_MNR_MAX_CT};
    uint8_t bytes[MN_MNR_CHUNK_HDR_SIZE];
    struct mn_mnr_chunk_hdr out;

    mn_mnr_chunk_hdr_write(&in, bytes);
    static const uint8_t golden[] = {0x04, 0x03, 0x02, 0x01, 0x00, 0x10};

    zassert_mem_equal(bytes, golden, sizeof(golden));
    zassert_equal(mn_mnr_chunk_hdr_parse(bytes, &out), 0);
    zassert_equal(out.seq, in.seq);
    zassert_equal(out.ct_len, in.ct_len);
}

ZTEST(mnr, test_chunk_hdr_rejects_bad_length)
{
    struct mn_mnr_chunk_hdr out;
    uint8_t bytes[MN_MNR_CHUNK_HDR_SIZE] = {0};

    zassert_equal(mn_mnr_chunk_hdr_parse(bytes, &out), -EBADMSG, "ct_len 0");
    sys_put_le16(MN_MNR_MAX_CT + 1, &bytes[4]);
    zassert_equal(mn_mnr_chunk_hdr_parse(bytes, &out), -EBADMSG, "ct_len 4097");
    sys_put_le16(1, &bytes[4]);
    zassert_equal(mn_mnr_chunk_hdr_parse(bytes, &out), 0, "ct_len 1");
}
