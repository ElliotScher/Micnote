/*
 * The recorder end to end on native_sim: a fake capture source (the test
 * voice in 10 ms blocks, released by the test a block count at a time),
 * the real encoder and storage threads, recstore on LittleFS. Each test
 * decrypts the file it produced with the host key and checks the packets,
 * markers and trailer.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <psa/crypto.h>

#include "aead.h"
#include "mnr.h"
#include "recorder.h"
#include "voice.h"
#include "wrap.h"

#define MNT "/lfs"
#define DIR MNT "/rec"
#define MAX_BLOCKS 400 /* 4 s */

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);
static struct fs_mount_t mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &lfs_cfg,
    .storage_dev = (void *)PARTITION_ID(rec_partition),
    .mnt_point = MNT,
};

/* ---- the fake source --------------------------------------------------- */

static int16_t voice[MAX_BLOCKS * MN_AUDIO_BLOCK_FRAMES];

struct fake {
    atomic_t allowed; /* blocks the test lets through so far */
    uint32_t reads;
    uint32_t end_after; /* then end_err; 0 never */
    int end_err;
};

static struct fake fake;

static int fake_read(void *ctx, int16_t **buf)
{
    struct fake *f = ctx;

    if (f->end_after != 0 && f->reads == f->end_after) {
        return f->end_err;
    }
    if (f->reads >= (uint32_t)atomic_get(&f->allowed) || f->reads >= MAX_BLOCKS) {
        k_msleep(1);
        return -EAGAIN;
    }
    *buf = &voice[f->reads++ * MN_AUDIO_BLOCK_FRAMES];
    return 0;
}

static void fake_release(void *ctx, int16_t *buf)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(buf);
}

/* Lets the source run to `blocks` and waits until they are encoded. */
static void feed(uint32_t blocks)
{
    atomic_set(&fake.allowed, (atomic_val_t)blocks);
    for (int i = 0; i < 5000 && mn_audio_position_ms() < blocks * MN_AUDIO_BLOCK_MS; i++) {
        k_msleep(1);
    }
    zassert_equal(mn_audio_position_ms(), blocks * MN_AUDIO_BLOCK_MS);
}

/* ---- the host key and recording params --------------------------------- */

static const uint8_t host_priv[32] = {
    0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
    0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd, 0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb};
static psa_key_id_t host_key;
static struct mn_rec_params params;
static uint64_t next_id = 1;

static const struct mn_rec_params *new_params(void)
{
    params.rec_id = next_id++;
    return &params;
}

/* ---- reading a recording back ------------------------------------------ */

struct readback {
    uint32_t packets;
    uint32_t markers;
    uint32_t marker_ms[16];
    bool trailer;
    uint32_t duration_ms;
};

static uint8_t file_buf[64 * 1024];

static void read_back(uint64_t rec_id, struct readback *out)
{
    static struct mn_mnr_header h;
    static uint8_t pt[MN_MNR_MAX_CT];
    char path[MN_RECSTORE_PATH_MAX];
    struct fs_file_t f;
    uint8_t core_hash[32];
    uint8_t cek[MN_CEK_SIZE];
    psa_key_id_t cek_key;

    memset(out, 0, sizeof(*out));
    snprintf(path, sizeof(path), DIR "/%016llx.mnr", (unsigned long long)rec_id);
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_READ), "%s isn't a finished recording", path);
    const ssize_t size = fs_read(&f, file_buf, sizeof(file_buf));

    fs_close(&f);
    zassert_true(size > 0 && size < (ssize_t)sizeof(file_buf));

    const int hdr_len = mn_mnr_header_parse(file_buf, (size_t)size, &h);

    zassert_true(hdr_len > 0);
    zassert_ok(mn_unwrap_cek(host_key, h.rec_id, &h.wraps[0], cek));
    zassert_ok(mn_cek_import(cek, &cek_key));
    zassert_ok(mn_header_core_hash(file_buf, core_hash));

    for (size_t off = (size_t)hdr_len; off < (size_t)size;) {
        struct mn_mnr_chunk_hdr ch;

        zassert_ok(mn_mnr_chunk_hdr_parse(&file_buf[off], &ch));
        const size_t ct_len = ch.ct_len + MN_MNR_TAG_SIZE;
        const int n = mn_chunk_open(cek_key, h.rec_id, ch.seq, core_hash,
                                    &file_buf[off + MN_MNR_CHUNK_HDR_SIZE], ct_len, pt, sizeof(pt));

        zassert_equal(n, ch.ct_len);
        off += MN_MNR_CHUNK_HDR_SIZE + ct_len;
        if (pt[0] == MN_CHUNK_TRAILER) {
            out->trailer = true;
            out->duration_ms = sys_get_le32(&pt[1]);
            continue;
        }
        for (size_t p = 1; p < (size_t)n;) {
            const uint16_t len = sys_get_le16(&pt[p]);

            if (len == MN_CHUNK_MARKER_SENTINEL) {
                zassert_true(out->markers < ARRAY_SIZE(out->marker_ms));
                out->marker_ms[out->markers++] = sys_get_le32(&pt[p + 3]);
                p += MN_CHUNK_MARKER_SIZE;
                continue;
            }
            out->packets++;
            p += 2 + len;
        }
    }
    psa_destroy_key(cek_key);
}

