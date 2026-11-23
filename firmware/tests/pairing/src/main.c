/*
 * Pairing crypto against known answers computed independently with
 * pyca/cryptography (core/tests/pairing_test.cpp checks the same values),
 * then the host-slot store.
 *
 * Inputs: the host key is RFC 7748 section 6.1's Bob, the device's pairing
 * key its Alice; device_id = 01..08, n_h = 00..0f, n_d = 10..1f.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <psa/crypto.h>

#include "hosts.h"
#include "pairing.h"
#include "wrap.h"

static const uint8_t host_priv[32] = {
    0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
    0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd, 0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb};
static const uint8_t dev_priv[32] = {
    0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
    0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a, 0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a};
static const uint8_t host_pub[32] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
    0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d, 0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f};
static const uint8_t dev_pub[32] = {
    0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54, 0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
    0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4, 0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a};
static const uint8_t host_id[8] = {0xf3, 0x5e, 0x56, 0x16, 0x16, 0x0a, 0x30, 0xbf};
static const uint8_t device_id[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static uint8_t n_h[16];
static uint8_t n_d[16];

/* Expected (pyca). sas bytes 25 7d b4 37, mod 6. */
static const uint8_t want_sas[4] = {1, 5, 0, 1};
static const uint8_t want_device_proof[32] = {
    0x90, 0xeb, 0x93, 0x12, 0x1c, 0x4b, 0x72, 0x39, 0x15, 0x52, 0x4b, 0xd0, 0xfc, 0xc0, 0x0f, 0xe8,
    0xac, 0xfa, 0x47, 0x95, 0x19, 0xc5, 0xc3, 0x37, 0x70, 0x89, 0xc6, 0xf1, 0x33, 0xab, 0x06, 0x04};
static const uint8_t want_host_proof[32] = {
    0x70, 0xb9, 0xb6, 0x06, 0x80, 0x42, 0xf5, 0xa4, 0x88, 0xc1, 0xb0, 0xc4, 0x7f, 0x2a, 0xb3, 0x0c,
    0x2a, 0x3c, 0x02, 0x14, 0xc1, 0x71, 0x2c, 0x4a, 0x80, 0x4d, 0x60, 0x3d, 0xa2, 0xc4, 0x6d, 0x47};

static psa_key_id_t dev_key;

/* The KAT's link key, as a fresh volatile key. */
static psa_key_id_t kat_link_key(uint8_t sas[4])
{
    psa_key_id_t link;

    zassert_ok(
        mn_pair_derive(dev_key, dev_pub, host_pub, host_id, device_id, n_h, n_d, &link, sas));
    return link;
}

static void *setup(void)
{
    const struct flash_area *fa;
    uint8_t pub[32];
    size_t len;

    for (int i = 0; i < 16; i++) {
        n_h[i] = (uint8_t)i;
        n_d[i] = (uint8_t)(16 + i);
    }
    zassert_ok(flash_area_open(PARTITION_ID(storage_partition), &fa));
    zassert_ok(flash_area_flatten(fa, 0, fa->fa_size));
    flash_area_close(fa);
    zassert_ok(settings_subsys_init());
    zassert_equal(psa_crypto_init(), PSA_SUCCESS);
    zassert_ok(mn_x25519_import(dev_priv, &dev_key));
    zassert_equal(psa_export_public_key(dev_key, pub, 32, &len), PSA_SUCCESS);
    zassert_mem_equal(pub, dev_pub, 32);
    zassert_ok(mn_hosts_init());
    return NULL;
}

ZTEST_SUITE(pairing, NULL, setup, NULL, NULL, NULL);

/* ---- crypto ------------------------------------------------------------ */

