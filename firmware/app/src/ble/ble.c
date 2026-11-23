#include "ble.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>

#include "ble_internal.h"

LOG_MODULE_REGISTER(mn_ble, LOG_LEVEL_INF);

/* SMP AuthReq bit for LE Secure Connections (Core v5.4, Vol 3, Part H,
 * 3.5.1). */
#define SMP_AUTH_SC 0x08

#define LINK_STACK_SIZE 4096
/* Requests are small; 4 KB of them only builds up if a host floods us. */
#define RX_RING_SIZE 4096

static const struct mn_link_config *link_cfg;
static struct mn_link link;

static struct bt_conn *current; /* the one connection; a ref is held */
static atomic_t transport;      /* enum mn_ble_transport */
static atomic_t confirm_pending;

RING_BUF_DECLARE(mn_ble_rx_ring, RX_RING_SIZE);
static struct k_spinlock rx_lock;
static K_SEM_DEFINE(wake, 0, 1);
/* Events for the link thread. */
static atomic_t ev_reset;
static atomic_t ev_press;
static atomic_t ev_cancel;
static atomic_t ev_overflow;

K_THREAD_STACK_DEFINE(link_stack, LINK_STACK_SIZE);
static struct k_thread link_thread;

static struct k_work_delayable adv_timeout;
static struct k_work_delayable idle_work;
static struct k_work fast_work;

/* ---- connection parameters (§4.7) ---------------------------------------- */

/* During transfer: 15 ms (12 x 1.25 ms), no latency; 2M PHY, 251-byte PDUs. */
static void go_fast(struct k_work *w)
{
    ARG_UNUSED(w);
    struct bt_conn *conn = current;
    const struct bt_le_conn_param fast = BT_LE_CONN_PARAM_INIT(12, 12, 0, 400);
    const struct bt_conn_le_data_len_param dle = {.tx_max_len = 251, .tx_max_time = 2120};

    if (conn == NULL) {
        return;
    }
    const int p = bt_conn_le_param_update(conn, &fast);
    const int dl = bt_conn_le_data_len_update(conn, &dle);
    /* The controller refuses this while another link-layer procedure is
     * running (on a reconnect the peer often starts one itself), and the
     * link still ends up on 2M: not worth a warning. */
    const int phy = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);

    if (phy != 0) {
        LOG_DBG("2M PHY request: %d", phy);
    }
    if (p != 0 || dl != 0) {
        LOG_WRN("transfer parameters: interval %d, data length %d", p, dl);
    }
}

/* Idle: 100-500 ms (80-400 x 1.25 ms), latency 4. */
static void go_idle(struct k_work *w)
{
    ARG_UNUSED(w);
    const struct bt_le_conn_param idle = BT_LE_CONN_PARAM_INIT(80, 400, 4, 600);

    if (current != NULL) {
        const int err = bt_conn_le_param_update(current, &idle);

        if (err != 0) {
            LOG_WRN("idle parameters: %d", err);
        }
    }
}

static void activity(void)
{
    k_work_reschedule(&idle_work, K_MSEC(MN_BLE_IDLE_MS));
}

/* ---- bytes in and out ---------------------------------------------------- */

struct bt_conn *mn_ble_conn(void)
{
    return current;
}

void mn_ble_rx(enum mn_ble_transport t, const uint8_t *data, size_t len)
{
    /* The first transport to speak owns the connection's link. */
    if (!atomic_cas(&transport, MN_BLE_NONE, t) && atomic_get(&transport) != t) {
        LOG_WRN("bytes on the other transport ignored");
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&rx_lock);
    const uint32_t put = ring_buf_put(&mn_ble_rx_ring, data, len);

    k_spin_unlock(&rx_lock, key);
    if (put != len) {
        atomic_set(&ev_overflow, 1);
    }
    k_sem_give(&wake);
}

