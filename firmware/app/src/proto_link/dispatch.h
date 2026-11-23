/*
 * The puck's side of the control protocol (docs/protocol.md sections 3-5):
 * one link's session state, and an answer for every request. Transport-free:
 * bytes come in through mn_link_rx(), responses go out through a callback,
 * so the same code serves USB CDC, L2CAP and the native_sim UART.
 *
 * Until pairing lands (stage 1 PR 5), the paired hosts are a fixed list the
 * caller provides.
 */
#ifndef MICNOTE_PROTO_LINK_DISPATCH_H_
#define MICNOTE_PROTO_LINK_DISPATCH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "framing.h"
#include "micnote.pb.h"

#define MN_MAX_HOSTS 4

/* Sends bytes on the link; returns 0 or a negative errno. */
typedef int (*mn_link_send_fn)(const uint8_t *bytes, size_t len, void *ctx);

struct mn_link_config {
    /* Where recstore keeps recordings. */
    const char *rec_dir;
    micnote_v1_DeviceInfo device_info;
    /* Paired hosts' host_ids. */
    uint8_t host_ids[MN_MAX_HOSTS][8];
    size_t n_hosts;
    /* Fills the battery, charge, firmware and clock fields; optional. The
     * dispatcher fills the storage and state fields. */
    void (*fill_status)(micnote_v1_DeviceStatus *status);
    /* Called with SetTime's values; optional. */
    void (*set_time)(uint64_t unix_ms, int32_t tz_offset_min);
};

struct mn_link {
    const struct mn_link_config *cfg;
    mn_link_send_fn send;
    void *send_ctx;
    struct mn_framer framer;
    /* True once Hello from a paired host succeeded. */
    bool session;
};

void mn_link_init(struct mn_link *l, const struct mn_link_config *cfg, mn_link_send_fn send,
                  void *send_ctx);

/* Feeds received bytes and answers every complete request. Returns 0, or
 * the framer's sticky negative errno once the stream has lost sync: the
 * caller should drop the link and call mn_link_init() for the next one. */
int mn_link_rx(struct mn_link *l, const uint8_t *data, size_t len);

#endif /* MICNOTE_PROTO_LINK_DISPATCH_H_ */
