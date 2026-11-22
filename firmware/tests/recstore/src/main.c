/*
 * The recording store on LittleFS over native_sim's simulated flash. Every
 * test that writes a recording reads it back the way a host would: unwrap
 * the CEK with the host's private key, then open and parse every chunk.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <psa/crypto.h>

#include "aead.h"
#include "mnr.h"
#include "recstore.h"
#include "wrap.h"

#define MNT "/lfs"
#define DIR MNT "/rec"
#define REC_ID 0x0000000700000003ULL
#define REC_ID2 0x0000000700000004ULL

/* RFC 7748 section 6.1 Bob, as the paired host. */
static const uint8_t host_priv[32] = {
    0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
    0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd, 0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb};
static const uint8_t host_pub[32] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
    0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d, 0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f};
static const uint8_t host_id[8] = {0xf3, 0x5e, 0x56, 0x16, 0x16, 0x0a, 0x30, 0xbf};

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);
static struct fs_mount_t mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &lfs_cfg,
    .storage_dev = (void *)PARTITION_ID(rec_partition),
    .mnt_point = MNT,
};

static struct mn_rec rec;
static struct mn_rec_params params;
static struct mn_rec_secrets secrets;
static psa_key_id_t host_key;

/* Packet i of a recording: 30..49 bytes of a pattern only it has. */
static size_t packet(uint32_t i, uint8_t *out)
{
    const size_t len = 30 + i % 20;

    for (size_t j = 0; j < len; j++) {
        out[j] = (uint8_t)(i * 7 + j);
    }
    return len;
}

static void fill_params(uint64_t rec_id)
{
    memset(&params, 0, sizeof(params));
    params.rec_id = rec_id;
    params.frame_ms = 20;
    params.start_boot = 7;
    params.start_uptime_ms = 1000;
    params.start_unix_ms = 1763800000000ULL;
    params.n_recipients = 1;
    memcpy(params.recipients[0].pub, host_pub, sizeof(host_pub));
    memcpy(params.recipients[0].host_id, host_id, sizeof(host_id));
    for (size_t i = 0; i < sizeof(secrets); i++) {
        ((uint8_t *)&secrets)[i] = (uint8_t)(i * 13 + 1);
    }
}

static void record(uint64_t rec_id, uint32_t n_packets, bool close)
{
    uint8_t buf[64];

    fill_params(rec_id);
    zassert_ok(mn_rec_open(&rec, DIR, &params, &secrets));
    for (uint32_t i = 0; i < n_packets; i++) {
        zassert_ok(mn_rec_append(&rec, buf, packet(i, buf)));
        if (i == 10 || i == 200) {
            zassert_ok(mn_rec_marker(&rec, 1, i * 20));
        }
    }
    if (close) {
        zassert_ok(mn_rec_close(&rec));
    }
}

/* ---- a host-side reader, for checking what was written ----------------- */

struct readback {
    uint32_t chunks;
    uint32_t packets;
    uint32_t markers;
    uint32_t marker_offsets[4];
    bool trailer;
    uint32_t trailer_duration_ms;
    uint32_t trailer_markers;
};

static uint8_t file_buf[64 * 1024];

static size_t read_file(const char *path)
{
    struct fs_file_t f;

    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_READ), "%s", path);
    const ssize_t n = fs_read(&f, file_buf, sizeof(file_buf));

    zassert_true(n > 0 && n < (ssize_t)sizeof(file_buf));
    fs_close(&f);
    return (size_t)n;
}

