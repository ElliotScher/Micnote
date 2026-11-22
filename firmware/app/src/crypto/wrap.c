#include "wrap.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include <mbedtls/platform_util.h>

static const char kek_label[] = "micnote/v1/kek";
#define KEK_LABEL_LEN (sizeof(kek_label) - 1)

int mn_x25519_import(const uint8_t priv[MN_X25519_KEY_SIZE], psa_key_id_t *out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    return psa_import_key(&attr, priv, MN_X25519_KEY_SIZE, out) == PSA_SUCCESS ? 0 : -EIO;
}

static bool all_zero(const uint8_t *p, size_t n)
{
    uint8_t acc = 0;

    for (size_t i = 0; i < n; i++) {
        acc |= p[i];
    }
    return acc == 0;
}

/*
 * X25519 with priv and peer_pub, then HKDF into an AES-256-CCM key. The raw
 * shared secret is computed separately (not fed straight into the
 * derivation) so an all-zero result from a low-order peer key can be
 * refused: it would make the KEK a public constant (RFC 7748 section 6.1).
 */
static int derive_kek(psa_key_id_t priv, const uint8_t peer_pub[MN_X25519_KEY_SIZE],
                      const uint8_t eph_pub[MN_X25519_KEY_SIZE], uint64_t rec_id,
                      const uint8_t host_id[8], psa_key_id_t *kek)
{
    uint8_t ss[MN_X25519_KEY_SIZE];
    uint8_t salt[8 + MN_X25519_KEY_SIZE];
    uint8_t info[KEK_LABEL_LEN + 8];
    size_t ss_len = 0;
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_attributes_t secret_attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t secret = PSA_KEY_ID_NULL;
    int ret = -EIO;

    const psa_status_t st = psa_raw_key_agreement(PSA_ALG_ECDH, priv, peer_pub, MN_X25519_KEY_SIZE,
                                                  ss, sizeof(ss), &ss_len);

    if (st == PSA_ERROR_INVALID_ARGUMENT) {
        /* Mbed TLS refuses low-order peer keys itself. */
        ret = -EINVAL;
        goto cleanup;
    }
    if (st != PSA_SUCCESS || ss_len != sizeof(ss)) {
        goto cleanup;
    }
    /* Not every PSA provider checks, so check again here. */
    if (all_zero(ss, sizeof(ss))) {
        ret = -EINVAL;
        goto cleanup;
    }

    sys_put_le64(rec_id, salt);
    memcpy(&salt[8], eph_pub, MN_X25519_KEY_SIZE);
    memcpy(info, kek_label, KEK_LABEL_LEN);
    memcpy(&info[KEK_LABEL_LEN], host_id, 8);

    /* PSA only derives a key (output_key) from a secret that is itself a
     * key, so the shared secret goes in as a short-lived DERIVE key. */
    psa_set_key_type(&secret_attr, PSA_KEY_TYPE_DERIVE);
    psa_set_key_usage_flags(&secret_attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&secret_attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    if (psa_import_key(&secret_attr, ss, sizeof(ss), &secret) != PSA_SUCCESS) {
        goto cleanup;
    }

    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CCM);

    /* PSA requires HKDF's inputs in this order: salt, secret, info. */
    if (psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) != PSA_SUCCESS ||
        psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, sizeof(salt)) !=
            PSA_SUCCESS ||
        psa_key_derivation_input_key(&op, PSA_KEY_DERIVATION_INPUT_SECRET, secret) != PSA_SUCCESS ||
        psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info, sizeof(info)) !=
            PSA_SUCCESS ||
        psa_key_derivation_output_key(&attr, &op, kek) != PSA_SUCCESS) {
        goto cleanup;
    }
    ret = 0;

cleanup:
    psa_key_derivation_abort(&op);
    psa_destroy_key(secret);
    mbedtls_platform_zeroize(ss, sizeof(ss));
    return ret;
}

/* The wrap's CCM nonce: all zeros, safe because every KEK is used once. */
static const uint8_t wrap_nonce[13];

int mn_wrap_cek(const uint8_t eph_priv[MN_X25519_KEY_SIZE],
                const uint8_t recipient_pub[MN_X25519_KEY_SIZE], const uint8_t host_id[8],
                uint64_t rec_id, const uint8_t cek[MN_CEK_SIZE], struct mn_mnr_wrap *out)
{
    psa_key_id_t eph = PSA_KEY_ID_NULL;
    psa_key_id_t kek = PSA_KEY_ID_NULL;
    uint8_t sealed[MN_CEK_SIZE + MN_MNR_TAG_SIZE];
    size_t len = 0;
    int ret = mn_x25519_import(eph_priv, &eph);

    if (ret != 0) {
        goto cleanup;
    }
    ret = -EIO;
    if (psa_export_public_key(eph, out->eph_pub, sizeof(out->eph_pub), &len) != PSA_SUCCESS ||
        len != sizeof(out->eph_pub)) {
        goto cleanup;
    }
    ret = derive_kek(eph, recipient_pub, out->eph_pub, rec_id, host_id, &kek);
    if (ret != 0) {
        goto cleanup;
    }
    ret = -EIO;
    if (psa_aead_encrypt(kek, PSA_ALG_CCM, wrap_nonce, sizeof(wrap_nonce), host_id, 8, cek,
                         MN_CEK_SIZE, sealed, sizeof(sealed), &len) != PSA_SUCCESS ||
        len != sizeof(sealed)) {
        goto cleanup;
    }

    memcpy(out->host_id, host_id, sizeof(out->host_id));
    memcpy(out->wrapped_cek, sealed, MN_CEK_SIZE);
    memcpy(out->tag, &sealed[MN_CEK_SIZE], MN_MNR_TAG_SIZE);
    ret = 0;

cleanup:
    /* Destroying the ephemeral key is what makes the wrap one-way for the
     * puck: nothing on it can recompute this KEK. */
    psa_destroy_key(eph);
    psa_destroy_key(kek);
    mbedtls_platform_zeroize(sealed, sizeof(sealed));
    return ret;
}

int mn_unwrap_cek(psa_key_id_t recipient_priv, uint64_t rec_id, const struct mn_mnr_wrap *w,
                  uint8_t cek_out[MN_CEK_SIZE])
{
    psa_key_id_t kek = PSA_KEY_ID_NULL;
    uint8_t sealed[MN_CEK_SIZE + MN_MNR_TAG_SIZE];
    size_t len = 0;
    int ret = derive_kek(recipient_priv, w->eph_pub, w->eph_pub, rec_id, w->host_id, &kek);

    if (ret != 0) {
        goto cleanup;
    }

    memcpy(sealed, w->wrapped_cek, MN_CEK_SIZE);
    memcpy(&sealed[MN_CEK_SIZE], w->tag, MN_MNR_TAG_SIZE);
    const psa_status_t st =
        psa_aead_decrypt(kek, PSA_ALG_CCM, wrap_nonce, sizeof(wrap_nonce), w->host_id,
                         sizeof(w->host_id), sealed, sizeof(sealed), cek_out, MN_CEK_SIZE, &len);

    if (st == PSA_ERROR_INVALID_SIGNATURE) {
        ret = -EBADMSG;
    } else if (st != PSA_SUCCESS || len != MN_CEK_SIZE) {
        ret = -EIO;
    }

cleanup:
    psa_destroy_key(kek);
    if (ret != 0) {
        mbedtls_platform_zeroize(cek_out, MN_CEK_SIZE);
    }
    return ret;
}
