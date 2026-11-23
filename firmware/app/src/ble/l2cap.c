/* The L2CAP connection-oriented channel (stage 1 §4.7): a dynamic PSM,
 * encryption required, one channel at a time. */
#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>

#include "ble_internal.h"

/* Four SDUs in flight: enough to keep the link busy while one is refilled,
 * and the blocking allocation is what throttles the sender. */
NET_BUF_POOL_DEFINE(sdu_pool, 4, BT_L2CAP_SDU_BUF_SIZE(MN_BLE_L2CAP_MTU),
                    CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static struct bt_l2cap_le_chan chan;
static bool chan_in_use;

static int on_recv(struct bt_l2cap_chan *c, struct net_buf *buf)
{
    ARG_UNUSED(c);
    mn_ble_rx(MN_BLE_L2CAP, buf->data, buf->len);
    return 0;
}

static void on_connected(struct bt_l2cap_chan *c)
{
    ARG_UNUSED(c);
    mn_ble_l2cap_state(true);
}

static void on_disconnected(struct bt_l2cap_chan *c)
{
    ARG_UNUSED(c);
    chan_in_use = false;
    mn_ble_l2cap_state(false);
}

static const struct bt_l2cap_chan_ops ops = {
    .recv = on_recv,
    .connected = on_connected,
    .disconnected = on_disconnected,
};

static int on_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
                     struct bt_l2cap_chan **out)
{
    ARG_UNUSED(server);
    if (chan_in_use || conn != mn_ble_conn()) {
        return -ENOMEM; /* one channel, on the one connection */
    }
    chan_in_use = true;
    memset(&chan, 0, sizeof(chan));
    chan.chan.ops = &ops;
    chan.rx.mtu = MN_BLE_L2CAP_MTU;
    *out = &chan.chan;
    return 0;
}

static struct bt_l2cap_server server = {
    .psm = 0, /* dynamic: the stack picks one, the psm characteristic serves it */
    .sec_level = BT_SECURITY_L2,
    .accept = on_accept,
};

int mn_l2cap_init(void)
{
    return bt_l2cap_server_register(&server);
}

uint16_t mn_l2cap_psm(void)
{
    return server.psm;
}

int mn_l2cap_send(const uint8_t *data, size_t len)
{
    while (len > 0) {
        if (!chan_in_use) {
            return -ENOTCONN;
        }
        const size_t n = MIN(len, (size_t)MIN(chan.tx.mtu, MN_BLE_L2CAP_MTU));
        /* A host that stops granting credits, or vanishes, ends the link. */
        struct net_buf *buf = net_buf_alloc(&sdu_pool, K_SECONDS(10));

        if (buf == NULL) {
            return -ETIMEDOUT;
        }
        net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
        net_buf_add_mem(buf, data, n);
        const int ret = bt_l2cap_chan_send(&chan.chan, buf);

        if (ret < 0) {
            net_buf_unref(buf);
            return ret;
        }
        data += n;
        len -= n;
    }
    return 0;
}
