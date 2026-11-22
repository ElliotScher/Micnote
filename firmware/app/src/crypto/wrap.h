/*
 * Content-key wrapping (stage 1 section 4.5): each recording's CEK is
 * wrapped once per recipient (a paired host, or the device fallback key)
 * with its own ephemeral X25519 key:
 *
 *   ss  = X25519(eph_priv, recipient_pub)
 *   KEK = HKDF-SHA256(ikm = ss, salt = rec_id(8, LE) | eph_pub,
 *                     info = "micnote/v1/kek" | host_id)
 *   wrapped_cek | tag = AES-256-CCM(KEK, nonce = 0^13, aad = host_id, CEK)
 *
 * Randomness is passed in (eph_priv, cek), never generated here: the caller
 * draws it from psa_generate_random(), and tests pass fixed bytes.
 * psa_crypto_init() must have succeeded first.
 */
#ifndef MICNOTE_CRYPTO_WRAP_H_
#define MICNOTE_CRYPTO_WRAP_H_

#include <stdint.h>

#include <psa/crypto.h>

#include "mnr.h"

#define MN_X25519_KEY_SIZE 32
#define MN_CEK_SIZE 32

/* Wraps cek for one recipient and fills every field of *out. Returns 0,
 * -EINVAL if recipient_pub is a low-order point (the shared secret would be
 * all zeros), or -EIO on any other crypto failure. */
int mn_wrap_cek(const uint8_t eph_priv[MN_X25519_KEY_SIZE],
                const uint8_t recipient_pub[MN_X25519_KEY_SIZE], const uint8_t host_id[8],
                uint64_t rec_id, const uint8_t cek[MN_CEK_SIZE], struct mn_mnr_wrap *out);

/* Unwraps w with the recipient's private key, which must be an X25519 key
 * pair with usage PSA_KEY_USAGE_DERIVE and algorithm PSA_ALG_ECDH. Returns 0
 * with the CEK in cek_out, -EBADMSG if the wrap doesn't authenticate (wrong
 * key, rec_id or host_id, or tampered bytes), -EINVAL for a low-order
 * eph_pub, or -EIO. cek_out is zeroed on failure. */
int mn_unwrap_cek(psa_key_id_t recipient_priv, uint64_t rec_id, const struct mn_mnr_wrap *w,
                  uint8_t cek_out[MN_CEK_SIZE]);

/* Imports a raw X25519 private key for mn_unwrap_cek(), as a volatile key. */
int mn_x25519_import(const uint8_t priv[MN_X25519_KEY_SIZE], psa_key_id_t *out);

#endif /* MICNOTE_CRYPTO_WRAP_H_ */
