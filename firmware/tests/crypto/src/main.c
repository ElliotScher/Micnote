/*
 * Key wrapping and chunk encryption (stage 1 section 4.5).
 *
 * The known answers were computed independently with pyca/cryptography, from
 * the formulas in docs/stages/01-firmware-and-sync.md section 4.5:
 *   eph = RFC 7748 6.1 Alice, host = RFC 7748 6.1 Bob,
 *   host_id = SHA-256(host_pub)[:8], rec_id = 0x0000000700000003,
 *   cek = 00 01 .. 1f, and for the chunk: seq 5, plaintext 01 "micnote chunk",
 *   core = the tests/mnr sample header's first 40 bytes.
 * A mismatch here means the firmware no longer follows the spec, even if its
 * own wrap/unwrap round trip still works.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <psa/crypto.h>

#include "aead.h"
#include "mnr.h"
#include "wrap.h"

/* RFC 7748 section 6.1. */
static const uint8_t alice_priv[32] = {
    0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
    0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a, 0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a};
static const uint8_t alice_pub[32] = {
    0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54, 0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
    0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4, 0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a};
static const uint8_t bob_priv[32] = {
    0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
    0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd, 0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb};
static const uint8_t bob_pub[32] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
    0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d, 0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f};

/* Known answers (pyca/cryptography, see the top of this file). */
static const uint8_t host_id[8] = {0xf3, 0x5e, 0x56, 0x16, 0x16, 0x0a, 0x30, 0xbf};
static const uint8_t kat_wrapped_cek[32] = {
    0x50, 0x45, 0xa9, 0xe3, 0x2c, 0x22, 0xcf, 0x5d, 0xcf, 0x48, 0x7a, 0x4f, 0xa3, 0xe9, 0x08, 0x20,
    0xf1, 0x7a, 0x5a, 0x7b, 0x13, 0xb3, 0x83, 0x87, 0x2a, 0x93, 0x15, 0x0b, 0xe9, 0xc0, 0xe0, 0x79};
static const uint8_t kat_wrap_tag[16] = {0x0b, 0x18, 0xd4, 0x9b, 0xe2, 0xc5, 0x8e, 0x2f,
                                         0xc6, 0x86, 0x58, 0x71, 0x4e, 0x23, 0x0e, 0x14};
static const uint8_t kat_core_hash[32] = {
    0x91, 0x97, 0xa9, 0x14, 0xdc, 0x88, 0xa6, 0xa1, 0xfb, 0x55, 0x6e, 0x42, 0x35, 0xcc, 0x00, 0xbe,
    0x74, 0x54, 0x8f, 0xe0, 0xd6, 0xcf, 0xc0, 0x5f, 0xdd, 0x9d, 0xb1, 0x0d, 0xe2, 0x7e, 0xf4, 0x70};
static const uint8_t kat_chunk_pt[] = {0x01, 'm', 'i', 'c', 'n', 'o', 't',
                                       'e',  ' ', 'c', 'h', 'u', 'n', 'k'};
static const uint8_t kat_chunk_ct[30] = {
    0x8e, 0xe4, 0x26, 0xc2, 0xa2, 0x59, 0xff, 0x4e, 0x3c, 0x17, 0x9a, 0x4a, 0x2f, 0x3c, 0xfb,
    0x54, 0x16, 0x16, 0x5e, 0xad, 0x12, 0x9d, 0x5f, 0x64, 0x5a, 0xc1, 0x45, 0xfb, 0x9f, 0x63};

#define REC_ID 0x0000000700000003ULL
#define CHUNK_SEQ 5

static uint8_t cek[MN_CEK_SIZE];
static struct mn_mnr_wrap wrap;
static psa_key_id_t bob_key;
static psa_key_id_t cek_key;
static uint8_t out[MN_MNR_MAX_CT + MN_MNR_TAG_SIZE];

static void *setup(void)
{
    zassert_equal(psa_crypto_init(), PSA_SUCCESS);
    return NULL;
}

static void before_each(void *fixture)
{
    ARG_UNUSED(fixture);
    for (size_t i = 0; i < sizeof(cek); i++) {
        cek[i] = (uint8_t)i;
    }
    memset(&wrap, 0, sizeof(wrap));
    memset(out, 0xEE, sizeof(out));
    zassert_ok(mn_x25519_import(bob_priv, &bob_key));
    zassert_ok(mn_cek_import(cek, &cek_key));
}

static void after_each(void *fixture)
{
    ARG_UNUSED(fixture);
    psa_destroy_key(bob_key);
    psa_destroy_key(cek_key);
}

ZTEST_SUITE(crypto, NULL, setup, before_each, after_each, NULL);

/* ---- wrapping ---------------------------------------------------------- */