static void parse_chunk(const uint8_t *pt, size_t len, struct readback *rb)
{
    uint8_t expect[64];

    if (pt[0] == MN_CHUNK_TRAILER) {
        zassert_equal(len, MN_CHUNK_TRAILER_SIZE);
        rb->trailer = true;
        rb->trailer_duration_ms = sys_get_le32(&pt[1]);
        rb->trailer_markers = sys_get_le32(&pt[5]);
        return;
    }
    zassert_equal(pt[0], MN_CHUNK_AUDIO);
    zassert_false(rb->trailer, "audio after the trailer");
    for (size_t off = 1; off < len;) {
        const uint16_t n = sys_get_le16(&pt[off]);

        if (n == MN_CHUNK_MARKER_SENTINEL) {
            zassert_true(rb->markers < ARRAY_SIZE(rb->marker_offsets));
            rb->marker_offsets[rb->markers++] = sys_get_le32(&pt[off + 3]);
            off += MN_CHUNK_MARKER_SIZE;
            continue;
        }
        zassert_equal(n, packet(rb->packets, expect), "packet %u length", rb->packets);
        zassert_mem_equal(&pt[off + 2], expect, n, "packet %u bytes", rb->packets);
        rb->packets++;
        off += 2 + n;
    }
}

/* Decrypts and parses a whole .mnr, as micnote-cli sync will. */
static void read_recording(const char *path, struct readback *rb)
{
    static struct mn_mnr_header h;
    static uint8_t pt[MN_MNR_MAX_CT];
    uint8_t core_hash[32];
    uint8_t cek[MN_CEK_SIZE];
    psa_key_id_t cek_key;

    memset(rb, 0, sizeof(*rb));
    const size_t size = read_file(path);
    const int hdr_len = mn_mnr_header_parse(file_buf, size, &h);

    zassert_equal(hdr_len, MN_MNR_HEADER_SIZE);
    zassert_equal(h.n_wraps, 1);
    zassert_ok(mn_unwrap_cek(host_key, h.rec_id, &h.wraps[0], cek));
    zassert_ok(mn_cek_import(cek, &cek_key));
    zassert_ok(mn_header_core_hash(file_buf, core_hash));

    for (size_t off = (size_t)hdr_len; off < size; rb->chunks++) {
        struct mn_mnr_chunk_hdr ch;

        zassert_true(off + MN_MNR_CHUNK_HDR_SIZE <= size);
        zassert_ok(mn_mnr_chunk_hdr_parse(&file_buf[off], &ch));
        zassert_equal(ch.seq, rb->chunks, "chunk sequence");
        const size_t ct_len = ch.ct_len + MN_MNR_TAG_SIZE;

        zassert_true(off + MN_MNR_CHUNK_HDR_SIZE + ct_len <= size, "truncated chunk");
        const int n = mn_chunk_open(cek_key, h.rec_id, ch.seq, core_hash,
                                    &file_buf[off + MN_MNR_CHUNK_HDR_SIZE], ct_len, pt, sizeof(pt));

        zassert_equal(n, ch.ct_len, "chunk %u: %d", ch.seq, n);
        parse_chunk(pt, (size_t)n, rb);
        off += MN_MNR_CHUNK_HDR_SIZE + ct_len;
    }
    psa_destroy_key(cek_key);
}

static bool exists(const char *path)
{
    struct fs_dirent entry;

    return fs_stat(path, &entry) == 0;
}

#define PART DIR "/0000000700000003.part"
#define DONE DIR "/0000000700000003.mnr"

/* ---- fixtures ---------------------------------------------------------- */

static void *setup(void)
{
    zassert_equal(psa_crypto_init(), PSA_SUCCESS);
    zassert_ok(mn_x25519_import(host_priv, &host_key));
    return NULL;
}

/* Every test starts on freshly erased flash. */
static void before_each(void *fixture)
{
    const struct flash_area *fa;

    ARG_UNUSED(fixture);
    zassert_ok(flash_area_open(PARTITION_ID(rec_partition), &fa));
    zassert_ok(flash_area_flatten(fa, 0, fa->fa_size));
    flash_area_close(fa);
    zassert_ok(fs_mount(&mnt));
    zassert_ok(mn_recstore_init(DIR));
}

static void after_each(void *fixture)
{
    ARG_UNUSED(fixture);
    if (rec.open) {
        fs_close(&rec.file);
        psa_destroy_key(rec.cek);
        rec.open = false;
    }
    fs_unmount(&mnt);
}

