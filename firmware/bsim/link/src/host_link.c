/*
 * The test host's BLE plumbing: scan for the puck, connect, encrypt, read a
 * characteristic, and carry framed requests and responses over L2CAP or the
 * GATT fallback. A test-only stand-in for core's SimpleBLE transport.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>

#include "adv.h"
#include "babblekit/testcase.h"
#include "ble.h"
#include "framing.h"
#include "roles.h"

/* ---- connect ------------------------------------------------------------- */

static K_SEM_DEFINE(found, 0, 1);
static K_SEM_DEFINE(conn_done, 0, 1);
static K_SEM_DEFINE(sec_done, 0, 1);
static bt_addr_le_t found_addr;
static bool found_sync_pending;
static uint8_t conn_err;
static int sec_err;
static struct bt_conn *conn_now;

static void on_scan(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad)
{
    bool pending;

    ARG_UNUSED(rssi);
    if (type != BT_GAP_ADV_TYPE_ADV_IND || !mn_adv_parse(ad->data, ad->len, &pending)) {
        return;
    }
    if (bt_le_scan_stop() == 0) {
        bt_addr_le_copy(&found_addr, addr);
        found_sync_pending = pending;
        k_sem_give(&found);
    }
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (conn == conn_now) {
        conn_err = err;
        k_sem_give(&conn_done);
    }
}

static void on_security(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    if (conn == conn_now) {
        sec_err = err != BT_SECURITY_ERR_SUCCESS ? (int)err : (level < BT_SECURITY_L2 ? -1 : 0);
        k_sem_give(&sec_done);
    }
}

static uint16_t last_interval;
static uint16_t last_latency;

static void on_params(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
    ARG_UNUSED(timeout);
    if (conn == conn_now) {
        last_interval = interval;
        last_latency = latency;
        TEST_PRINT("host: connection interval %u.%02u ms, latency %u", interval * 125 / 100,
                   interval * 125 % 100, latency);
    }
}

static void on_phy(struct bt_conn *conn, struct bt_conn_le_phy_info *info)
{
    if (conn == conn_now) {
        TEST_PRINT("host: PHY tx %u rx %u", info->tx_phy, info->rx_phy);
    }
}

static void on_data_len(struct bt_conn *conn, struct bt_conn_le_data_len_info *info)
{
    if (conn == conn_now) {
        TEST_PRINT("host: data length tx %u rx %u", info->tx_max_len, info->rx_max_len);
    }
}

BT_CONN_CB_DEFINE(host_callbacks) = {
    .connected = on_connected,
    .security_changed = on_security,
    .le_param_updated = on_params,
    .le_phy_updated = on_phy,
    .le_data_len_updated = on_data_len,
};

struct bt_conn *hl_connect(bool *sync_pending)
{
    k_sem_reset(&found);
    TEST_ASSERT(bt_le_scan_start(BT_LE_SCAN_PASSIVE, on_scan) == 0, "scan");
    TEST_ASSERT(k_sem_take(&found, K_SECONDS(30)) == 0, "no puck advertising");
    *sync_pending = found_sync_pending;

    k_sem_reset(&conn_done);
    const int err =
        bt_conn_le_create(&found_addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &conn_now);

    TEST_ASSERT(err == 0, "connect: %d", err);
    TEST_ASSERT(k_sem_take(&conn_done, K_SECONDS(10)) == 0 && conn_err == 0, "connect failed");
    return conn_now;
}

void hl_disconnect(struct bt_conn *conn)
{
    (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    for (int i = 0; i < 100; i++) {
        struct bt_conn_info info;

        if (bt_conn_get_info(conn, &info) != 0 || info.state == BT_CONN_STATE_DISCONNECTED) {
            break;
        }
        k_msleep(50);
    }
    bt_conn_unref(conn);
    conn_now = NULL;
    k_msleep(200);
}

int hl_encrypt(struct bt_conn *conn)
{
    k_sem_reset(&sec_done);
    const int err = bt_conn_set_security(conn, BT_SECURITY_L2);

    if (err != 0) {
        return err;
    }
    if (k_sem_take(&sec_done, K_SECONDS(10)) != 0) {
        return -ETIMEDOUT;
    }
    return sec_err;
}

/* ---- read by UUID -------------------------------------------------------- */

static K_SEM_DEFINE(read_done, 0, 1);
static uint8_t *read_out;
static size_t read_cap;
static int read_result;

/* Read-by-UUID keeps reading the rest of the handle range after a match
 * and ends with ATTRIBUTE_NOT_FOUND, so stop at the first value. */
static uint8_t on_read(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
                       const void *data, uint16_t length)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(params);
    if (err != 0) {
        read_result = -(int)err;
    } else if (data != NULL) {
        read_result = MIN(length, read_cap);
        memcpy(read_out, data, read_result);
    } else {
        read_result = -ENOENT;
    }
    k_sem_give(&read_done);
    return BT_GATT_ITER_STOP;
}