ZTEST(crypto, test_wrap_matches_known_answer)
{
    zassert_ok(mn_wrap_cek(alice_priv, bob_pub, host_id, REC_ID, cek, &wrap));
    zassert_mem_equal(wrap.host_id, host_id, sizeof(host_id));
    zassert_mem_equal(wrap.eph_pub, alice_pub, sizeof(alice_pub), "eph_pub from eph_priv");
    zassert_mem_equal(wrap.wrapped_cek, kat_wrapped_cek, sizeof(kat_wrapped_cek));
    zassert_mem_equal(wrap.tag, kat_wrap_tag, sizeof(kat_wrap_tag));
}

ZTEST(crypto, test_unwrap_recovers_cek)
{
    uint8_t got[MN_CEK_SIZE];

    zassert_ok(mn_wrap_cek(alice_priv, bob_pub, host_id, REC_ID, cek, &wrap));
    zassert_ok(mn_unwrap_cek(bob_key, REC_ID, &wrap, got));
    zassert_mem_equal(got, cek, sizeof(cek));
}

static void expect_unwrap_fails(psa_key_id_t key, uint64_t rec_id, const struct mn_mnr_wrap *w)
{
    uint8_t got[MN_CEK_SIZE];

    memset(got, 0xEE, sizeof(got));
    zassert_equal(mn_unwrap_cek(key, rec_id, w, got), -EBADMSG);
    for (size_t i = 0; i < sizeof(got); i++) {
        zassert_equal(got[i], 0, "cek_out not zeroed at %zu", i);
    }
}

ZTEST(crypto, test_unwrap_with_wrong_key_fails)
{
    psa_key_id_t alice_key;

    zassert_ok(mn_wrap_cek(alice_priv, bob_pub, host_id, REC_ID, cek, &wrap));
    zassert_ok(mn_x25519_import(alice_priv, &alice_key));
    expect_unwrap_fails(alice_key, REC_ID, &wrap);
    psa_destroy_key(alice_key);
}

/* The salt binds a wrap to its recording: it can't be copied into another. */
ZTEST(crypto, test_unwrap_with_wrong_rec_id_fails)
{
    zassert_ok(mn_wrap_cek(alice_priv, bob_pub, host_id, REC_ID, cek, &wrap));
    expect_unwrap_fails(bob_key, REC_ID + 1, &wrap);
}

ZTEST(crypto, test_unwrap_rejects_tampering)
{
    zassert_ok(mn_wrap_cek(alice_priv, bob_pub, host_id, REC_ID, cek, &wrap));
    const struct mn_mnr_wrap good = wrap;

    wrap.wrapped_cek[0] ^= 0x01;
    expect_unwrap_fails(bob_key, REC_ID, &wrap);
    wrap = good;
    wrap.tag[15] ^= 0x80;
    expect_unwrap_fails(bob_key, REC_ID, &wrap);
    wrap = good;
    wrap.host_id[0] ^= 0x01;
    expect_unwrap_fails(bob_key, REC_ID, &wrap);
}

/* An all-zero public key is a low-order point: X25519 with it gives an
 * all-zero secret whatever the private key, so the KEK would be public. */
ZTEST(crypto, test_wrap_refuses_low_order_recipient)
{
    static const uint8_t zero_pub[32];

    zassert_equal(mn_wrap_cek(alice_priv, zero_pub, host_id, REC_ID, cek, &wrap), -EINVAL);
}

ZTEST(crypto, test_unwrap_refuses_low_order_eph_pub)
{
    uint8_t got[MN_CEK_SIZE];

    zassert_ok(mn_wrap_cek(alice_priv, bob_pub, host_id, REC_ID, cek, &wrap));
    memset(wrap.eph_pub, 0, sizeof(wrap.eph_pub));
    zassert_equal(mn_unwrap_cek(bob_key, REC_ID, &wrap, got), -EINVAL);
}

/* ---- chunks ------------------------------------------------------------ */

static void sample_core_hash(uint8_t hash[MN_SHA256_SIZE])
{
    /* The tests/mnr sample header; only the core matters. */
    static struct mn_mnr_header h;
    static uint8_t hdr[MN_MNR_HEADER_SIZE];

    memset(&h, 0, sizeof(h));
    h.flags = MN_MNR_FLAG_STEREO;
    h.rec_id = REC_ID;
    h.codec = MN_MNR_CODEC_OPUS;
    h.rate_hz = 16000;
    h.frame_ms = 20;
    h.start_boot = 7;
    h.start_uptime_ms = 123456789ULL;
    h.start_unix_ms = 1763800000000ULL;
    zassert_equal(mn_mnr_header_write(&h, hdr, sizeof(hdr)), MN_MNR_HEADER_SIZE);
    zassert_ok(mn_header_core_hash(hdr, hash));
}

