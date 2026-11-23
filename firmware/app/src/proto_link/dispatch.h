/*
 * The puck's side of the control protocol (docs/protocol.md sections 3-5
 * and 8): one link's session state, and an answer for every request.
 * Transport-free: bytes come in through mn_link_rx(), responses go out
 * through a callback, so the same code serves USB CDC, L2CAP and the
 * native_sim UART.
 *
 * Hosts come from the host-slot store (storage/hosts.h): mn_hosts_init()
 * must have run. Pairing needs the PAIRING window (cfg->pairing_open) and a
 * button press, which the caller reports with mn_link_pair_press().
 */
#ifndef MICNOTE_PROTO_LINK_DISPATCH_H_
#define MICNOTE_PROTO_LINK_DISPATCH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <psa/crypto.h>

#include "framing.h"
#include "keys.h"
#include "micnote.pb.h"
#include "pairing.h"

/* Sends bytes on the link; returns 0 or a negative errno. */
typedef int (*mn_link_send_fn)(const uint8_t *bytes, size_t len, void *ctx);

struct mn_link_config {
    /* Where recstore keeps recordings. */
    const char *rec_dir;
    /* device_id (8 bytes) goes into every link key. */
    micnote_v1_DeviceInfo device_info;
    /* The device fallback key: at each pairing, recordings still wrapped
     * to it move to the new host. Optional. */
    const struct mn_fallback_key *fallback;
    /* Fills the battery, charge, firmware and clock fields; optional. The
     * dispatcher fills the storage and state fields. */
    void (*fill_status)(micnote_v1_DeviceStatus *status);
    /* Called with SetTime's values; optional. */
    void (*set_time)(uint64_t unix_ms, int32_t tz_offset_min);
    /* True while the PAIRING window is open; optional (never, if NULL). */
    bool (*pairing_open)(void);
    /* The SAS colours to play while a pairing waits for the user, then NULL
     * once it's over (done or cancelled). Optional. */
    void (*show_sas)(const uint8_t sas[MN_SAS_SYMBOLS]);
};

enum mn_link_state {
    MN_LINK_NONE,    /* waiting for Hello */
    MN_LINK_HELLO,   /* HelloAck sent: Auth, or PairBegin in PAIRING */
    MN_LINK_PAIRING, /* PairChallenge sent: PairConfirm, then the button */
    MN_LINK_SESSION, /* authenticated */
};

struct mn_link {
    const struct mn_link_config *cfg;
    mn_link_send_fn send;
    void *send_ctx;
    struct mn_framer framer;
    enum mn_link_state state;
    /* From this link's Hello / HelloAck. */
    uint8_t host_id[8];
    uint8_t n_h[MN_PAIR_NONCE_SIZE];
    uint8_t n_d[MN_PAIR_NONCE_SIZE];
    /* The host's slot if it's paired, else -1. */
    int slot;
    /* A pairing in progress (MN_LINK_PAIRING). */
    psa_key_id_t pair_key;
    psa_key_id_t link_key;
    uint8_t host_pub[32];
    char host_name[32];
    /* PairConfirm{true} came and waits for the button. */
    bool confirm_pending;
    uint32_t confirm_req_id;
};

void mn_link_init(struct mn_link *l, const struct mn_link_config *cfg, mn_link_send_fn send,
                  void *send_ctx);

/* Feeds received bytes and answers every complete request. Returns 0, or
 * the framer's sticky negative errno once the stream has lost sync: the
 * caller should drop the link (mn_link_close()) and call mn_link_init() for
 * the next one. */
int mn_link_rx(struct mn_link *l, const uint8_t *data, size_t len);

/* The puck's button was pressed: completes a PairConfirm that waits for it
 * (stores the host, moves fallback recordings to it, answers PairDone).
 * Returns 0, -EALREADY if no confirm is waiting, or a send error. */
int mn_link_pair_press(struct mn_link *l);

/* The PAIRING window closed: a pairing on this link is cancelled, and a
 * waiting PairConfirm gets Error{BAD_STATE}. Returns 0 or a send error. */
int mn_link_pair_cancel(struct mn_link *l);

/* Call when the link goes away: destroys a pairing's keys. */
void mn_link_close(struct mn_link *l);

#endif /* MICNOTE_PROTO_LINK_DISPATCH_H_ */
