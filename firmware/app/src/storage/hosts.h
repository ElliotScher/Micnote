/*
 * The paired hosts (stage 1 §4.6, docs/protocol.md section 8): up to four
 * slots, each a host's ID, X25519 public key (recordings are wrapped to it),
 * name, and link key (the session challenge-response, protocol section 3).
 *
 * The slot records live in settings under "mn/hosts/<slot>". Link keys are
 * PSA persistent HMAC keys, MN_KEY_ID_LINK(slot), so their bytes never sit in
 * settings. One table for the whole device; the functions aren't
 * thread-safe (the link thread is their only user).
 *
 * settings_subsys_init() and psa_crypto_init() must have run first.
 */
#ifndef MICNOTE_STORAGE_HOSTS_H_
#define MICNOTE_STORAGE_HOSTS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <psa/crypto.h>

#include "recstore.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MN_HOSTS_MAX MN_MNR_MAX_WRAPS
#define MN_HOST_NAME_MAX 32 /* with the NUL, as HostInfo.name */
/* After the fallback key (crypto/keys.h) in the application range. */
#define MN_KEY_ID_LINK(slot) ((psa_key_id_t)(0x30000010 + (slot)))

struct mn_host {
    bool used;
    uint8_t host_id[8];
    uint8_t pub[32];
    char name[MN_HOST_NAME_MAX];
};

/* Loads the table from settings. Returns 0 or a negative errno. */
int mn_hosts_init(void);

/* The slot, or NULL if it's empty or out of range. */
const struct mn_host *mn_hosts_get(int slot);

size_t mn_hosts_count(void);

/* The slot holding host_id, or -ENOENT. */
int mn_hosts_find(const uint8_t host_id[8]);

/*
 * Stores a host: into its own slot if it's already paired, else the first
 * free one. link_key (a volatile key with PSA_KEY_USAGE_COPY, from
 * mn_pair_derive()) is copied to the slot's persistent key; the caller still
 * owns, and should destroy, the volatile one. Returns the slot, -ENOSPC when
 * all are taken, or a negative errno.
 */
int mn_hosts_add(const uint8_t host_id[8], const uint8_t pub[32], const char *name,
                 psa_key_id_t link_key);

/* Frees host_id's slot and destroys its link key. 0 or -ENOENT. */
int mn_hosts_remove(const uint8_t host_id[8]);

/* The slot's link key, for mn_pair_proof(). */
static inline psa_key_id_t mn_hosts_link_key(int slot)
{
    return MN_KEY_ID_LINK(slot);
}

/* Every paired host as a recording recipient; returns how many. */
size_t mn_hosts_recipients(struct mn_rec_recipient out[MN_HOSTS_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_STORAGE_HOSTS_H_ */
