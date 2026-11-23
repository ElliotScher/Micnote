#include "pairing.h"

#include <errno.h>
#include <string.h>

#include <mbedtls/platform_util.h>

static const char link_label[] = "micnote/v1/link";
static const char sas_label[] = "micnote/v1/sas";
#define LABEL_LEN(l) (sizeof(l) - 1)
#define PROOF_LABEL_LEN 4

int mn_pair_key_generate(psa_key_id_t *d, uint8_t d_pub[MN_X25519_KEY_SIZE])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    size_t len = 0;

    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    if (psa_generate_key(&attr, d) != PSA_SUCCESS) {
        return -EIO;
    }
    if (psa_export_public_key(*d, d_pub, MN_X25519_KEY_SIZE, &len) != PSA_SUCCESS ||
        len != MN_X25519_KEY_SIZE) {
        psa_destroy_key(*d);
        *d = PSA_KEY_ID_NULL;
        return -EIO;
    }
    return 0;
}

/* Starts HKDF-SHA256 over secret with salt and info (PSA's order). */
static int hkdf_setup(psa_key_derivation_operation_t *op, psa_key_id_t secret, const uint8_t *salt,
                      size_t salt_len, const uint8_t *info, size_t info_len)
{
    if (psa_key_derivation_setup(op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) != PSA_SUCCESS ||
        psa_key_derivation_input_bytes(op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len) !=
            PSA_SUCCESS ||
        psa_key_derivation_input_key(op, PSA_KEY_DERIVATION_INPUT_SECRET, secret) != PSA_SUCCESS ||
        psa_key_derivation_input_bytes(op, PSA_KEY_DERIVATION_INPUT_INFO, info, info_len) !=
            PSA_SUCCESS) {
        return -EIO;
    }
    return 0;
}

int mn_pair_derive(psa_key_id_t d, const uint8_t d_pub[MN_X25519_KEY_SIZE],
                   const uint8_t host_pub[MN_X25519_KEY_SIZE], const uint8_t host_id[8],
                   const uint8_t device_id[8], const uint8_t n_h[MN_PAIR_NONCE_SIZE],
                   const uint8_t n_d[MN_PAIR_NONCE_SIZE], psa_key_id_t *link_key,
                   uint8_t sas[MN_SAS_SYMBOLS])
{
    uint8_t ss[MN_X25519_KEY_SIZE];
    uint8_t salt[2 * MN_PAIR_NONCE_SIZE];
    uint8_t info[LABEL_LEN(sas_label) + 2 * MN_X25519_KEY_SIZE];
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_key_attributes_t secret_attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_attributes_t link_attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t secret = PSA_KEY_ID_NULL;
    int ret = mn_x25519_agree(d, host_pub, ss);

    *link_key = PSA_KEY_ID_NULL;
    if (ret != 0) {
        goto cleanup;
    }
    ret = -EIO;
    psa_set_key_type(&secret_attr, PSA_KEY_TYPE_DERIVE);
    psa_set_key_usage_flags(&secret_attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&secret_attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    if (psa_import_key(&secret_attr, ss, sizeof(ss), &secret) != PSA_SUCCESS) {
        goto cleanup;
    }

    /* The link key. */
    memcpy(salt, host_id, 8);
    memcpy(&salt[8], device_id, 8);
    psa_set_key_type(&link_attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&link_attr, 256);
    psa_set_key_usage_flags(
        &link_attr, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE | PSA_KEY_USAGE_COPY);
    psa_set_key_algorithm(&link_attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    if (hkdf_setup(&op, secret, salt, 16, (const uint8_t *)link_label, LABEL_LEN(link_label)) !=
            0 ||
        psa_key_derivation_output_key(&link_attr, &op, link_key) != PSA_SUCCESS) {
        goto cleanup;
    }
    psa_key_derivation_abort(&op);

    /* The SAS. */
    memcpy(salt, n_h, MN_PAIR_NONCE_SIZE);
    memcpy(&salt[MN_PAIR_NONCE_SIZE], n_d, MN_PAIR_NONCE_SIZE);
    memcpy(info, sas_label, LABEL_LEN(sas_label));
    memcpy(&info[LABEL_LEN(sas_label)], host_pub, MN_X25519_KEY_SIZE);
    memcpy(&info[LABEL_LEN(sas_label) + MN_X25519_KEY_SIZE], d_pub, MN_X25519_KEY_SIZE);
    if (hkdf_setup(&op, secret, salt, sizeof(salt), info, sizeof(info)) != 0 ||
        psa_key_derivation_output_bytes(&op, sas, MN_SAS_SYMBOLS) != PSA_SUCCESS) {
        goto cleanup;
    }
    for (int i = 0; i < MN_SAS_SYMBOLS; i++) {
        sas[i] %= MN_SAS_COLORS;
    }
    ret = 0;

cleanup:
    psa_key_derivation_abort(&op);
    psa_destroy_key(secret);
    mbedtls_platform_zeroize(ss, sizeof(ss));
    if (ret != 0 && *link_key != PSA_KEY_ID_NULL) {
        psa_destroy_key(*link_key);
        *link_key = PSA_KEY_ID_NULL;
    }
    return ret;
}

static void proof_input(const char *label, const uint8_t n_h[MN_PAIR_NONCE_SIZE],
                        const uint8_t n_d[MN_PAIR_NONCE_SIZE],
                        uint8_t out[PROOF_LABEL_LEN + 2 * MN_PAIR_NONCE_SIZE])
{
    memcpy(out, label, PROOF_LABEL_LEN);
    memcpy(&out[PROOF_LABEL_LEN], n_h, MN_PAIR_NONCE_SIZE);
    memcpy(&out[PROOF_LABEL_LEN + MN_PAIR_NONCE_SIZE], n_d, MN_PAIR_NONCE_SIZE);
}

int mn_pair_proof(psa_key_id_t link_key, const char *label, const uint8_t n_h[MN_PAIR_NONCE_SIZE],
                  const uint8_t n_d[MN_PAIR_NONCE_SIZE], uint8_t proof[MN_PAIR_PROOF_SIZE])
{
    uint8_t in[PROOF_LABEL_LEN + 2 * MN_PAIR_NONCE_SIZE];
    size_t len = 0;

    proof_input(label, n_h, n_d, in);
    if (psa_mac_compute(link_key, PSA_ALG_HMAC(PSA_ALG_SHA_256), in, sizeof(in), proof,
                        MN_PAIR_PROOF_SIZE, &len) != PSA_SUCCESS ||
        len != MN_PAIR_PROOF_SIZE) {
        return -EIO;
    }
    return 0;
}

int mn_pair_proof_verify(psa_key_id_t link_key, const char *label,
                         const uint8_t n_h[MN_PAIR_NONCE_SIZE],
                         const uint8_t n_d[MN_PAIR_NONCE_SIZE],
                         const uint8_t proof[MN_PAIR_PROOF_SIZE])
{
    uint8_t in[PROOF_LABEL_LEN + 2 * MN_PAIR_NONCE_SIZE];

    proof_input(label, n_h, n_d, in);
    const psa_status_t st = psa_mac_verify(link_key, PSA_ALG_HMAC(PSA_ALG_SHA_256), in, sizeof(in),
                                           proof, MN_PAIR_PROOF_SIZE);

    if (st == PSA_ERROR_INVALID_SIGNATURE) {
        return -EBADMSG;
    }
    return st == PSA_SUCCESS ? 0 : -EIO;
}