int hl_read(struct bt_conn *conn, uint8_t suffix, uint8_t *out, size_t cap)
{
    static struct bt_uuid_128 uuid;
    static struct bt_gatt_read_params params;
    const uint8_t bytes[16] = {MN_BLE_UUID_BYTES(0)};

    uuid.uuid.type = BT_UUID_TYPE_128;
    memcpy(uuid.val, bytes, 16);
    uuid.val[10] = suffix;
    memset(&params, 0, sizeof(params));
    params.func = on_read;
    params.handle_count = 0;
    params.by_uuid.uuid = &uuid.uuid;
    params.by_uuid.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    params.by_uuid.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    read_out = out;
    read_cap = cap;
    read_result = 0;
    k_sem_reset(&read_done);
    const int err = bt_gatt_read(conn, &params);

    if (err != 0) {
        return err;
    }
    TEST_ASSERT(k_sem_take(&read_done, K_SECONDS(10)) == 0, "read timed out");
    return read_result;
}

/* ---- the byte stream ----------------------------------------------------- */

RING_BUF_DECLARE(hl_rx_ring, 16 * 1024);
static struct k_spinlock rx_lock;
static K_SEM_DEFINE(rx_sem, 0, 1);
static size_t rx_total;
static struct mn_framer framer;
static enum hl_transport mode;
static struct bt_conn *stream_conn;
static uint32_t next_req_id = 1;
static uint32_t last_req_id;

static void rx_bytes(const uint8_t *data, size_t len)
{
    k_spinlock_key_t key = k_spin_lock(&rx_lock);
    const uint32_t put = ring_buf_put(&hl_rx_ring, data, len);

    rx_total += len;
    k_spin_unlock(&rx_lock, key);
    TEST_ASSERT(put == len, "host receive ring overflowed");
    k_sem_give(&rx_sem);
}

/* L2CAP */
NET_BUF_POOL_DEFINE(host_pool, 2, BT_L2CAP_SDU_BUF_SIZE(MN_BLE_L2CAP_MTU),
                    CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);
static struct bt_l2cap_le_chan chan;
static K_SEM_DEFINE(chan_up, 0, 1);

/* Like a good phone: a 2048-byte MTU and a deep credit window, granted
 * back segment by segment. (Zephyr's default receive path caps the MTU at
 * one PDU and starts at a single credit, which would make this host, not
 * the puck, the bottleneck.) */
#define HOST_CREDITS 32

static void chan_seg_recv(struct bt_l2cap_chan *c, size_t sdu_len, off_t seg_offset,
                          struct net_buf_simple *seg)
{
    ARG_UNUSED(sdu_len);
    ARG_UNUSED(seg_offset);
    rx_bytes(seg->data, seg->len);
    (void)bt_l2cap_chan_give_credits(c, 1);
}

static void chan_connected(struct bt_l2cap_chan *c)
{
    (void)bt_l2cap_chan_give_credits(c, HOST_CREDITS);
    k_sem_give(&chan_up);
}

static void chan_disconnected(struct bt_l2cap_chan *c)
{
    ARG_UNUSED(c);
}

static const struct bt_l2cap_chan_ops chan_ops = {
    .seg_recv = chan_seg_recv,
    .connected = chan_connected,
    .disconnected = chan_disconnected,
};

/* GATT */
static struct bt_gatt_subscribe_params sub;
static struct bt_gatt_discover_params disc;
static uint16_t ctrl_handle;
static uint16_t data_handle;
static K_SEM_DEFINE(disc_done, 0, 1);
static K_SEM_DEFINE(sub_done, 0, 1);

static uint8_t on_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                         const void *data, uint16_t length)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(params);
    if (data != NULL) {
        rx_bytes(data, length);
    }
    return BT_GATT_ITER_CONTINUE;
}

static void on_subscribed(struct bt_conn *conn, uint8_t err, struct bt_gatt_subscribe_params *p)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(p);
    TEST_ASSERT(err == 0, "subscribe: %u", err);
    k_sem_give(&sub_done);
}

