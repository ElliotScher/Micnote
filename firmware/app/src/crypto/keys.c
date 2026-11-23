#include "keys.h"

#include <errno.h>
#include <string.h>

static void attributes(psa_key_attributes_t *attr)
{
    psa_set_key_type(attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(attr, 255);
    /* ECDH for unwrapping, nothing else: not even export. */
    psa_set_key_usage_flags(attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(attr, PSA_ALG_ECDH);
    psa_set_key_lifetime(attr, PSA_KEY_LIFETIME_PERSISTENT);
    psa_set_key_id(attr, MN_KEY_ID_FALLBACK);
}

int mn_keys_fallback(struct mn_fallback_key *out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t id = MN_KEY_ID_FALLBACK;
    int created = 0;
    size_t len = 0;
    uint8_t hash[32];

    /* Exists already? Reading its attributes loads it from storage. */
    psa_status_t st = psa_get_key_attributes(MN_KEY_ID_FALLBACK, &attr);

    psa_reset_key_attributes(&attr);
    if (st == PSA_ERROR_INVALID_HANDLE || st == PSA_ERROR_DOES_NOT_EXIST) {
        attributes(&attr);
        st = psa_generate_key(&attr, &id);
        psa_reset_key_attributes(&attr);
        created = 1;
    }
    if (st != PSA_SUCCESS || id != MN_KEY_ID_FALLBACK) {
        return -EIO;
    }

    /* The public half of a key pair can always be exported, whatever its
     * usage flags. */
    if (psa_export_public_key(id, out->pub, sizeof(out->pub), &len) != PSA_SUCCESS ||
        len != sizeof(out->pub) ||
        psa_hash_compute(PSA_ALG_SHA_256, out->pub, sizeof(out->pub), hash, sizeof(hash), &len) !=
            PSA_SUCCESS) {
        return -EIO;
    }
    memcpy(out->host_id, hash, sizeof(out->host_id));
    out->id = id;
    return created;
}

int mn_keys_fallback_destroy(void)
{
    const psa_status_t st = psa_destroy_key(MN_KEY_ID_FALLBACK);

    return st == PSA_SUCCESS || st == PSA_ERROR_INVALID_HANDLE ? 0 : -EIO;
}
