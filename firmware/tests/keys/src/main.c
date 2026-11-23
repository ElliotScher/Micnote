/*
 * The device fallback key and the first-pairing re-wrap, on native_sim:
 * PSA persistent keys in Zephyr's secure storage, recordings in LittleFS.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <psa/crypto.h>

#include "aead.h"
#include "keys.h"
#include "mnr.h"
#include "recstore.h"
#include "wrap.h"

#define MNT "/lfs"
#define DIR MNT "/rec"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);
static struct fs_mount_t mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &lfs_cfg,
    .storage_dev = (void *)PARTITION_ID(rec_partition),
    .mnt_point = MNT,
};

/* The host the recordings move to: RFC 7748 section 6.1's Bob, as in the
 * test vectors. */
static const uint8_t host_priv[32] = {
    0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
    0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd, 0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb};
static psa_key_id_t host_key;
static struct mn_rec_recipient host;
static struct mn_fallback_key fallback;

static struct mn_rec rec;
static uint8_t file_buf[16 * 1024];

static void restart_psa(void)
{
    /* What a reboot does to PSA: volatile keys are gone, persistent ones
     * are loaded again from storage on first use. */
    mbedtls_psa_crypto_free();
    zassert_equal(psa_crypto_init(), PSA_SUCCESS);
    zassert_ok(mn_x25519_import(host_priv, &host_key));
}

/* A short closed recording wrapped to `to` (and nobody else). */
static void record(uint64_t rec_id, const struct mn_rec_recipient *to)
{
    static struct mn_rec_params p;
    static struct mn_rec_secrets s;
    const uint8_t packet[30] = {0x48};

    memset(&p, 0, sizeof(p));
    p.rec_id = rec_id;
    p.frame_ms = 20;
    p.n_recipients = 1;
    p.recipients[0] = *to;
    zassert_ok(mn_rec_secrets_random(&s));
    zassert_ok(mn_rec_open(&rec, DIR, &p, &s));
    for (int i = 0; i < 200; i++) {
        zassert_ok(mn_rec_append(&rec, packet, sizeof(packet)));
    }
    zassert_ok(mn_rec_close(&rec));
}

static size_t read_rec(uint64_t rec_id)
{
    char path[MN_RECSTORE_PATH_MAX];
    struct fs_file_t f;
    static const char hex[] = "0123456789abcdef";

    strcpy(path, DIR "/");
    for (int i = 0; i < 16; i++) {
        path[strlen(DIR) + 1 + i] = hex[(rec_id >> (60 - 4 * i)) & 0xF];
    }
    strcpy(&path[strlen(DIR) + 17], ".mnr");
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_READ), "%s", path);
    const ssize_t n = fs_read(&f, file_buf, sizeof(file_buf));

    fs_close(&f);
    zassert_true(n > 0 && n < (ssize_t)sizeof(file_buf));
    return (size_t)n;
}

/* Opens the recording's wrap for host_id with key, then its first chunk.
 * Returns 0, or the error from the wrap (-ENOENT if there is none). */
static int open_as(uint64_t rec_id, psa_key_id_t key, const uint8_t host_id[8])
{
    static struct mn_mnr_header h;
    static uint8_t pt[MN_MNR_MAX_CT];
    uint8_t cek[MN_CEK_SIZE];
    uint8_t core_hash[32];
    psa_key_id_t cek_key;
    struct mn_mnr_chunk_hdr ch;
    const size_t size = read_rec(rec_id);
    const int hdr_len = mn_mnr_header_parse(file_buf, size, &h);

    zassert_true(hdr_len > 0);
    for (int i = 0; i < h.n_wraps; i++) {
        if (memcmp(h.wraps[i].host_id, host_id, 8) != 0) {
            continue;
        }
        const int ret = mn_unwrap_cek(key, rec_id, &h.wraps[i], cek);

        if (ret < 0) {
            return ret;
        }
        zassert_ok(mn_cek_import(cek, &cek_key));
        zassert_ok(mn_header_core_hash(file_buf, core_hash));
        zassert_ok(mn_mnr_chunk_hdr_parse(&file_buf[hdr_len], &ch));
        zassert_equal(mn_chunk_open(cek_key, rec_id, ch.seq, core_hash,
                                    &file_buf[hdr_len + MN_MNR_CHUNK_HDR_SIZE],
                                    ch.ct_len + MN_MNR_TAG_SIZE, pt, sizeof(pt)),
                      ch.ct_len, "the chunks no longer open");
        psa_destroy_key(cek_key);
        return 0;
    }
    return -ENOENT;
}

static void *setup(void)
{
    const struct flash_area *fa;
    uint8_t hash[32];
    size_t len;

    /* Fresh storage for the keys (settings) and the recordings. */
    zassert_ok(flash_area_open(PARTITION_ID(storage_partition), &fa));
    zassert_ok(flash_area_flatten(fa, 0, fa->fa_size));
    flash_area_close(fa);
    zassert_ok(flash_area_open(PARTITION_ID(rec_partition), &fa));
    zassert_ok(flash_area_flatten(fa, 0, fa->fa_size));
    flash_area_close(fa);
    zassert_ok(settings_subsys_init());
    zassert_ok(fs_mount(&mnt));
    zassert_ok(mn_recstore_init(DIR));

    zassert_equal(psa_crypto_init(), PSA_SUCCESS);
    zassert_ok(mn_x25519_import(host_priv, &host_key));
    zassert_equal(psa_export_public_key(host_key, host.pub, 32, &len), PSA_SUCCESS);
    zassert_equal(psa_hash_compute(PSA_ALG_SHA_256, host.pub, 32, hash, 32, &len), PSA_SUCCESS);
    memcpy(host.host_id, hash, 8);
    return NULL;
}