/* ---- setup ------------------------------------------------------------- */

static void *setup(void)
{
    const struct flash_area *fa;
    uint8_t hash[32];
    size_t len;

    zassert_equal(psa_crypto_init(), PSA_SUCCESS);
    zassert_ok(mn_x25519_import(host_priv, &host_key));
    zassert_equal(psa_export_public_key(host_key, params.recipients[0].pub, 32, &len), PSA_SUCCESS);
    zassert_equal(
        psa_hash_compute(PSA_ALG_SHA_256, params.recipients[0].pub, 32, hash, sizeof(hash), &len),
        PSA_SUCCESS);
    memcpy(params.recipients[0].host_id, hash, 8);
    params.n_recipients = 1;
    params.frame_ms = MN_ENC_FRAME_MS;

    zassert_ok(flash_area_open(PARTITION_ID(rec_partition), &fa));
    zassert_ok(flash_area_flatten(fa, 0, fa->fa_size));
    flash_area_close(fa);
    zassert_ok(fs_mount(&mnt));
    zassert_ok(mn_recstore_init(DIR));

    mn_voice(voice, ARRAY_SIZE(voice));
    const struct mn_recorder_config cfg = {
        .dir = DIR,
        .source = {.read = fake_read, .release = fake_release, .ctx = &fake, .channels = 1},
        .gain_q8 = 256,
        .complexity = MN_ENC_COMPLEXITY_DEFAULT,
    };

    zassert_ok(mn_recorder_init(&cfg));
    return NULL;
}

static void before(void *f)
{
    ARG_UNUSED(f);
    if (mn_recorder_active()) {
        (void)mn_recorder_stop();
    }
    memset(&fake, 0, sizeof(fake));
}

/* Polls until the recording ends by itself. */
static enum mn_recorder_end poll_until_end(int *err)
{
    enum mn_recorder_end why = MN_REC_END_NONE;

    for (int i = 0; i < 5000 && why == MN_REC_END_NONE; i++) {
        why = mn_recorder_poll(MN_STORAGE_OK, 100, err);
        if (why == MN_REC_END_NONE) {
            k_msleep(1);
        }
    }
    return why;
}

/* ---- the tests --------------------------------------------------------- */

ZTEST(recorder, test_start_marker_stop)
{
    struct readback rb;
    const uint64_t id = next_id;

    zassert_ok(mn_recorder_start(new_params(), MN_STORAGE_OK, 80));
    zassert_true(mn_recorder_active());
    zassert_equal(mn_recorder_rec_id(), id);
    feed(100);
    zassert_ok(mn_recorder_marker(1));
    feed(150);
    zassert_equal(mn_recorder_poll(MN_STORAGE_WARN, 80, NULL), MN_REC_END_NONE);
    zassert_ok(mn_recorder_stop());
    zassert_false(mn_recorder_active());
    zassert_equal(mn_recorder_rec_id(), 0);

    read_back(id, &rb);
    zassert_equal(rb.packets, 75); /* 1.5 s */
    zassert_equal(rb.markers, 1);
    zassert_equal(rb.marker_ms[0], 1000);
    zassert_true(rb.trailer);
    zassert_equal(rb.duration_ms, 1500);
}

