#include "encoder.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "voice.h"

/* Same input and settings as tests/opus, whose packet stream has this
 * CRC-32. Pushed in any block size, the encoder must give the same packets. */
#define FRAMES 150
#define SAMPLES (MN_ENC_FRAME_SAMPLES * FRAMES)
#define OPUS_SUITE_CRC 0x21f1f4c2u

static struct mn_enc enc;
static int16_t voice[SAMPLES];

struct collect {
    uint32_t crc;
    uint32_t packets;
    uint32_t fail_at; /* return -ENOSPC for this packet (1-based); 0 never */
};

static int collect_sink(void *ctx, const uint8_t *packet, size_t len)
{
    struct collect *c = ctx;
    const uint8_t len_le[2] = {(uint8_t)len, (uint8_t)(len >> 8)};

    if (++c->packets == c->fail_at) {
        return -ENOSPC;
    }
    zassert_true(len > 0 && len <= MN_ENC_MAX_PACKET);
    c->crc = crc32_ieee_update(c->crc, len_le, sizeof(len_le));
    c->crc = crc32_ieee_update(c->crc, packet, len);
    return 0;
}

static void encode_in_blocks(size_t block, struct collect *c)
{
    memset(c, 0, sizeof(*c));
    zassert_ok(mn_enc_init(&enc, MN_ENC_COMPLEXITY_DEFAULT, collect_sink, c));
    for (size_t i = 0; i < SAMPLES; i += block) {
        zassert_ok(mn_enc_push(&enc, &voice[i], MIN(block, SAMPLES - i)));
    }
    zassert_ok(mn_enc_flush(&enc));
}

static void *setup(void)
{
    mn_voice(voice, SAMPLES);
    return NULL;
}

ZTEST(encoder, test_10ms_blocks_match_opus_suite)
{
    struct collect c;

    encode_in_blocks(MN_ENC_FRAME_SAMPLES / 2, &c);
    zassert_equal(c.packets, FRAMES);
    zassert_equal(enc.packets, FRAMES);
    zassert_equal(c.crc, OPUS_SUITE_CRC, "CRC-32 0x%08x", c.crc);
}

ZTEST(encoder, test_odd_blocks_match_opus_suite)
{
    struct collect c;

    /* 7 doesn't divide 320: frames fill across push boundaries. */
    encode_in_blocks(7, &c);
    zassert_equal(c.packets, FRAMES);
    zassert_equal(c.crc, OPUS_SUITE_CRC, "CRC-32 0x%08x", c.crc);

    encode_in_blocks(SAMPLES, &c);
    zassert_equal(c.crc, OPUS_SUITE_CRC, "CRC-32 0x%08x", c.crc);
}

ZTEST(encoder, test_flush_pads_partial_frame)
{
    struct collect c = {0};

    zassert_ok(mn_enc_init(&enc, MN_ENC_COMPLEXITY_DEFAULT, collect_sink, &c));
    zassert_ok(mn_enc_push(&enc, voice, MN_ENC_FRAME_SAMPLES + 10));
    zassert_equal(c.packets, 1);
    zassert_ok(mn_enc_flush(&enc));
    zassert_equal(c.packets, 2);
    /* Nothing left: a second flush encodes nothing. */
    zassert_ok(mn_enc_flush(&enc));
    zassert_equal(c.packets, 2);
}

ZTEST(encoder, test_flush_on_frame_boundary)
{
    struct collect c = {0};

    zassert_ok(mn_enc_init(&enc, MN_ENC_COMPLEXITY_DEFAULT, collect_sink, &c));
    zassert_ok(mn_enc_flush(&enc));
    zassert_equal(c.packets, 0);
    zassert_ok(mn_enc_push(&enc, voice, 2 * MN_ENC_FRAME_SAMPLES));
    zassert_ok(mn_enc_flush(&enc));
    zassert_equal(c.packets, 2);
}

ZTEST(encoder, test_position)
{
    struct collect c = {0};

    zassert_ok(mn_enc_init(&enc, MN_ENC_COMPLEXITY_DEFAULT, collect_sink, &c));
    zassert_equal(mn_enc_position_ms(&enc), 0);
    zassert_ok(mn_enc_push(&enc, voice, 330)); /* 20.6 ms */
    zassert_equal(mn_enc_position_ms(&enc), 20);
    zassert_ok(mn_enc_push(&enc, voice, 16000 - 330));
    zassert_equal(mn_enc_position_ms(&enc), 1000);
}

ZTEST(encoder, test_sink_error_sticks)
{
    struct collect c = {.fail_at = 3};

    zassert_ok(mn_enc_init(&enc, MN_ENC_COMPLEXITY_DEFAULT, collect_sink, &c));
    zassert_equal(mn_enc_push(&enc, voice, 5 * MN_ENC_FRAME_SAMPLES), -ENOSPC);
    /* It stopped at the failing packet... */
    zassert_equal(c.packets, 3);
    zassert_equal(enc.packets, 2);
    /* ...and stays stopped. */
    zassert_equal(mn_enc_push(&enc, voice, MN_ENC_FRAME_SAMPLES), -ENOSPC);
    zassert_equal(mn_enc_flush(&enc), -ENOSPC);
    zassert_equal(c.packets, 3);
}

ZTEST(encoder, test_init_checks)
{
    struct collect c = {0};

    zassert_equal(mn_enc_init(&enc, -1, collect_sink, &c), -EINVAL);
    zassert_equal(mn_enc_init(&enc, 11, collect_sink, &c), -EINVAL);
    zassert_equal(mn_enc_init(&enc, 3, NULL, &c), -EINVAL);
    for (int cx = 0; cx <= 10; cx++) {
        zassert_ok(mn_enc_init(&enc, cx, collect_sink, &c));
    }
}

ZTEST_SUITE(encoder, NULL, setup, NULL, NULL, NULL);