ZTEST(pairing, test_sas_and_proofs_match_known_answers)
{
    uint8_t sas[4];
    uint8_t proof[32];
    const psa_key_id_t link = kat_link_key(sas);

    zassert_mem_equal(sas, want_sas, 4);
    /* The proofs pin the link key too: it can't be exported. */
    zassert_ok(mn_pair_proof(link, MN_PROOF_PUCK, n_h, n_d, proof));
    zassert_mem_equal(proof, want_device_proof, 32);
    zassert_ok(mn_pair_proof(link, MN_PROOF_HOST, n_h, n_d, proof));
    zassert_mem_equal(proof, want_host_proof, 32);
    psa_destroy_key(link);
}

/* The host gets the same shared secret from its side of the exchange. */
ZTEST(pairing, test_both_sides_agree)
{
    psa_key_id_t host;
    uint8_t pub[32];
    uint8_t ss_host[32];
    uint8_t ss_dev[32];
    size_t len;

    zassert_ok(mn_x25519_import(host_priv, &host));
    zassert_equal(psa_export_public_key(host, pub, 32, &len), PSA_SUCCESS);
    zassert_mem_equal(pub, host_pub, 32);
    zassert_ok(mn_x25519_agree(host, dev_pub, ss_host));
    zassert_ok(mn_x25519_agree(dev_key, host_pub, ss_dev));
    zassert_mem_equal(ss_host, ss_dev, 32);
    psa_destroy_key(host);
}

ZTEST(pairing, test_proof_verify)
{
    uint8_t sas[4];
    uint8_t bad[32];
    const psa_key_id_t link = kat_link_key(sas);

    zassert_ok(mn_pair_proof_verify(link, MN_PROOF_HOST, n_h, n_d, want_host_proof));
    /* The other side's proof, a flipped bit, or swapped nonces all fail. */
    zassert_equal(mn_pair_proof_verify(link, MN_PROOF_HOST, n_h, n_d, want_device_proof), -EBADMSG);
    memcpy(bad, want_host_proof, 32);
    bad[31] ^= 1;
    zassert_equal(mn_pair_proof_verify(link, MN_PROOF_HOST, n_h, n_d, bad), -EBADMSG);
    zassert_equal(mn_pair_proof_verify(link, MN_PROOF_HOST, n_d, n_h, want_host_proof), -EBADMSG);
    psa_destroy_key(link);
}

ZTEST(pairing, test_inputs_change_the_outcome)
{
    uint8_t sas[4];
    uint8_t proof[32];
    psa_key_id_t link;
    uint8_t other_id[8] = {1, 2, 3, 4, 5, 6, 7, 9};

    /* Another device_id: another link key, same SAS. */
    zassert_ok(mn_pair_derive(dev_key, dev_pub, host_pub, host_id, other_id, n_h, n_d, &link, sas));
    zassert_mem_equal(sas, want_sas, 4);
    zassert_ok(mn_pair_proof(link, MN_PROOF_PUCK, n_h, n_d, proof));
    zassert_true(memcmp(proof, want_device_proof, 32) != 0);
    psa_destroy_key(link);
}

ZTEST(pairing, test_low_order_host_key_refused)
{
    static const uint8_t zero[32];
    uint8_t sas[4] = {9, 9, 9, 9};
    psa_key_id_t link = 1;

    zassert_equal(mn_pair_derive(dev_key, dev_pub, zero, host_id, device_id, n_h, n_d, &link, sas),
                  -EINVAL);
    zassert_equal(link, PSA_KEY_ID_NULL);
}

ZTEST(pairing, test_fresh_keys_differ)
{
    psa_key_id_t a;
    psa_key_id_t b;
    uint8_t pa[32];
    uint8_t pb[32];

    zassert_ok(mn_pair_key_generate(&a, pa));
    zassert_ok(mn_pair_key_generate(&b, pb));
    zassert_true(memcmp(pa, pb, 32) != 0);
    psa_destroy_key(a);
    psa_destroy_key(b);
}

/* ---- the host store ---------------------------------------------------- */

static void host_n(int n, uint8_t id[8], uint8_t pub[32])
{
    memset(id, 0xA0 + n, 8);
    memset(pub, 0xB0 + n, 32);
}

