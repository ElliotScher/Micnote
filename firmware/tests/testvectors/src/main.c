/*
 * Generates proto/testvectors/ (stage 1 section 4.15). Writes recordings
 * with the real recstore from fixed keys and fixed "randomness", reads each
 * one back with the host key, and prints every file between markers:
 *
 *   MNTV-FILE <name>
 *   <hex, 64 bytes per line>
 *   MNTV-END
 *
 * proto/testvectors.py extracts them. Everything is deterministic, so
 * regenerating gives identical files and CI can check for drift.
 *
 * The packets are a synthetic pattern, not real Opus, until the encoder
 * lands (stage 1 PR 2): the vectors pin the container and the crypto.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>

#include <psa/crypto.h>

#include "aead.h"
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

/* ---- keys -------------------------------------------------------------- */

struct key {
    const char *name;
    uint8_t priv[32];
    uint8_t pub[32];
    uint8_t host_id[8];
    psa_key_id_t id;
};

/* host_a is RFC 7748 section 6.1 Bob; the others are fixed patterns (any 32
 * bytes are a valid X25519 private key). */
static struct key keys[] = {
    {.name = "host_a", .priv = {0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f,
                                0x8b, 0x83, 0x80, 0x0e, 0xe6, 0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18,
                                0xb6, 0xfd, 0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb}},
    {.name = "host_b"},
    {.name = "device_fallback"},
};
enum { HOST_A, HOST_B, FALLBACK };

static void derive_key(struct key *k)
{
    size_t len = 0;
    uint8_t hash[32];

    zassert_ok(mn_x25519_import(k->priv, &k->id));
    zassert_equal(psa_export_public_key(k->id, k->pub, sizeof(k->pub), &len), PSA_SUCCESS);
    zassert_equal(
        psa_hash_compute(PSA_ALG_SHA_256, k->pub, sizeof(k->pub), hash, sizeof(hash), &len),
        PSA_SUCCESS);
    memcpy(k->host_id, hash, sizeof(k->host_id));
}

/* ---- recordings -------------------------------------------------------- */

static struct mn_rec rec;

static size_t packet(uint32_t i, uint8_t *out)
{
    const size_t len = 30 + i % 20;

    for (size_t j = 0; j < len; j++) {
        out[j] = (uint8_t)(i * 7 + j);
    }
    return len;
}

/* markers: packet indexes before which an item marker goes. */
static void write_recording(uint64_t rec_id, const int *recipients, uint8_t n_recipients,
                            uint32_t n_packets, const uint32_t *markers, size_t n_markers)
{
    static struct mn_rec_params p;
    static struct mn_rec_secrets s;
    uint8_t buf[64];
    size_t m = 0;

    memset(&p, 0, sizeof(p));
    p.rec_id = rec_id;
    p.frame_ms = 20;
    p.start_boot = (uint32_t)(rec_id >> 32);
    p.start_uptime_ms = 60000;
    p.start_unix_ms = 1763800000000ULL;
    p.n_recipients = n_recipients;
    for (uint8_t i = 0; i < n_recipients; i++) {
        memcpy(p.recipients[i].pub, keys[recipients[i]].pub, 32);
        memcpy(p.recipients[i].host_id, keys[recipients[i]].host_id, 8);
    }
    /* Fixed "randomness", different per recording. */
    for (size_t i = 0; i < sizeof(s); i++) {
        ((uint8_t *)&s)[i] = (uint8_t)(i * 31 + rec_id);
    }

    zassert_ok(mn_rec_open(&rec, DIR, &p, &s));
    for (uint32_t i = 0; i < n_packets; i++) {
        if (m < n_markers && markers[m] == i) {
            zassert_ok(mn_rec_marker(&rec, 1, i * 20));
            m++;
        }
        zassert_ok(mn_rec_append(&rec, buf, packet(i, buf)));
    }
}

/* ---- reading back, for the expected values ----------------------------- */

static uint8_t file_buf[64 * 1024];

struct summary {
    size_t size;
    uint32_t chunks;
    uint32_t packets;
    uint32_t markers;
    uint32_t marker_offsets[8];
    bool trailer;
    uint32_t duration_ms;
    uint8_t packet_stream_sha256[32];
    uint8_t file_sha256[32];
};

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

/* Decrypts a recording with key k (one of its recipients) and summarizes it.
 * packet_stream_sha256 covers every packet as `len u16 LE | bytes`, in order:
 * what a host must get back. */
