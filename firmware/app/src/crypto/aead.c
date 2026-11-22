#include "aead.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include <mbedtls/platform_util.h>

#define NONCE_SIZE 13
#define AAD_SIZE (MN_SHA256_SIZE + 4)

int mn_cek_import(const uint8_t cek[MN_CEK_SIZE], psa_key_id_t *out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CCM);
    return psa_import_key(&attr, cek, MN_CEK_SIZE, out) == PSA_SUCCESS ? 0 : -EIO;
}

int mn_header_core_hash(const uint8_t *header, uint8_t out[MN_SHA256_SIZE])
{
    size_t len = 0;

    return psa_hash_compute(PSA_ALG_SHA_256, header, MN_MNR_CORE_SIZE, out, MN_SHA256_SIZE, &len) ==
                   PSA_SUCCESS
               ? 0
               : -EIO;
}

static void nonce_aad(uint64_t rec_id, uint32_t seq, const uint8_t core_hash[MN_SHA256_SIZE],
                      uint8_t nonce[NONCE_SIZE], uint8_t aad[AAD_SIZE])
{
    sys_put_le64(rec_id, nonce);
    sys_put_le32(seq, &nonce[8]);
    nonce[12] = 0x00;
    memcpy(aad, core_hash, MN_SHA256_SIZE);
    sys_put_le32(seq, &aad[MN_SHA256_SIZE]);
}

int mn_chunk_seal(psa_key_id_t cek, uint64_t rec_id, uint32_t seq,
                  const uint8_t core_hash[MN_SHA256_SIZE], const uint8_t *pt, size_t pt_len,
                  uint8_t *out, size_t cap)
{
    uint8_t nonce[NONCE_SIZE];
    uint8_t aad[AAD_SIZE];
    size_t len = 0;

    if (pt_len == 0 || pt_len > MN_MNR_MAX_CT) {
        return -EINVAL;
    }
    if (cap < pt_len + MN_MNR_TAG_SIZE) {
        return -ENOSPC;
    }
    nonce_aad(rec_id, seq, core_hash, nonce, aad);
    if (psa_aead_encrypt(cek, PSA_ALG_CCM, nonce, sizeof(nonce), aad, sizeof(aad), pt, pt_len, out,
                         cap, &len) != PSA_SUCCESS) {
        return -EIO;
    }
    return (int)len;
}

int mn_chunk_open(psa_key_id_t cek, uint64_t rec_id, uint32_t seq,
                  const uint8_t core_hash[MN_SHA256_SIZE], const uint8_t *ct, size_t ct_len,
                  uint8_t *pt, size_t cap)
{
    uint8_t nonce[NONCE_SIZE];
    uint8_t aad[AAD_SIZE];
    size_t len = 0;

    if (ct_len <= MN_MNR_TAG_SIZE || ct_len > MN_MNR_MAX_CT + MN_MNR_TAG_SIZE) {
        return -EINVAL;
    }
    if (cap < ct_len - MN_MNR_TAG_SIZE) {
        return -ENOSPC;
    }
    nonce_aad(rec_id, seq, core_hash, nonce, aad);
    const psa_status_t st = psa_aead_decrypt(cek, PSA_ALG_CCM, nonce, sizeof(nonce), aad,
                                             sizeof(aad), ct, ct_len, pt, cap, &len);

    if (st != PSA_SUCCESS) {
        mbedtls_platform_zeroize(pt, cap);
        return st == PSA_ERROR_INVALID_SIGNATURE ? -EBADMSG : -EIO;
    }
    return (int)len;
}