ZTEST_SUITE(keys, NULL, setup, NULL, NULL, NULL);

/* The tests share the key store and run in name order, so each one leaves
 * a fallback key in place. */

ZTEST(keys, test_1_generated_once_then_kept)
{
    struct mn_fallback_key again;

    zassert_equal(mn_keys_fallback(&fallback), 1, "first boot generates it");
    zassert_equal(fallback.id, MN_KEY_ID_FALLBACK);
    zassert_equal(mn_keys_fallback(&again), 0);
    zassert_mem_equal(again.pub, fallback.pub, 32);

    /* After a "reboot" it's loaded, not made again. */
    restart_psa();
    zassert_equal(mn_keys_fallback(&again), 0);
    zassert_mem_equal(again.pub, fallback.pub, 32);
    zassert_mem_equal(again.host_id, fallback.host_id, 8);
}

ZTEST(keys, test_2_private_half_stays_inside)
{
    uint8_t out[32];
    size_t len = 0;

    zassert_equal(mn_keys_fallback(&fallback), 0);
    zassert_not_equal(psa_export_key(fallback.id, out, sizeof(out), &len), PSA_SUCCESS);
}

ZTEST(keys, test_3_destroy_makes_a_new_one)
{
    struct mn_fallback_key old;

    zassert_equal(mn_keys_fallback(&old), 0);
    zassert_ok(mn_keys_fallback_destroy());
    zassert_ok(mn_keys_fallback_destroy(), "destroying nothing is fine");
    restart_psa();
    zassert_equal(mn_keys_fallback(&fallback), 1);
    zassert_true(memcmp(old.pub, fallback.pub, 32) != 0);
}

ZTEST(keys, test_4_rewrap_moves_a_recording_to_the_host)
{
    struct mn_rec_recipient fb;
    uint8_t eph[32];

    zassert_equal(mn_keys_fallback(&fallback), 0);
    memcpy(fb.pub, fallback.pub, 32);
    memcpy(fb.host_id, fallback.host_id, 8);
    record(0x100000001ULL, &fb);
    zassert_ok(open_as(0x100000001ULL, fallback.id, fallback.host_id));
    zassert_equal(open_as(0x100000001ULL, host_key, host.host_id), -ENOENT);

    memset(eph, 0x77, sizeof(eph));
    zassert_ok(mn_recstore_rewrap(DIR, 0x100000001ULL, fallback.id, fallback.host_id, &host, eph));

    /* The host can read it now, with the chunks as they were; the fallback
     * wrap is gone. */
    zassert_ok(open_as(0x100000001ULL, host_key, host.host_id));
    zassert_equal(open_as(0x100000001ULL, fallback.id, fallback.host_id), -ENOENT);
    zassert_equal(
        mn_recstore_rewrap(DIR, 0x100000001ULL, fallback.id, fallback.host_id, &host, eph),
        -EALREADY);
}

ZTEST(keys, test_5_rewrap_all)
{
    struct mn_rec_recipient fb;
    struct mn_rec_recipient other = host;
    uint8_t eph[32] = {1};

    memcpy(fb.pub, fallback.pub, 32);
    memcpy(fb.host_id, fallback.host_id, 8);
    /* Two more on the fallback key, one already for the host, and one still
     * being written. */
    record(0x100000002ULL, &fb);
    record(0x100000003ULL, &fb);
    record(0x100000004ULL, &other);
    static struct mn_rec_params p;
    static struct mn_rec_secrets s;

    memset(&p, 0, sizeof(p));
    p.rec_id = 0x100000005ULL;
    p.frame_ms = 20;
    p.n_recipients = 1;
    p.recipients[0] = fb;
    zassert_ok(mn_rec_secrets_random(&s));
    zassert_ok(mn_rec_open(&rec, DIR, &p, &s));

    zassert_equal(mn_recstore_rewrap_all(DIR, fallback.id, fallback.host_id, &host), 2);
    for (uint64_t id = 0x100000001ULL; id <= 0x100000004ULL; id++) {
        zassert_ok(open_as(id, host_key, host.host_id), "%llx", (unsigned long long)id);
    }
    zassert_equal(mn_recstore_rewrap_all(DIR, fallback.id, fallback.host_id, &host), 0);
    zassert_equal(
        mn_recstore_rewrap(DIR, 0x100000005ULL, fallback.id, fallback.host_id, &host, eph), -EBUSY);
    zassert_equal(mn_recstore_rewrap(DIR, 0x1ULL, fallback.id, fallback.host_id, &host, eph),
                  -ENOENT);
    zassert_ok(mn_rec_close(&rec));
}

ZTEST(keys, test_6_rewrap_with_the_wrong_key)
{
    struct mn_rec_recipient fb;
    uint8_t eph[32] = {2};

    memcpy(fb.pub, fallback.pub, 32);
    memcpy(fb.host_id, fallback.host_id, 8);
    record(0x100000006ULL, &fb);
    /* The right host_id but another private key: nothing is written. */
    zassert_equal(mn_recstore_rewrap(DIR, 0x100000006ULL, host_key, fallback.host_id, &host, eph),
                  -EBADMSG);
    zassert_ok(open_as(0x100000006ULL, fallback.id, fallback.host_id));
}