static void summarize(const char *path, const struct key *k, struct summary *out)
{
    static struct mn_mnr_header h;
    static uint8_t pt[MN_MNR_MAX_CT];
    uint8_t core_hash[32];
    uint8_t cek[MN_CEK_SIZE];
    psa_key_id_t cek_key;
    psa_hash_operation_t stream = PSA_HASH_OPERATION_INIT;
    size_t len = 0;
    int wrap = -1;

    memset(out, 0, sizeof(*out));
    out->size = read_file(path);
    const int hdr_len = mn_mnr_header_parse(file_buf, out->size, &h);

    zassert_true(hdr_len > 0);
    for (int i = 0; i < h.n_wraps; i++) {
        if (memcmp(h.wraps[i].host_id, k->host_id, 8) == 0) {
            wrap = i;
        }
    }
    zassert_true(wrap >= 0, "%s isn't a recipient of %s", k->name, path);
    zassert_ok(mn_unwrap_cek(k->id, h.rec_id, &h.wraps[wrap], cek));
    zassert_ok(mn_cek_import(cek, &cek_key));
    zassert_ok(mn_header_core_hash(file_buf, core_hash));
    zassert_equal(psa_hash_setup(&stream, PSA_ALG_SHA_256), PSA_SUCCESS);

    for (size_t off = (size_t)hdr_len; off < out->size; out->chunks++) {
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
            const uint16_t plen = sys_get_le16(&pt[p]);

            if (plen == MN_CHUNK_MARKER_SENTINEL) {
                out->marker_offsets[out->markers++] = sys_get_le32(&pt[p + 3]);
                p += MN_CHUNK_MARKER_SIZE;
                continue;
            }
            psa_hash_update(&stream, &pt[p], 2 + plen);
            out->packets++;
            p += 2 + plen;
        }
    }
    if (!out->trailer) {
        /* Ended by a power cut: the host counts the packets instead. */
        out->duration_ms = out->packets * 20;
    }
    zassert_equal(psa_hash_finish(&stream, out->packet_stream_sha256, 32, &len), PSA_SUCCESS);
    /* Ack's hash: the header and every stored chunk record, i.e. the file. */
    zassert_equal(
        psa_hash_compute(PSA_ALG_SHA_256, file_buf, out->size, out->file_sha256, 32, &len),
        PSA_SUCCESS);
    psa_destroy_key(cek_key);
}

/* ---- output ------------------------------------------------------------ */

static void print_hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        printk("%02x", p[i]);
    }
}

static void dump_bytes(const char *name, const uint8_t *p, size_t n)
{
    printk("MNTV-FILE %s\n", name);
    for (size_t i = 0; i < n; i += 64) {
        print_hex(&p[i], MIN(64, n - i));
        printk("\n");
    }
    printk("MNTV-END\n");
}

static void dump_file(const char *path, const char *name)
{
    dump_bytes(name, file_buf, read_file(path));
}

/* vectors.json, built as text so it's one more MNTV-FILE. */
static char json[4096];
static size_t json_len;

static void emit(const char *s)
{
    const size_t n = strlen(s);

    zassert_true(json_len + n < sizeof(json));
    memcpy(&json[json_len], s, n);
    json_len += n;
}

static void emit_hex(const uint8_t *p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    char two[3] = {0};

    emit("\"");
    for (size_t i = 0; i < n; i++) {
        two[0] = digits[p[i] >> 4];
        two[1] = digits[p[i] & 0xF];
        emit(two);
    }
    emit("\"");
}

static void emit_u32(uint32_t v)
{
    char buf[11];
    int i = sizeof(buf) - 1;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    emit(&buf[i]);
}

static void emit_recording(const char *file, uint64_t rec_id, const int *recipients, int n,
                           const struct summary *s, bool last)
{
    uint8_t id[8];

    sys_put_be64(rec_id, id);
    emit("    {\n      \"file\": \"");
    emit(file);
    emit("\",\n      \"rec_id\": ");
    emit_hex(id, 8);
    emit(",\n      \"recipients\": [");
    for (int i = 0; i < n; i++) {
        emit(i ? ", \"" : "\"");
        emit(keys[recipients[i]].name);
        emit("\"");
    }
    emit("],\n      \"size\": ");
    emit_u32((uint32_t)s->size);
    emit(",\n      \"chunks\": ");
    emit_u32(s->chunks);
    emit(",\n      \"packets\": ");
    emit_u32(s->packets);
    emit(",\n      \"marker_offsets_ms\": [");
    for (uint32_t i = 0; i < s->markers; i++) {
        if (i) {
            emit(", ");
        }
        emit_u32(s->marker_offsets[i]);
    }
    emit("],\n      \"trailer\": ");
    emit(s->trailer ? "true" : "false");
    emit(",\n      \"duration_ms\": ");
    emit_u32(s->duration_ms);
    emit(",\n      \"packet_stream_sha256\": ");
    emit_hex(s->packet_stream_sha256, 32);
    emit(",\n      \"file_sha256\": ");
    emit_hex(s->file_sha256, 32);
    emit(last ? "\n    }\n" : "\n    },\n");
}