ZTEST_SUITE(recstore, NULL, setup, before_each, after_each, NULL);

/* ---- writing ----------------------------------------------------------- */

ZTEST(recstore, test_init_is_idempotent)
{
    zassert_ok(mn_recstore_init(DIR));
}

ZTEST(recstore, test_record_and_read_back)
{
    struct readback rb;

    record(REC_ID, 50, true);
    zassert_false(exists(PART), ".part left behind");
    zassert_true(exists(DONE));

    read_recording(DONE, &rb);
    zassert_equal(rb.packets, 50);
    zassert_equal(rb.markers, 1);
    zassert_equal(rb.marker_offsets[0], 200);
    zassert_true(rb.trailer);
    zassert_equal(rb.trailer_duration_ms, 50 * 20);
    zassert_equal(rb.trailer_markers, 1);
    zassert_equal(rb.chunks, 2, "one audio chunk + the trailer");
}

/* 600 packets of ~40 bytes is ~25 KB: several 4 KB chunks, each sealed and
 * synced on its own. */
ZTEST(recstore, test_long_recording_spans_chunks)
{
    struct readback rb;

    record(REC_ID, 600, true);
    read_recording(DONE, &rb);
    zassert_equal(rb.packets, 600);
    zassert_equal(rb.markers, 2);
    zassert_equal(rb.marker_offsets[1], 4000);
    zassert_true(rb.chunks >= 7, "only %u chunks", rb.chunks);
    zassert_true(rb.trailer);
    zassert_equal(rb.trailer_duration_ms, 600 * 20);
}

ZTEST(recstore, test_empty_recording_has_only_a_trailer)
{
    struct readback rb;

    record(REC_ID, 0, true);
    read_recording(DONE, &rb);
    zassert_equal(rb.chunks, 1);
    zassert_true(rb.trailer);
    zassert_equal(rb.trailer_duration_ms, 0);
}

ZTEST(recstore, test_open_wipes_secrets)
{
    record(REC_ID, 0, false);
    for (size_t i = 0; i < sizeof(secrets); i++) {
        zassert_equal(((uint8_t *)&secrets)[i], 0, "byte %zu", i);
    }
}

ZTEST(recstore, test_never_overwrites_a_recording)
{
    record(REC_ID, 5, false);
    fill_params(REC_ID);
    static struct mn_rec other;

    zassert_equal(mn_rec_open(&other, DIR, &params, &secrets), -EEXIST, "while recording");
    zassert_ok(mn_rec_close(&rec));
    fill_params(REC_ID);
    zassert_equal(mn_rec_open(&other, DIR, &params, &secrets), -EEXIST, "after closing");
}

ZTEST(recstore, test_rejects_bad_input)
{
    uint8_t buf[8] = {0};

    fill_params(REC_ID);
    params.n_recipients = 0;
    zassert_equal(mn_rec_open(&rec, DIR, &params, &secrets), -EINVAL);
    fill_params(REC_ID);
    params.n_recipients = MN_MNR_MAX_WRAPS + 1;
    zassert_equal(mn_rec_open(&rec, DIR, &params, &secrets), -EINVAL);
    zassert_false(exists(PART));

    record(REC_ID, 0, false);
    zassert_equal(mn_rec_append(&rec, buf, 0), -EINVAL, "empty packet");
    zassert_equal(mn_rec_append(&rec, buf, MN_CHUNK_MARKER_SENTINEL), -EINVAL, "sentinel length");
    zassert_ok(mn_rec_close(&rec));

    zassert_equal(mn_rec_append(&rec, buf, 1), -EBADF, "after close");
    zassert_equal(mn_rec_marker(&rec, 1, 0), -EBADF);
    zassert_equal(mn_rec_close(&rec), -EBADF);
}

/* ---- power-cut recovery ------------------------------------------------ */

