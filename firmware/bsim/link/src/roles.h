/* The three roles in this BabbleSim test, and the host side's helpers. */
#ifndef MICNOTE_BSIM_LINK_ROLES_H_
#define MICNOTE_BSIM_LINK_ROLES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>

#include "micnote.pb.h"

void puck_main(void);
void host_main(void);
void stranger_main(void);

/* The puck's fixed device_id, as in DeviceInfo. */
extern const uint8_t puck_device_id[8];

/* ---- host_link.c: a central speaking the protocol ----------------------- */

/* Scans for a puck advertising the Micnote service and connects. Returns the
 * connection (a ref), and *sync_pending from its advertising. */
struct bt_conn *hl_connect(bool *sync_pending);
void hl_disconnect(struct bt_conn *conn);

/* Asks for an encrypted link; returns 0 or the security error. */
int hl_encrypt(struct bt_conn *conn);

/* Reads a characteristic of the service by UUID suffix (1 ctrl ... 4
 * status); returns the length read, or a negative ATT error / errno. */
int hl_read(struct bt_conn *conn, uint8_t suffix, uint8_t *out, size_t cap);

enum hl_transport { HL_L2CAP, HL_GATT };

/* Opens the byte stream: the L2CAP channel on `psm`, or GATT (subscribe to
 * data, write to ctrl). */
int hl_open(struct bt_conn *conn, enum hl_transport t, uint16_t psm);

/* Sends a request (req_id is filled in) and waits up to timeout_ms for the
 * next frame answering it; returns 0 with *resp, or -ETIMEDOUT. */
int hl_call(micnote_v1_Frame *req, micnote_v1_Frame *resp, uint32_t timeout_ms);
/* The next frame answering the last request (for Fetch's Chunk stream). */
int hl_next(micnote_v1_Frame *resp, uint32_t timeout_ms);
/* Bytes received since the stream opened. */
size_t hl_rx_bytes(void);
/* The last connection parameters the puck asked for and got (units of
 * 1.25 ms), 0 if none yet. */
void hl_interval(uint16_t *interval, uint16_t *latency);
/* The L2CAP channel's negotiated send MTU, or 0. */
uint16_t hl_l2cap_tx_mtu(void);

#endif /* MICNOTE_BSIM_LINK_ROLES_H_ */
