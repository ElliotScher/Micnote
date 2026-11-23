/*
 * The device fallback key (stage 1 §4.5): an X25519 key pair the puck makes
 * on its first boot and keeps as a PSA persistent key. Recordings made before
 * any host is paired are wrapped to it; at the first pairing the puck unwraps
 * them with it and wraps them to the new host (mn_recstore_rewrap()).
 *
 * The private key never leaves PSA: callers get its key ID (for
 * mn_unwrap_cek()) and the public half. Where PSA keeps it depends on the
 * build: Zephyr's secure storage (ITS over settings/NVS) on native_sim and,
 * for now, on the boards. Hardening on the nRF5340 (KMU, or the HUK-derived
 * encryption of NCS's trusted storage) is stage 7 work; this interface
 * doesn't change with it.
 *
 * psa_crypto_init() (and, with settings-backed ITS, settings_subsys_init())
 * must have run first.
 */
#ifndef MICNOTE_CRYPTO_KEYS_H_
#define MICNOTE_CRYPTO_KEYS_H_

#include <stdint.h>

#include <psa/crypto.h>

#include "wrap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* In Zephyr's application range (zephyr/psa/key_ids.h). */
#define MN_KEY_ID_FALLBACK ((psa_key_id_t)0x30000001)

struct mn_fallback_key {
    psa_key_id_t id;
    uint8_t pub[MN_X25519_KEY_SIZE];
    /* SHA-256(pub)[:8], the host_id its wraps carry. */
    uint8_t host_id[8];
};

/* Opens the fallback key, generating it if this is the first boot. Returns
 * 1 if it was just generated, 0 if it already existed, or -EIO. */
int mn_keys_fallback(struct mn_fallback_key *out);

/* Destroys it (factory reset). The next mn_keys_fallback() makes a new one,
 * so recordings still wrapped to the old one can never be read. Returns 0,
 * also when there was none, or -EIO. */
int mn_keys_fallback_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_CRYPTO_KEYS_H_ */