void mn_ble_l2cap_state(bool up)
{
    if (up) {
        k_work_submit(&fast_work);
        activity();
    } else if (atomic_get(&transport) == MN_BLE_L2CAP) {
        /* The channel closed: the next transport starts a fresh link. */
        atomic_set(&ev_reset, 1);
        k_sem_give(&wake);
    }
}

static int link_send(const uint8_t *bytes, size_t len, void *ctx)
{
    ARG_UNUSED(ctx);
    activity();
    switch (atomic_get(&transport)) {
        case MN_BLE_L2CAP:
            return mn_l2cap_send(bytes, len);
        case MN_BLE_GATT: {
            struct bt_conn *conn = current;

            return conn != NULL ? mn_gatt_send(conn, bytes, len) : -ENOTCONN;
        }
        default:
            return -ENOTCONN;
    }
}

/* ---- the link thread ----------------------------------------------------- */

static void link_main(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    static uint8_t buf[512];

    mn_link_init(&link, link_cfg, link_send, NULL);
    for (;;) {
        k_sem_take(&wake, K_FOREVER);
        if (atomic_cas(&ev_reset, 1, 0)) {
            mn_link_close(&link);
            mn_link_init(&link, link_cfg, link_send, NULL);
            k_spinlock_key_t key = k_spin_lock(&rx_lock);

            ring_buf_reset(&mn_ble_rx_ring);
            k_spin_unlock(&rx_lock, key);
            atomic_set(&transport, MN_BLE_NONE);
        }
        if (atomic_cas(&ev_cancel, 1, 0)) {
            (void)mn_link_pair_cancel(&link);
        }
        if (atomic_cas(&ev_press, 1, 0)) {
            /* Taken: completing the pairing (host slot, re-wrapping) takes a
             * while, and the button shouldn't count twice meanwhile. */
            atomic_set(&confirm_pending, 0);
            (void)mn_link_pair_press(&link);
        }
        for (;;) {
            k_spinlock_key_t key = k_spin_lock(&rx_lock);
            const uint32_t n = ring_buf_get(&mn_ble_rx_ring, buf, sizeof(buf));

            k_spin_unlock(&rx_lock, key);
            if (n == 0) {
                break;
            }
            activity();
            const int ret = mn_link_rx(&link, buf, n);

            if (ret < 0) {
                /* Lost framing or the link failed: drop the connection. */
                LOG_WRN("link error %d: disconnecting", ret);
                if (current != NULL) {
                    (void)bt_conn_disconnect(current, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
                }
                break;
            }
        }
        if (atomic_cas(&ev_overflow, 1, 0) && current != NULL) {
            LOG_WRN("receive queue overflowed: disconnecting");
            (void)bt_conn_disconnect(current, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
        atomic_set(&confirm_pending, link.confirm_pending);
    }
}

/* ---- connections and security -------------------------------------------- */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    /* Connection callbacks are global: ignore them until mn_ble_init(). */
    if (link_cfg == NULL || err != 0) {
        return;
    }
    if (current != NULL) {
        /* One host at a time (the dispatcher serves one link). */
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }
    current = bt_conn_ref(conn);
    k_work_cancel_delayable(&adv_timeout);
    LOG_INF("connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (link_cfg == NULL || conn != current) {
        return;
    }
    LOG_INF("disconnected (0x%02x)", reason);
    k_work_cancel_delayable(&idle_work);
    bt_conn_unref(current);
    current = NULL;
    atomic_set(&ev_reset, 1);
    k_sem_give(&wake);
}

static void on_params(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
    ARG_UNUSED(timeout);
    if (link_cfg != NULL && conn == current) {
        LOG_INF("connection interval %u x 1.25 ms, latency %u", interval, latency);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .le_param_updated = on_params,
};

static enum bt_security_err pairing_accept(struct bt_conn *conn,
                                           const struct bt_conn_pairing_feat *const feat)
{
    ARG_UNUSED(conn);
    if (link_cfg->pairing_open == NULL || !link_cfg->pairing_open()) {
        LOG_WRN("bonding refused: no pairing window");
        return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
    }
    if (feat == NULL || (feat->auth_req & SMP_AUTH_SC) == 0) {
        LOG_WRN("bonding refused: legacy pairing");
        return BT_SECURITY_ERR_AUTH_REQUIREMENT;
    }
    return BT_SECURITY_ERR_SUCCESS;
}

static struct bt_conn_auth_cb auth_callbacks = {
    .pairing_accept = pairing_accept,
};

/* ---- advertising --------------------------------------------------------- */

static void adv_expired(struct k_work *w)
{
    ARG_UNUSED(w);
    (void)bt_le_adv_stop();
}

int mn_ble_advertise(enum mn_adv_mode mode, bool sync_pending)
{
    static uint8_t mfg[MN_ADV_MFG_LEN];
    static const uint8_t uuid[16] = {MN_BLE_UUID_BYTES(0x00)};
    const struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
        BT_DATA(BT_DATA_UUID128_ALL, uuid, sizeof(uuid)),
        BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg, sizeof(mfg)),
    };
    const struct mn_adv_timing t = mn_adv_timing(mode);

    k_work_cancel_delayable(&adv_timeout);
    (void)bt_le_adv_stop();
    if (mode == MN_ADV_OFF) {
        return 0;
    }
    if (current != NULL) {
        return -EBUSY; /* already connected: nothing to advertise for */
    }
    mn_adv_mfg_data(sync_pending, mfg);
    const struct bt_le_adv_param param =
        BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, t.interval_min, t.interval_max, NULL);
    const int ret = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);

    if (ret == 0) {
        k_work_reschedule(&adv_timeout, K_MSEC(t.duration_ms));
    }
    return ret;
}