ZTEST(pairing, test_hosts_add_find_remove)
{
    uint8_t sas[4];
    uint8_t proof[32];
    const psa_key_id_t link = kat_link_key(sas);

    zassert_equal(mn_hosts_count(), 0);
    zassert_equal(mn_hosts_find(host_id), -ENOENT);
    zassert_equal(mn_hosts_add(host_id, host_pub, "laptop", link), 0);
    psa_destroy_key(link);
    zassert_equal(mn_hosts_find(host_id), 0);
    zassert_equal(mn_hosts_count(), 1);
    zassert_str_equal(mn_hosts_get(0)->name, "laptop");

    /* The stored link key answers like the one it was copied from. */
    zassert_ok(mn_pair_proof(mn_hosts_link_key(0), MN_PROOF_PUCK, n_h, n_d, proof));
    zassert_mem_equal(proof, want_device_proof, 32);

    struct mn_rec_recipient r[MN_HOSTS_MAX];

    zassert_equal(mn_hosts_recipients(r), 1);
    zassert_mem_equal(r[0].pub, host_pub, 32);
    zassert_mem_equal(r[0].host_id, host_id, 8);

    zassert_ok(mn_hosts_remove(host_id));
    zassert_equal(mn_hosts_remove(host_id), -ENOENT);
    zassert_equal(mn_hosts_count(), 0);
    zassert_is_null(mn_hosts_get(0));
    zassert_not_equal(mn_pair_proof(mn_hosts_link_key(0), MN_PROOF_PUCK, n_h, n_d, proof), 0,
                      "the link key went with the slot");
}

ZTEST(pairing, test_hosts_persist_across_reboot)
{
    uint8_t sas[4];
    uint8_t proof[32];
    const psa_key_id_t link = kat_link_key(sas);

    zassert_equal(mn_hosts_add(host_id, host_pub, "desk", link), 0);
    psa_destroy_key(link);

    /* A reboot: RAM table and volatile keys gone, then reloaded. */
    mbedtls_psa_crypto_free();
    zassert_equal(psa_crypto_init(), PSA_SUCCESS);
    zassert_ok(mn_x25519_import(dev_priv, &dev_key));
    zassert_ok(mn_hosts_init());
    zassert_equal(mn_hosts_find(host_id), 0);
    zassert_str_equal(mn_hosts_get(0)->name, "desk");
    zassert_ok(mn_pair_proof(mn_hosts_link_key(0), MN_PROOF_PUCK, n_h, n_d, proof));
    zassert_mem_equal(proof, want_device_proof, 32);
    zassert_ok(mn_hosts_remove(host_id));
}

ZTEST(pairing, test_hosts_full_and_repair)
{
    uint8_t sas[4];
    uint8_t id[8];
    uint8_t pub[32];
    const psa_key_id_t link = kat_link_key(sas);

    for (int i = 0; i < MN_HOSTS_MAX; i++) {
        host_n(i, id, pub);
        zassert_equal(mn_hosts_add(id, pub, "h", link), i);
    }
    host_n(9, id, pub);
    zassert_equal(mn_hosts_add(id, pub, "fifth", link), -ENOSPC);

    /* Pairing an existing host again keeps its slot and renames it. */
    host_n(2, id, pub);
    zassert_equal(mn_hosts_add(id, pub, "again", link), 2);
    zassert_str_equal(mn_hosts_get(2)->name, "again");
    zassert_equal(mn_hosts_count(), MN_HOSTS_MAX);

    /* A freed slot is reused; a long name is cut to fit. */
    host_n(1, id, pub);
    zassert_ok(mn_hosts_remove(id));
    host_n(9, id, pub);
    zassert_equal(mn_hosts_add(id, pub, "a name that is much longer than thirty-one bytes", link),
                  1);
    zassert_equal(strlen(mn_hosts_get(1)->name), MN_HOST_NAME_MAX - 1);
    psa_destroy_key(link);

    for (int i = 0; i < MN_HOSTS_MAX; i++) {
        zassert_ok(mn_hosts_remove(mn_hosts_get(i)->host_id));
    }
}
