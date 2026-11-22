/*
 * Chunk encryption (stage 1 section 4.5): AES-256-CCM under the recording's
 * CEK, with
 *   nonce = rec_id(8, LE) | seq(4, LE) | 0x00
 *   aad   = SHA-256(header core) | seq(4, LE)
 * so a chunk can't be moved to another recording or another position.
 * Output is ciphertext followed by the 16-byte tag, exactly as stored.
 * psa_crypto_init() must have succeeded first.
 */
#ifndef MICNOTE_CRYPTO_AEAD_H_
#define MICNOTE_CRYPTO_AEAD_H_

#include <stddef.h>
#include <stdint.h>

#include <psa/crypto.h>

#include "wrap.h"

#define MN_SHA256_SIZE 32

/* Imports a raw CEK as a volatile AES-256-CCM key. The caller zeroizes its
 * copy of the raw bytes; the key ID is destroyed when the recording closes. */
int mn_cek_import(const uint8_t cek[MN_CEK_SIZE], psa_key_id_t *out);

/* SHA-256 over the first MN_MNR_CORE_SIZE bytes of a written header. */
int mn_header_core_hash(const uint8_t *header, uint8_t out[MN_SHA256_SIZE]);

/* Seals pt (1 ..= MN_MNR_MAX_CT bytes) into out, which needs pt_len +
 * MN_MNR_TAG_SIZE bytes. Returns the bytes written, -EINVAL for a bad length,
 * -ENOSPC if cap is too small, or -EIO. */
int mn_chunk_seal(psa_key_id_t cek, uint64_t rec_id, uint32_t seq,
                  const uint8_t core_hash[MN_SHA256_SIZE], const uint8_t *pt, size_t pt_len,
                  uint8_t *out, size_t cap);

/* Opens ct (ciphertext then tag) into pt. Returns the plaintext length,
 * -EBADMSG if it doesn't authenticate, -EINVAL for a bad length, -ENOSPC if
 * cap is too small, or -EIO. pt is zeroed on failure. */
int mn_chunk_open(psa_key_id_t cek, uint64_t rec_id, uint32_t seq,
                  const uint8_t core_hash[MN_SHA256_SIZE], const uint8_t *ct, size_t ct_len,
                  uint8_t *pt, size_t cap);

#endif /* MICNOTE_CRYPTO_AEAD_H_ */