/* What a power cut leaves: the file as of its last fs_sync, with the CEK
 * gone from RAM. */
static void power_cut(void)
{
    fs_close(&rec.file);
    psa_destroy_key(rec.cek);
    rec.open = false;
}

static void append_raw(const char *path, const uint8_t *bytes, size_t n)
{
    struct fs_file_t f;

    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_WRITE | FS_O_APPEND));
    zassert_equal(fs_write(&f, bytes, n), (ssize_t)n);
    fs_close(&f);
}

ZTEST(recstore, test_recover_keeps_synced_chunks)
{
    struct readback rb;

    /* 600 packets: everything but the chunk still being filled is synced. */
    record(REC_ID, 600, false);
    const uint32_t synced = rec.seq;

    power_cut();

    zassert_equal(mn_recstore_recover(DIR), 1);
    zassert_false(exists(PART));
    read_recording(DONE, &rb);
    zassert_equal(rb.chunks, synced);
    zassert_false(rb.trailer, "a recovered recording has no trailer");
    zassert_true(rb.packets > 500 && rb.packets < 600, "%u packets survived", rb.packets);
}

/* A half-written chunk record at the end is cut off. */
ZTEST(recstore, test_recover_truncates_partial_chunk)
{
    struct readback rb;
    struct fs_dirent before;
    struct fs_dirent after;
    uint8_t partial[6 + 100];

    record(REC_ID, 300, false);
    const uint32_t synced = rec.seq;

    power_cut();
    zassert_ok(fs_stat(PART, &before));

    /* Next record's header claims 4096 bytes; only 100 made it. */
    const struct mn_mnr_chunk_hdr ch = {.seq = synced, .ct_len = 4096};

    mn_mnr_chunk_hdr_write(&ch, partial);
    memset(&partial[6], 0xAB, 100);
    append_raw(PART, partial, sizeof(partial));

    zassert_equal(mn_recstore_recover(DIR), 1);
    zassert_ok(fs_stat(DONE, &after));
    zassert_equal(after.size, before.size, "partial record not cut off");
    read_recording(DONE, &rb);
    zassert_equal(rb.chunks, synced);
}

/* A record whose sequence number is wrong ends the valid prefix too. */
ZTEST(recstore, test_recover_stops_at_out_of_sequence_record)
{
    struct readback rb;
    uint8_t bogus[6 + 1 + 16] = {0};

    record(REC_ID, 300, false);
    const uint32_t synced = rec.seq;

    power_cut();
    const struct mn_mnr_chunk_hdr ch = {.seq = synced + 5, .ct_len = 1};

    mn_mnr_chunk_hdr_write(&ch, bogus);
    append_raw(PART, bogus, sizeof(bogus));

    zassert_equal(mn_recstore_recover(DIR), 1);
    read_recording(DONE, &rb);
    zassert_equal(rb.chunks, synced);
}

/* No header on flash means no key wraps: the file is deleted. */
ZTEST(recstore, test_recover_deletes_headerless_file)
{
    static const uint8_t junk[10] = {'M', 'N', 'R', '1'};

    struct fs_file_t f;

    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, PART, FS_O_CREATE | FS_O_WRITE));
    zassert_equal(fs_write(&f, junk, sizeof(junk)), sizeof(junk));
    fs_close(&f);

    zassert_equal(mn_recstore_recover(DIR), 0);
    zassert_false(exists(PART));
    zassert_false(exists(DONE));
}

ZTEST(recstore, test_recover_leaves_finished_recordings_alone)
{
    struct readback rb;

    record(REC_ID, 50, true);
    record(REC_ID2, 50, false);
    power_cut();

    zassert_equal(mn_recstore_recover(DIR), 1);
    zassert_equal(mn_recstore_recover(DIR), 0, "second run finds nothing");
    read_recording(DONE, &rb);
    zassert_true(rb.trailer);
    zassert_true(exists(DIR "/0000000700000004.mnr"));
}