ZTEST(crypto, test_core_hash_matches_known_answer)
{
    uint8_t hash[MN_SHA256_SIZE];

    sample_core_hash(hash);
    zassert_mem_equal(hash, kat_core_hash, sizeof(hash));
}

ZTEST(crypto, test_seal_matches_known_answer)
{
    const int n = mn_chunk_seal(cek_key, REC_ID, CHUNK_SEQ, kat_core_hash, kat_chunk_pt,
                                sizeof(kat_chunk_pt), out, sizeof(out));

    zassert_equal(n, sizeof(kat_chunk_ct));
    zassert_mem_equal(out, kat_chunk_ct, sizeof(kat_chunk_ct));
}

ZTEST(crypto, test_open_recovers_plaintext)
{
    const int n = mn_chunk_open(cek_key, REC_ID, CHUNK_SEQ, kat_core_hash, kat_chunk_ct,
                                sizeof(kat_chunk_ct), out, sizeof(out));

    zassert_equal(n, sizeof(kat_chunk_pt));
    zassert_mem_equal(out, kat_chunk_pt, sizeof(kat_chunk_pt));
}

ZTEST(crypto, test_largest_chunk_round_trips)
{
    static uint8_t pt[MN_MNR_MAX_CT];
    static uint8_t back[MN_MNR_MAX_CT];

    memset(pt, 0x5A, sizeof(pt));
    zassert_equal(
        mn_chunk_seal(cek_key, REC_ID, 0, kat_core_hash, pt, sizeof(pt), out, sizeof(out)),
        sizeof(out));
    zassert_equal(
        mn_chunk_open(cek_key, REC_ID, 0, kat_core_hash, out, sizeof(out), back, sizeof(back)),
        sizeof(back));
    zassert_mem_equal(back, pt, sizeof(pt));
}

static void expect_open_fails(uint64_t rec_id, uint32_t seq, const uint8_t *hash, const uint8_t *ct)
{
    uint8_t pt[sizeof(kat_chunk_pt)];

    memset(pt, 0xEE, sizeof(pt));
    zassert_equal(
        mn_chunk_open(cek_key, rec_id, seq, hash, ct, sizeof(kat_chunk_ct), pt, sizeof(pt)),
        -EBADMSG);
    for (size_t i = 0; i < sizeof(pt); i++) {
        zassert_equal(pt[i], 0, "pt not zeroed at %zu", i);
    }
}

/* The nonce and AAD pin a chunk to its recording, position and header. */
ZTEST(crypto, test_open_rejects_moved_or_tampered_chunks)
{
    uint8_t ct[sizeof(kat_chunk_ct)];
    uint8_t hash[MN_SHA256_SIZE];

    expect_open_fails(REC_ID + 1, CHUNK_SEQ, kat_core_hash, kat_chunk_ct);
    expect_open_fails(REC_ID, CHUNK_SEQ + 1, kat_core_hash, kat_chunk_ct);

    memcpy(hash, kat_core_hash, sizeof(hash));
    hash[31] ^= 0x01;
    expect_open_fails(REC_ID, CHUNK_SEQ, hash, kat_chunk_ct);

    memcpy(ct, kat_chunk_ct, sizeof(ct));
    ct[0] ^= 0x01;
    expect_open_fails(REC_ID, CHUNK_SEQ, kat_core_hash, ct);
    memcpy(ct, kat_chunk_ct, sizeof(ct));
    ct[sizeof(ct) - 1] ^= 0x01;
    expect_open_fails(REC_ID, CHUNK_SEQ, kat_core_hash, ct);
}

ZTEST(crypto, test_seal_rejects_bad_lengths)
{
    static uint8_t big[MN_MNR_MAX_CT + 1];

    zassert_equal(mn_chunk_seal(cek_key, REC_ID, 0, kat_core_hash, big, 0, out, sizeof(out)),
                  -EINVAL, "empty");
    zassert_equal(
        mn_chunk_seal(cek_key, REC_ID, 0, kat_core_hash, big, sizeof(big), out, sizeof(out)),
        -EINVAL, "over 4096");
    zassert_equal(mn_chunk_seal(cek_key, REC_ID, 0, kat_core_hash, kat_chunk_pt,
                                sizeof(kat_chunk_pt), out, sizeof(kat_chunk_ct) - 1),
                  -ENOSPC);
}

ZTEST(crypto, test_open_rejects_bad_lengths)
{
    zassert_equal(mn_chunk_open(cek_key, REC_ID, CHUNK_SEQ, kat_core_hash, kat_chunk_ct,
                                MN_MNR_TAG_SIZE, out, sizeof(out)),
                  -EINVAL, "tag only");
    zassert_equal(mn_chunk_open(cek_key, REC_ID, CHUNK_SEQ, kat_core_hash, kat_chunk_ct,
                                sizeof(kat_chunk_ct), out, sizeof(kat_chunk_pt) - 1),
                  -ENOSPC);
}