/* ---- the generator ----------------------------------------------------- */

static void *setup(void)
{
    const struct flash_area *fa;

    zassert_equal(psa_crypto_init(), PSA_SUCCESS);
    memset(keys[HOST_B].priv, 0x42, 32);
    memset(keys[FALLBACK].priv, 0x46, 32);
    for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
        derive_key(&keys[i]);
    }
    zassert_ok(flash_area_open(PARTITION_ID(rec_partition), &fa));
    zassert_ok(flash_area_flatten(fa, 0, fa->fa_size));
    flash_area_close(fa);
    zassert_ok(fs_mount(&mnt));
    zassert_ok(mn_recstore_init(DIR));
    return NULL;
}

ZTEST_SUITE(testvectors, NULL, setup, NULL, NULL, NULL);

ZTEST(testvectors, test_generate)
{
    static const int two_hosts[] = {HOST_A, HOST_B};
    static const int fallback[] = {FALLBACK};
    static const int host_a[] = {HOST_A};
    static const uint32_t markers[] = {50, 150};
    static struct summary two, two_b, fb, cut;

    /* 1. Two hosts, two item markers, closed normally: ~10 KB, 3 audio
     *    chunks + the trailer. */
    write_recording(0x0000000100000001ULL, two_hosts, 2, 250, markers, 2);
    zassert_ok(mn_rec_close(&rec));
    summarize(DIR "/0000000100000001.mnr", &keys[HOST_A], &two);
    summarize(DIR "/0000000100000001.mnr", &keys[HOST_B], &two_b);
    zassert_mem_equal(two.packet_stream_sha256, two_b.packet_stream_sha256, 32,
                      "both hosts must read the same audio");
    zassert_equal(two.packets, 250);
    zassert_equal(two.markers, 2);
    zassert_true(two.trailer);
    zassert_equal(two.duration_ms, 5000);

    /* 2. Before any pairing: wrapped to the device fallback key only. */
    write_recording(0x0000000100000002ULL, fallback, 1, 100, NULL, 0);
    zassert_ok(mn_rec_close(&rec));
    summarize(DIR "/0000000100000002.mnr", &keys[FALLBACK], &fb);
    zassert_equal(fb.packets, 100);

    /* 3. A power cut while recording, then recovery at the next boot. */
    write_recording(0x0000000200000001ULL, host_a, 1, 300, NULL, 0);
    fs_close(&rec.file);
    psa_destroy_key(rec.cek);
    rec.open = false;
    zassert_equal(mn_recstore_recover(DIR), 1);
    summarize(DIR "/0000000200000001.mnr", &keys[HOST_A], &cut);
    zassert_false(cut.trailer);
    zassert_true(cut.packets > 0 && cut.packets < 300);

    dump_file(DIR "/0000000100000001.mnr", "rec_two_hosts.mnr");
    dump_file(DIR "/0000000100000002.mnr", "rec_fallback.mnr");
    dump_file(DIR "/0000000200000001.mnr", "rec_power_cut.mnr");

    emit("{\n  \"format\": 1,\n  \"keys\": {\n");
    for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
        emit("    \"");
        emit(keys[i].name);
        emit("\": {\"priv\": ");
        emit_hex(keys[i].priv, 32);
        emit(", \"pub\": ");
        emit_hex(keys[i].pub, 32);
        emit(", \"host_id\": ");
        emit_hex(keys[i].host_id, 8);
        emit(i + 1 < ARRAY_SIZE(keys) ? "},\n" : "}\n");
    }
    emit("  },\n  \"recordings\": [\n");
    emit_recording("rec_two_hosts.mnr", 0x0000000100000001ULL, two_hosts, 2, &two, false);
    emit_recording("rec_fallback.mnr", 0x0000000100000002ULL, fallback, 1, &fb, false);
    emit_recording("rec_power_cut.mnr", 0x0000000200000001ULL, host_a, 1, &cut, true);
    emit("  ]\n}\n");
    dump_bytes("vectors.json", (const uint8_t *)json, json_len);
}