ZTEST(recorder, test_markers_stay_in_order)
{
    struct readback rb;
    const uint64_t id = next_id;

    zassert_ok(mn_recorder_start(new_params(), MN_STORAGE_OK, 80));
    for (uint32_t b = 20; b <= 300; b += 20) {
        feed(b);
        zassert_ok(mn_recorder_marker(2));
    }
    zassert_ok(mn_recorder_stop());

    read_back(id, &rb);
    zassert_equal(rb.packets, 150);
    zassert_equal(rb.markers, 15);
    for (uint32_t i = 0; i < rb.markers; i++) {
        zassert_equal(rb.marker_ms[i], (i + 1) * 200);
    }
}

ZTEST(recorder, test_low_battery_finalizes)
{
    struct readback rb;
    const uint64_t id = next_id;
    int err = 1;

    zassert_ok(mn_recorder_start(new_params(), MN_STORAGE_OK, 6));
    feed(50);
    zassert_equal(mn_recorder_poll(MN_STORAGE_OK, 5, &err), MN_REC_END_NONE);
    zassert_equal(err, 0);
    zassert_equal(mn_recorder_poll(MN_STORAGE_OK, 4, &err), MN_REC_END_LOW_BATTERY);
    zassert_equal(err, 0);
    zassert_false(mn_recorder_active());

    /* Finalized like a normal stop: trailer and all. */
    read_back(id, &rb);
    zassert_equal(rb.packets, 25);
    zassert_true(rb.trailer);
    zassert_equal(rb.duration_ms, 500);

    /* And no new recording until it's charged. */
    zassert_equal(mn_recorder_start(new_params(), MN_STORAGE_OK, 4), -EPERM);
    zassert_false(mn_recorder_active());
}

ZTEST(recorder, test_storage_full_finalizes)
{
    struct readback rb;
    const uint64_t id = next_id;

    zassert_ok(mn_recorder_start(new_params(), MN_STORAGE_WARN, 50));
    feed(40);
    zassert_equal(mn_recorder_poll(MN_STORAGE_FULL, 50, NULL), MN_REC_END_STORAGE_FULL);
    read_back(id, &rb);
    zassert_equal(rb.packets, 20);
    zassert_true(rb.trailer);

    zassert_equal(mn_recorder_start(new_params(), MN_STORAGE_FULL, 50), -ENOSPC);
}

ZTEST(recorder, test_capture_fault_finalizes)
{
    struct readback rb;
    const uint64_t id = next_id;
    int err = 0;

    fake.end_after = 30;
    fake.end_err = -EIO;
    atomic_set(&fake.allowed, MAX_BLOCKS);
    zassert_ok(mn_recorder_start(new_params(), MN_STORAGE_OK, 50));
    zassert_equal(poll_until_end(&err), MN_REC_END_FAULT);
    zassert_equal(err, -EIO);
    zassert_false(mn_recorder_active());

    /* What was captured before the fault is kept, and closed properly. */
    read_back(id, &rb);
    zassert_equal(rb.packets, 15);
    zassert_true(rb.trailer);
}

ZTEST(recorder, test_source_that_ends)
{
    struct readback rb;
    const uint64_t id = next_id;
    int err = 1;

    fake.end_after = 21;
    fake.end_err = -ENODATA;
    atomic_set(&fake.allowed, MAX_BLOCKS);
    zassert_ok(mn_recorder_start(new_params(), MN_STORAGE_OK, 50));
    zassert_equal(poll_until_end(&err), MN_REC_END_USER);
    zassert_equal(err, 0);
    read_back(id, &rb);
    zassert_equal(rb.packets, 11); /* 10 frames and the padded half */
    zassert_equal(rb.duration_ms, 220);
}

ZTEST(recorder, test_busy_and_idle)
{
    zassert_equal(mn_recorder_stop(), -EALREADY);
    zassert_equal(mn_recorder_marker(1), -EALREADY);
    zassert_equal(mn_recorder_poll(MN_STORAGE_FULL, 0, NULL), MN_REC_END_NONE);

    zassert_ok(mn_recorder_start(new_params(), MN_STORAGE_OK, 50));
    zassert_equal(mn_recorder_start(new_params(), MN_STORAGE_OK, 50), -EBUSY);
    zassert_ok(mn_recorder_stop());

    /* An ID that's already on flash. */
    params.rec_id = 1;
    zassert_equal(mn_recorder_start(&params, MN_STORAGE_OK, 50), -EEXIST);
    zassert_false(mn_recorder_active());
}

ZTEST_SUITE(recorder, NULL, setup, before, NULL, NULL);