/* ---- the rest of the API ------------------------------------------------- */

int mn_ble_init(const struct mn_link_config *cfg)
{
    link_cfg = cfg;
    k_work_init_delayable(&adv_timeout, adv_expired);
    k_work_init_delayable(&idle_work, go_idle);
    k_work_init(&fast_work, go_fast);

    int ret = bt_conn_auth_cb_register(&auth_callbacks);

    if (ret == 0) {
        ret = bt_enable(NULL);
    }
    if (ret == 0 && IS_ENABLED(CONFIG_BT_SETTINGS)) {
        ret = settings_load_subtree("bt"); /* bonds and the identity */
    }
    if (ret == 0) {
        ret = mn_l2cap_init();
    }
    if (ret != 0) {
        return ret;
    }
    k_thread_create(&link_thread, link_stack, K_THREAD_STACK_SIZEOF(link_stack), link_main, NULL,
                    NULL, NULL, MN_BLE_LINK_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&link_thread, "link");
    return 0;
}

void mn_ble_pair_press(void)
{
    atomic_set(&ev_press, 1);
    k_sem_give(&wake);
}

void mn_ble_pair_cancel(void)
{
    atomic_set(&ev_cancel, 1);
    k_sem_give(&wake);
}

bool mn_ble_connected(void)
{
    return current != NULL;
}

enum mn_ble_transport mn_ble_transport(void)
{
    return (enum mn_ble_transport)atomic_get(&transport);
}

bool mn_ble_confirm_pending(void)
{
    return atomic_get(&confirm_pending) != 0;
}

uint16_t mn_ble_psm(void)
{
    return mn_l2cap_psm();
}

void mn_ble_status_encode(const struct mn_ble_status *s, uint8_t out[MN_BLE_STATUS_LEN])
{
    out[0] = s->battery_pct;
    out[1] = s->state;
    sys_put_le16(s->recording_count, &out[2]);
    out[4] = s->storage_used_pct;
    out[5] = s->flags;
}

void mn_ble_status_set(const struct mn_ble_status *s)
{
    uint8_t v[MN_BLE_STATUS_LEN];

    mn_ble_status_encode(s, v);
    mn_gatt_status_notify(v);
}