static uint8_t on_discover(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(params);
    if (attr == NULL) {
        k_sem_give(&disc_done);
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_chrc *chrc = attr->user_data;
    const uint8_t base[16] = {MN_BLE_UUID_BYTES(0)};
    const struct bt_uuid_128 *u = (const struct bt_uuid_128 *)chrc->uuid;

    if (chrc->uuid->type == BT_UUID_TYPE_128 && memcmp(u->val, base, 10) == 0 &&
        memcmp(&u->val[11], &base[11], 5) == 0) {
        if (u->val[10] == 0x01) {
            ctrl_handle = chrc->value_handle;
        } else if (u->val[10] == 0x02) {
            data_handle = chrc->value_handle;
        }
    }
    return BT_GATT_ITER_CONTINUE;
}

int hl_open(struct bt_conn *conn, enum hl_transport t, uint16_t psm)
{
    k_spinlock_key_t key = k_spin_lock(&rx_lock);

    ring_buf_reset(&hl_rx_ring);
    rx_total = 0;
    k_spin_unlock(&rx_lock, key);
    mn_framer_init(&framer);
    mode = t;
    stream_conn = conn;

    if (t == HL_L2CAP) {
        memset(&chan, 0, sizeof(chan));
        chan.chan.ops = &chan_ops;
        chan.rx.mtu = MN_BLE_L2CAP_MTU;
        chan.rx.mps = BT_L2CAP_RX_MTU;
        k_sem_reset(&chan_up);
        const int err = bt_l2cap_chan_connect(conn, &chan.chan, psm);

        if (err != 0) {
            return err;
        }
        return k_sem_take(&chan_up, K_SECONDS(10)) == 0 ? 0 : -ETIMEDOUT;
    }

    /* GATT: find ctrl and data, then subscribe to data (its CCC follows its
     * value handle). */
    ctrl_handle = 0;
    data_handle = 0;
    memset(&disc, 0, sizeof(disc));
    disc.func = on_discover;
    disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    k_sem_reset(&disc_done);
    TEST_ASSERT(bt_gatt_discover(conn, &disc) == 0, "discover");
    TEST_ASSERT(k_sem_take(&disc_done, K_SECONDS(10)) == 0, "discover timed out");
    TEST_ASSERT(ctrl_handle != 0 && data_handle != 0, "ctrl/data not found");
    memset(&sub, 0, sizeof(sub));
    sub.notify = on_notify;
    sub.subscribe = on_subscribed;
    sub.value = BT_GATT_CCC_NOTIFY;
    sub.value_handle = data_handle;
    sub.ccc_handle = data_handle + 1;
    k_sem_reset(&sub_done);
    TEST_ASSERT(bt_gatt_subscribe(conn, &sub) == 0, "subscribe");
    TEST_ASSERT(k_sem_take(&sub_done, K_SECONDS(10)) == 0, "subscribe timed out");
    return 0;
}

static int send_bytes(const uint8_t *data, size_t len)
{
    while (len > 0) {
        if (mode == HL_L2CAP) {
            const size_t n = MIN(len, chan.tx.mtu);
            struct net_buf *buf = net_buf_alloc(&host_pool, K_SECONDS(5));

            TEST_ASSERT(buf != NULL, "host tx buffers");
            net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
            net_buf_add_mem(buf, data, n);
            if (bt_l2cap_chan_send(&chan.chan, buf) < 0) {
                net_buf_unref(buf);
                return -EIO;
            }
            data += n;
            len -= n;
        } else {
            const size_t n = MIN(len, bt_gatt_get_mtu(stream_conn) - 3);
            const int err =
                bt_gatt_write_without_response(stream_conn, ctrl_handle, data, (uint16_t)n, false);

            if (err == -ENOMEM) {
                k_msleep(1);
                continue;
            }
            if (err != 0) {
                return err;
            }
            data += n;
            len -= n;
        }
    }
    return 0;
}

int hl_next(micnote_v1_Frame *resp, uint32_t timeout_ms)
{
    static uint8_t buf[512];
    const int64_t deadline = k_uptime_get() + timeout_ms;

    for (;;) {
        const int got = mn_framer_next(&framer, resp);

        TEST_ASSERT(got >= 0, "framing: %d", got);
        if (got == 1) {
            if (resp->req_id == 0) {
                continue; /* unsolicited status */
            }
            TEST_ASSERT(resp->req_id == last_req_id, "response to req %u, want %u", resp->req_id,
                        last_req_id);
            return 0;
        }
        k_spinlock_key_t key = k_spin_lock(&rx_lock);
        const uint32_t n = ring_buf_peek(&hl_rx_ring, buf, sizeof(buf));

        k_spin_unlock(&rx_lock, key);
        if (n > 0) {
            const size_t took = mn_framer_push(&framer, buf, n);

            key = k_spin_lock(&rx_lock);
            ring_buf_get(&hl_rx_ring, NULL, took);
            k_spin_unlock(&rx_lock, key);
            continue;
        }
        const int64_t left = deadline - k_uptime_get();

        if (left <= 0 || k_sem_take(&rx_sem, K_MSEC(left)) != 0) {
            return -ETIMEDOUT;
        }
    }
}

int hl_call(micnote_v1_Frame *req, micnote_v1_Frame *resp, uint32_t timeout_ms)
{
    static uint8_t wire[MN_FRAME_MAX_WIRE];

    req->req_id = next_req_id++;
    last_req_id = req->req_id;
    const int n = mn_frame_encode(req, wire, sizeof(wire));

    TEST_ASSERT(n > 0, "encode: %d", n);
    const int err = send_bytes(wire, (size_t)n);

    if (err != 0) {
        return err;
    }
    return hl_next(resp, timeout_ms);
}

size_t hl_rx_bytes(void)
{
    return rx_total;
}

uint16_t hl_l2cap_tx_mtu(void)
{
    return mode == HL_L2CAP ? chan.tx.mtu : 0;
}

void hl_interval(uint16_t *interval, uint16_t *latency)
{
    *interval = last_interval;
    *latency = last_latency;
}
