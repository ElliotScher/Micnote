/*
 * Pairing and session crypto (docs/protocol.md sections 3 and 8), the
 * puck's side:
 *
 *   ss       = X25519(d, host_pub)            d: a fresh key per pairing
 *   link_key = HKDF-SHA256(ss, salt = host_id | device_id,
 *                          info = "micnote/v1/link", L = 32)
 *   sas      = HKDF-SHA256(ss, salt = n_h | n_d,
 *                          info = "micnote/v1/sas" | host_pub | d_pub, L = 4)
 *   symbol i = sas[i] mod 6
 *   proof    = HMAC-SHA256(link_key, "puck" or "host" | n_h | n_d)
 *
 * The link key only ever exists as a PSA HMAC key, never as bytes the
 * firmware handles. psa_crypto_init() must have succeeded first.
 */
#ifndef MICNOTE_CRYPTO_PAIRING_H_
#define MICNOTE_CRYPTO_PAIRING_H_

#include <stdint.h>

#include <psa/crypto.h>

#include "wrap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MN_PAIR_NONCE_SIZE 16
#define MN_PAIR_PROOF_SIZE 32
#define MN_SAS_SYMBOLS 4
#define MN_SAS_COLORS 6

/* SAS colours, by symbol value. */
enum mn_sas_color {
    MN_SAS_RED,
    MN_SAS_GREEN,
    MN_SAS_BLUE,
    MN_SAS_YELLOW,
    MN_SAS_CYAN,
    MN_SAS_MAGENTA,
};

/* The proof labels. */
#define MN_PROOF_PUCK "puck"
#define MN_PROOF_HOST "host"

/* A fresh volatile X25519 key pair for one pairing, and its public half.
 * Returns 0 or -EIO. */
int mn_pair_key_generate(psa_key_id_t *d, uint8_t d_pub[MN_X25519_KEY_SIZE]);

/*
 * Derives the link key and the SAS from the pairing exchange. The link key
 * comes out as a volatile HMAC-SHA256 key that can also be copied
 * (psa_copy_key) to persistent storage once the user confirms. Returns 0;
 * -EINVAL for a low-order host_pub; or -EIO.
 */
int mn_pair_derive(psa_key_id_t d, const uint8_t d_pub[MN_X25519_KEY_SIZE],
                   const uint8_t host_pub[MN_X25519_KEY_SIZE], const uint8_t host_id[8],
                   const uint8_t device_id[8], const uint8_t n_h[MN_PAIR_NONCE_SIZE],
                   const uint8_t n_d[MN_PAIR_NONCE_SIZE], psa_key_id_t *link_key,
                   uint8_t sas[MN_SAS_SYMBOLS]);

/* proof = HMAC-SHA256(link_key, label | n_h | n_d), label MN_PROOF_PUCK or
 * MN_PROOF_HOST. Returns 0 or -EIO. */
int mn_pair_proof(psa_key_id_t link_key, const char *label, const uint8_t n_h[MN_PAIR_NONCE_SIZE],
                  const uint8_t n_d[MN_PAIR_NONCE_SIZE], uint8_t proof[MN_PAIR_PROOF_SIZE]);

/* Checks a proof in constant time. Returns 0, -EBADMSG if it's wrong, or
 * -EIO. */
int mn_pair_proof_verify(psa_key_id_t link_key, const char *label,
                         const uint8_t n_h[MN_PAIR_NONCE_SIZE],
                         const uint8_t n_d[MN_PAIR_NONCE_SIZE],
                         const uint8_t proof[MN_PAIR_PROOF_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_CRYPTO_PAIRING_H_ */
