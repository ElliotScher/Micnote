/* Shared between ble.c and the transports (l2cap.c, gatt_svc.c). */
#ifndef MICNOTE_BLE_BLE_INTERNAL_H_
#define MICNOTE_BLE_BLE_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>

#include "ble.h"

/* Bytes from a transport, in the stack's context: queued for the link
 * thread. The first transport to deliver bytes on a connection owns it. */
void mn_ble_rx(enum mn_ble_transport t, const uint8_t *data, size_t len);

/* The connection the transports serve, or NULL. */
struct bt_conn *mn_ble_conn(void);

int mn_l2cap_init(void);
uint16_t mn_l2cap_psm(void);
/* Sends bytes as SDUs of up to the channel's MTU; blocks for buffers. Link
 * thread only. */
int mn_l2cap_send(const uint8_t *data, size_t len);
/* The channel connected (true) or went away (false). */
void mn_ble_l2cap_state(bool up);

/* Sends bytes as `data` notifications of up to ATT MTU - 3. Link thread. */
int mn_gatt_send(struct bt_conn *conn, const uint8_t *data, size_t len);
void mn_gatt_status_notify(const uint8_t status[MN_BLE_STATUS_LEN]);
void mn_gatt_status_value(uint8_t out[MN_BLE_STATUS_LEN]);

#endif /* MICNOTE_BLE_BLE_INTERNAL_H_ */
