/*
 * The puck's BLE side (stage 1 §4.7, docs/protocol.md section 9): one
 * connection at a time, carrying the control protocol (proto_link/dispatch)
 * over an L2CAP connection-oriented channel, or over the GATT `ctrl`/`data`
 * characteristics when the host can't open one.
 *
 * Security:
 * - LE Secure Connections only (legacy pairing is refused), Just Works,
 *   bonded. The SAS of protocol section 8 is what defeats an active MITM.
 * - New bonds only while the PAIRING window is open (link->pairing_open);
 *   bonded hosts reconnect and re-encrypt at any time.
 * - Every characteristic and the L2CAP channel need an encrypted link.
 * - A resolvable private address, rotated every 15 min (CONFIG_BT_RPA_TIMEOUT).
 *
 * Threads: the Bluetooth stack's callbacks only queue bytes and events; the
 * protocol runs on the link thread (§4.1, priority 7), so a slow flash read
 * during a Fetch never stalls the stack, and the stack never waits on us.
 *
 * Settings (bonds) and PSA must be initialized first, and hosts.c loaded.
 */
#ifndef MICNOTE_BLE_BLE_H_
#define MICNOTE_BLE_BLE_H_

#include <stdbool.h>
#include <stdint.h>

#include "adv.h"
#include "dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MN_BLE_LINK_PRIORITY 7
/* L2CAP SDU size the puck receives and sends (§4.7, to be tuned on boards). */
#define MN_BLE_L2CAP_MTU 2048
/* After this long without traffic, back to the idle connection interval. */
#define MN_BLE_IDLE_MS 5000

enum mn_ble_transport {
    MN_BLE_NONE,
    MN_BLE_L2CAP,
    MN_BLE_GATT,
};

/* The compact status characteristic (6 bytes, little-endian). */
struct mn_ble_status {
    uint8_t battery_pct;
    uint8_t state; /* micnote_v1_DeviceState */
    uint16_t recording_count;
    uint8_t storage_used_pct;
    uint8_t flags; /* MN_BLE_STATUS_* */
};
#define MN_BLE_STATUS_SYNC_PENDING 0x01
#define MN_BLE_STATUS_CHARGING 0x02
#define MN_BLE_STATUS_LEN 6

/* Enables Bluetooth (loading bonds from settings), registers the service and
 * the L2CAP server, and starts the link thread. Once, at boot. */
int mn_ble_init(const struct mn_link_config *link);

/* Starts advertising in `mode` (stopping what was running), or stops it for
 * MN_ADV_OFF. It ends by itself after the mode's duration, and when a host
 * connects. */
int mn_ble_advertise(enum mn_adv_mode mode, bool sync_pending);

/* The button was pressed: completes a PairConfirm waiting for it. */
void mn_ble_pair_press(void);

/* The PAIRING window closed: cancels a pairing on the link. */
void mn_ble_pair_cancel(void);

bool mn_ble_connected(void);
enum mn_ble_transport mn_ble_transport(void);
/* True while a PairConfirm waits for the button. */
bool mn_ble_confirm_pending(void);
/* The L2CAP PSM the `psm` characteristic serves. */
uint16_t mn_ble_psm(void);

/* Updates the status characteristic, notifying a subscribed host. */
void mn_ble_status_set(const struct mn_ble_status *s);
void mn_ble_status_encode(const struct mn_ble_status *s, uint8_t out[MN_BLE_STATUS_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_BLE_BLE_H_ */
