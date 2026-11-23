/*
 * The Micnote GATT service (stage 1 §4.7):
 *
 *   ctrl    write, write w/o response, indicate   requests from the host
 *   data    notify                                responses to the host
 *   psm     read                                  the L2CAP PSM (u16 LE)
 *   status  read, notify                          mn_ble_status (6 bytes)
 *
 * ctrl + data are the fallback transport for hosts that can't open an L2CAP
 * channel: the same framed byte stream as everywhere (protocol section 2),
 * requests written to ctrl, every response sent as data notifications.
 * ctrl's indicate property is reserved (no message uses it in v1).
 * Everything needs an encrypted link.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "ble_internal.h"

static const struct bt_uuid_128 uuid_svc = BT_UUID_INIT_128(MN_BLE_UUID_BYTES(0x00));
static const struct bt_uuid_128 uuid_ctrl = BT_UUID_INIT_128(MN_BLE_UUID_BYTES(0x01));
static const struct bt_uuid_128 uuid_data = BT_UUID_INIT_128(MN_BLE_UUID_BYTES(0x02));
static const struct bt_uuid_128 uuid_psm = BT_UUID_INIT_128(MN_BLE_UUID_BYTES(0x03));
static const struct bt_uuid_128 uuid_status = BT_UUID_INIT_128(MN_BLE_UUID_BYTES(0x04));

static uint8_t status_value[MN_BLE_STATUS_LEN];

static ssize_t write_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                          uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (conn != mn_ble_conn()) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    mn_ble_rx(MN_BLE_GATT, buf, len);
    return len;
}

static ssize_t read_psm(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                        uint16_t len, uint16_t offset)
{
    uint8_t value[2];

    sys_put_le16(mn_l2cap_psm(), value);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, status_value, sizeof(status_value));
}

BT_GATT_SERVICE_DEFINE(mn_svc, BT_GATT_PRIMARY_SERVICE(&uuid_svc),
                       /* attrs[1..3]: ctrl */
                       BT_GATT_CHARACTERISTIC(&uuid_ctrl.uuid,
                                              BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP |
                                                  BT_GATT_CHRC_INDICATE,
                                              BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_ctrl, NULL),
                       BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
                       /* attrs[4..6]: data */
                       BT_GATT_CHARACTERISTIC(&uuid_data.uuid, BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_NONE, NULL, NULL, NULL),
                       BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
                       /* attrs[7..8]: psm */
                       BT_GATT_CHARACTERISTIC(&uuid_psm.uuid, BT_GATT_CHRC_READ,
                                              BT_GATT_PERM_READ_ENCRYPT, read_psm, NULL, NULL),
                       /* attrs[9..11]: status */
                       BT_GATT_CHARACTERISTIC(&uuid_status.uuid,
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ_ENCRYPT, read_status, NULL, NULL),
                       BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT), );

#define ATTR_DATA (&mn_svc.attrs[5])
#define ATTR_STATUS (&mn_svc.attrs[10])

int mn_gatt_send(struct bt_conn *conn, const uint8_t *data, size_t len)
{
    const size_t chunk = bt_gatt_get_mtu(conn) - 3;
    int stalls = 0;

    while (len > 0) {
        const size_t n = MIN(len, chunk);
        const int ret = bt_gatt_notify(conn, ATTR_DATA, data, (uint16_t)n);

        if (ret == -ENOMEM) {
            /* Out of ATT buffers: wait for the controller to drain them. */
            if (++stalls > 10000) {
                return -ETIMEDOUT;
            }
            k_msleep(1);
            continue;
        }
        if (ret < 0) {
            return ret; /* not subscribed, or the link is gone */
        }
        stalls = 0;
        data += n;
        len -= n;
    }
    return 0;
}

void mn_gatt_status_value(uint8_t out[MN_BLE_STATUS_LEN])
{
    memcpy(out, status_value, sizeof(status_value));
}

void mn_gatt_status_notify(const uint8_t status[MN_BLE_STATUS_LEN])
{
    memcpy(status_value, status, sizeof(status_value));
    struct bt_conn *conn = mn_ble_conn();

    if (conn != NULL) {
        (void)bt_gatt_notify(conn, ATTR_STATUS, status_value, sizeof(status_value));
    }
}
