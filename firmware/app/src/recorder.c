#include "recorder.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>

/* Ring records: kind u8 | pad u8 | len u16 LE | payload[len]. */
enum { REC_PACKET = 1, REC_MARKER = 2, REC_CLOSE = 3 };
#define REC_HDR 4
#define STORAGE_STACK_SIZE 4096

static struct mn_recorder_config config;
static struct mn_rec rec;

RING_BUF_DECLARE(ring, MN_RECORDER_RING_SIZE);
static struct k_spinlock ring_lock;
static K_SEM_DEFINE(work, 0, K_SEM_MAX_LIMIT);
static K_SEM_DEFINE(closed, 0, 1);

K_THREAD_STACK_DEFINE(storage_stack, STORAGE_STACK_SIZE);
static struct k_thread storage_thread;

static bool active;
static atomic_t storage_err; /* first recstore error of this recording */
static int close_err;

/* ---- the ring ---------------------------------------------------------- */

/* Puts one whole record, or nothing: -ENOBUFS if it doesn't fit. Called by
 * the encoder thread (packets) and the caller's thread (markers, close). */
static int ring_put(uint8_t kind, const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[REC_HDR] = {kind, 0};
    int rc = 0;

    sys_put_le16(len, &hdr[2]);
    k_spinlock_key_t key = k_spin_lock(&ring_lock);

    if (ring_buf_space_get(&ring) < REC_HDR + (uint32_t)len) {
        rc = -ENOBUFS;
    } else {
        ring_buf_put(&ring, hdr, REC_HDR);
        ring_buf_put(&ring, payload, len);
    }
    k_spin_unlock(&ring_lock, key);
    if (rc == 0) {
        k_sem_give(&work);
    }
    return rc;
}

/* Takes the next record into buf (at least MN_ENC_MAX_PACKET bytes). False
 * when the ring is empty. */
static bool ring_get(uint8_t *kind, uint8_t *buf, uint16_t *len)
{
    uint8_t hdr[REC_HDR];
    bool got = false;
    k_spinlock_key_t key = k_spin_lock(&ring_lock);

    if (ring_buf_get(&ring, hdr, REC_HDR) == REC_HDR) {
        *kind = hdr[0];
        *len = sys_get_le16(&hdr[2]);
        ring_buf_get(&ring, buf, *len);
        got = true;
    }
    k_spin_unlock(&ring_lock, key);
    return got;
}

/* ---- the storage thread ------------------------------------------------ */

static void storage_main(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    static uint8_t buf[MN_ENC_MAX_PACKET];

    for (;;) {
        uint8_t kind = 0;
        uint16_t len = 0;

        k_sem_take(&work, K_FOREVER);
        while (ring_get(&kind, buf, &len)) {
            int rc = 0;

            if (kind == REC_CLOSE) {
                close_err = mn_rec_close(&rec);
                k_sem_give(&closed);
                continue;
            }
            if (atomic_get(&storage_err) != 0) {
                continue; /* the recording is failing: drop until the close */
            }
            if (kind == REC_PACKET) {
                rc = mn_rec_append(&rec, buf, len);
            } else {
                rc = mn_rec_marker(&rec, buf[0], sys_get_le32(&buf[1]));
            }
            if (rc < 0) {
                atomic_cas(&storage_err, 0, rc);
            }
        }
    }
}

/* The encoder thread's sink. */
static int ring_sink(void *ctx, const uint8_t *packet, size_t len)
{
    ARG_UNUSED(ctx);
    int err = (int)atomic_get(&storage_err);

    /* A storage error ends the recording through the encoder, which
     * mn_recorder_poll() then sees. */
    return err != 0 ? err : ring_put(REC_PACKET, packet, (uint16_t)len);
}

/* ---- the API ----------------------------------------------------------- */

int mn_recorder_init(const struct mn_recorder_config *cfg)
{
    if (cfg == NULL || cfg->dir == NULL) {
        return -EINVAL;
    }
    config = *cfg;
    k_thread_create(&storage_thread, storage_stack, K_THREAD_STACK_SIZEOF(storage_stack),
                    storage_main, NULL, NULL, NULL, MN_RECORDER_STORAGE_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&storage_thread, "storage");
    return 0;
}

int mn_recorder_start(const struct mn_rec_params *p, enum mn_storage_state storage,
                      uint8_t battery_pct)
{
    static struct mn_rec_secrets secrets;
    const struct mn_audio_config audio = {
        .gain_q8 = config.gain_q8,
        .complexity = config.complexity,
        .sink = ring_sink,
        .sink_ctx = NULL,
    };

    if (active) {
        return -EBUSY;
    }
    if (storage == MN_STORAGE_FULL) {
        return -ENOSPC;
    }
    if (mn_policy_battery_low(battery_pct)) {
        return -EPERM;
    }

    int rc = mn_rec_secrets_random(&secrets);

    if (rc == 0) {
        rc = mn_rec_open(&rec, config.dir, p, &secrets);
    }
    if (rc < 0) {
        return rc;
    }
    atomic_set(&storage_err, 0);
    rc = mn_audio_start(&config.source, &audio);
    if (rc < 0) {
        /* Nothing was recorded; close it so it isn't left as a .part. */
        (void)mn_rec_close(&rec);
        return rc;
    }
    active = true;
    return 0;
}

/* Collects the encoder thread (asking it to stop or not), lets the storage
 * thread write everything before the close, and closes. */
static int finish(bool ask_to_stop)
{
    int audio_err = ask_to_stop ? mn_audio_stop() : mn_audio_wait();

    /* The close has to follow every packet: wait for room if need be. */
    while (ring_put(REC_CLOSE, NULL, 0) == -ENOBUFS) {
        k_msleep(1);
    }
    k_sem_take(&closed, K_FOREVER);
    active = false;

    int storage = (int)atomic_get(&storage_err);

    /* The storage error is the cause when the encoder stopped because of
     * it (it returns the sink's error). */
    if (storage != 0) {
        return storage;
    }
    return audio_err != 0 ? audio_err : close_err;
}

int mn_recorder_stop(void)
{
    if (!active) {
        return -EALREADY;
    }
    return finish(true);
}

int mn_recorder_marker(uint8_t type)
{
    uint8_t m[5] = {type};

    if (!active) {
        return -EALREADY;
    }
    sys_put_le32(mn_audio_position_ms(), &m[1]);
    return ring_put(REC_MARKER, m, sizeof(m));
}

enum mn_recorder_end mn_recorder_poll(enum mn_storage_state storage, uint8_t battery_pct, int *err)
{
    enum mn_recorder_end why = MN_REC_END_NONE;
    int rc = 0;

    if (!active) {
        /* nothing to do */
    } else if (mn_audio_done()) {
        rc = finish(false);
        /* Ended with no error: the source ran out (tests, the simulator). */
        why = rc != 0 ? MN_REC_END_FAULT : MN_REC_END_USER;
    } else if (mn_policy_battery_low(battery_pct)) {
        rc = finish(true);
        why = MN_REC_END_LOW_BATTERY;
    } else if (storage == MN_STORAGE_FULL) {
        rc = finish(true);
        why = MN_REC_END_STORAGE_FULL;
    }
    if (err != NULL) {
        *err = rc;
    }
    return why;
}

bool mn_recorder_active(void)
{
    return active;
}

uint64_t mn_recorder_rec_id(void)
{
    return active ? rec.rec_id : 0;
}
