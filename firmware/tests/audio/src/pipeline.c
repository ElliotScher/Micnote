/*
 * The encoder thread with a fake capture source: stereo 10 ms blocks of the
 * test voice, with -EAGAIN gaps like a DMIC read timing out, then the end of
 * the input or a fault.
 */
#include "pipeline.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "preproc.h"
#include "voice.h"

#define BLOCKS 100 /* 1 s */

static int16_t voice[BLOCKS * MN_AUDIO_BLOCK_FRAMES];
static int16_t block[MN_AUDIO_BLOCK_FRAMES * 2];

struct fake {
    uint32_t reads; /* blocks handed out */
    uint32_t released;
    uint32_t calls;
    uint32_t end_after; /* -ENODATA (or `fault`) once this many were read */
    int fault;          /* 0: -ENODATA at the end; else this error */
    bool forever;       /* never end; -EAGAIN after end_after */
    bool outstanding;
};

static struct fake fake;

static int fake_read(void *ctx, int16_t **buf)
{
    struct fake *f = ctx;

    zassert_false(f->outstanding, "read before the last block was released");
    if (++f->calls % 3 == 0) {
        k_msleep(1);
        return -EAGAIN;
    }
    if (f->reads == f->end_after) {
        if (f->forever) {
            k_msleep(1);
            return -EAGAIN;
        }
        return f->fault ? f->fault : -ENODATA;
    }
    for (size_t i = 0; i < MN_AUDIO_BLOCK_FRAMES; i++) {
        int16_t s = voice[(f->reads % BLOCKS) * MN_AUDIO_BLOCK_FRAMES + i];

        block[2 * i] = s;
        block[2 * i + 1] = s;
    }
    f->reads++;
    f->outstanding = true;
    *buf = block;
    return 0;
}

static void fake_release(void *ctx, int16_t *buf)
{
    struct fake *f = ctx;

    zassert_equal_ptr(buf, block);
    f->outstanding = false;
    f->released++;
}

static const struct mn_audio_source src = {
    .read = fake_read,
    .release = fake_release,
    .ctx = &fake,
    .channels = 2,
};

struct sink {
    uint32_t packets;
    uint32_t bytes;
    uint32_t fail_at;
};

static struct sink sink;

static int count_sink(void *ctx, const uint8_t *packet, size_t len)
{
    struct sink *s = ctx;

    ARG_UNUSED(packet);
    if (++s->packets == s->fail_at) {
        return -ENOSPC;
    }
    s->bytes += len;
    return 0;
}

static const struct mn_audio_config cfg = {
    .gain_q8 = MN_PREPROC_GAIN_UNITY,
    .complexity = MN_ENC_COMPLEXITY_DEFAULT,
    .sink = count_sink,
    .sink_ctx = &sink,
};

static void *setup(void)
{
    mn_voice(voice, ARRAY_SIZE(voice));
    return NULL;
}

static void before(void *f)
{
    ARG_UNUSED(f);
    memset(&fake, 0, sizeof(fake));
    memset(&sink, 0, sizeof(sink));
    /* Collect a thread a failed test left behind. */
    (void)mn_audio_stop();
}

/* Waits for the thread to finish reading, without asking it to stop. */
static void wait_reads(uint32_t n)
{
    for (int i = 0; i < 5000 && (fake.reads < n || fake.outstanding); i++) {
        k_msleep(1);
    }
    zassert_equal(fake.reads, n);
}

ZTEST(pipeline, test_encodes_until_source_ends)
{
    fake.end_after = BLOCKS + 1; /* 1.01 s: the last frame is half full */
    zassert_ok(mn_audio_start(&src, &cfg));
    wait_reads(BLOCKS + 1);
    zassert_ok(mn_audio_stop());

    zassert_equal(fake.released, fake.reads);
    zassert_equal(mn_audio_blocks(), BLOCKS + 1);
    zassert_equal(mn_audio_position_ms(), (BLOCKS + 1) * MN_AUDIO_BLOCK_MS);
    /* 50 whole frames, and the flush pads the half one. */
    zassert_equal(sink.packets, BLOCKS / 2 + 1);
    TC_PRINT("%u packets, %u bytes\n", sink.packets, sink.bytes);
}

/* A stop straight after the start may beat the thread to its first read
 * (it runs at a lower priority than this one); wait lets it run to the end. */
ZTEST(pipeline, test_wait_runs_to_the_end)
{
    fake.end_after = 7;
    zassert_ok(mn_audio_start(&src, &cfg));
    zassert_false(mn_audio_done());
    for (int i = 0; i < 5000 && !mn_audio_done(); i++) {
        k_msleep(1);
    }
    zassert_true(mn_audio_done());
    zassert_ok(mn_audio_wait());
    zassert_false(mn_audio_done());
    zassert_equal(fake.reads, 7);
    zassert_equal(fake.released, 7);
    zassert_equal(sink.packets, 4); /* 3 whole frames and the padded half */
    zassert_equal(mn_audio_wait(), -EALREADY);
}

ZTEST(pipeline, test_stop_while_running)
{
    fake.end_after = 10;
    fake.forever = true;
    zassert_ok(mn_audio_start(&src, &cfg));
    wait_reads(10);
    zassert_ok(mn_audio_stop());
    zassert_equal(fake.released, 10);
    zassert_equal(sink.packets, 5);
}

ZTEST(pipeline, test_source_fault_ends_recording)
{
    fake.end_after = 5;
    fake.fault = -EIO;
    zassert_ok(mn_audio_start(&src, &cfg));
    wait_reads(5);
    zassert_equal(mn_audio_stop(), -EIO);
    zassert_equal(fake.released, 5);
    /* No flush after a fault. */
    zassert_equal(sink.packets, 2);
}

ZTEST(pipeline, test_sink_error_ends_recording)
{
    fake.end_after = BLOCKS;
    sink.fail_at = 3;
    zassert_ok(mn_audio_start(&src, &cfg));
    /* The thread ends by itself at the failing packet. */
    zassert_equal(mn_audio_wait(), -ENOSPC);
    zassert_equal(sink.packets, 3);
    zassert_equal(fake.released, fake.reads);
}

ZTEST(pipeline, test_one_at_a_time)
{
    fake.end_after = 1;
    fake.forever = true;
    zassert_ok(mn_audio_start(&src, &cfg));
    zassert_equal(mn_audio_start(&src, &cfg), -EBUSY);
    zassert_ok(mn_audio_stop());
    zassert_equal(mn_audio_stop(), -EALREADY);
    /* And a new one can start after the stop. */
    memset(&fake, 0, sizeof(fake));
    fake.end_after = 2;
    zassert_ok(mn_audio_start(&src, &cfg));
    wait_reads(2);
    zassert_ok(mn_audio_stop());
}

ZTEST(pipeline, test_bad_arguments)
{
    struct mn_audio_source mono3 = src;

    mono3.channels = 3;
    zassert_equal(mn_audio_start(&mono3, &cfg), -EINVAL);
    zassert_equal(mn_audio_start(NULL, &cfg), -EINVAL);
    zassert_equal(mn_audio_start(&src, NULL), -EINVAL);

    struct mn_audio_config bad = cfg;

    bad.complexity = 42;
    zassert_equal(mn_audio_start(&src, &bad), -EINVAL);
    /* A failed start leaves nothing running. */
    zassert_equal(mn_audio_stop(), -EALREADY);
}

ZTEST_SUITE(pipeline, NULL, setup, before, NULL, NULL);
