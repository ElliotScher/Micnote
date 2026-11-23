/*
 * The puck role: the firmware's BLE module, dispatcher, host slots and
 * recording store, set up as main.c will on the board. Two memos are on the
 * fallback key from before any pairing. Stand-ins for the hardware: the
 * PAIRING window is open while no host is paired, and the button is
 * "pressed" 300 ms after a host confirms.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>

#include <psa/crypto.h>

#include "babblekit/testcase.h"
#include "ble.h"
#include "hosts.h"
#include "keys.h"
#include "recstore.h"
#include "roles.h"

#define MNT "/lfs"
#define DIR MNT "/rec"
#define MEMO_PACKETS 400 /* 8 s of 40-byte packets, about 17 KB */

const uint8_t puck_device_id[8] = {'m', 'n', 'b', 's', 'i', 'm', 0, 1};

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);
static struct fs_mount_t mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &lfs_cfg,
    .storage_dev = (void *)PARTITION_ID(rec_partition),
    .mnt_point = MNT,
};

static struct mn_fallback_key fallback;
static struct mn_rec rec;

static bool pairing_open(void)
{
    return mn_hosts_count() == 0;
}

static void show_sas(const uint8_t sas[MN_SAS_SYMBOLS])
{
    if (sas != NULL) {
        TEST_PRINT("puck: LED shows %u %u %u %u", sas[0], sas[1], sas[2], sas[3]);
    }
}

static struct mn_link_config cfg = {
    .rec_dir = DIR,
    .fallback = &fallback,
    .pairing_open = pairing_open,
    .show_sas = show_sas,
};

static void record_memo(uint64_t id)
{
    static struct mn_rec_params p;
    static struct mn_rec_secrets s;
    uint8_t packet[40];

    memset(&p, 0, sizeof(p));
    p.rec_id = id;
    p.frame_ms = 20;
    p.start_boot = 1;
    p.n_recipients = 1;
    memcpy(p.recipients[0].pub, fallback.pub, 32);
    memcpy(p.recipients[0].host_id, fallback.host_id, 8);
    TEST_ASSERT(mn_rec_secrets_random(&s) == 0, "randomness");
    const int err = mn_rec_open(&rec, DIR, &p, &s);

    TEST_ASSERT(err == 0, "rec open: %d", err);
    for (uint32_t i = 0; i < MEMO_PACKETS; i++) {
        memset(packet, (int)i, sizeof(packet));
        packet[0] = 9 << 3; /* a valid Opus TOC, for the host's sake */
        TEST_ASSERT(mn_rec_append(&rec, packet, sizeof(packet)) == 0, "append");
    }
    TEST_ASSERT(mn_rec_close(&rec) == 0, "rec close");
}

static size_t recordings_left(void)
{
    static struct mn_recstore_info info[4];
    bool more;
    const int n = mn_recstore_list(DIR, 0, info, ARRAY_SIZE(info), &more);

    TEST_ASSERT(n >= 0, "list: %d", n);
    return (size_t)n;
}

void puck_main(void)
{
    const struct mn_ble_status status = {
        .battery_pct = 80,
        .state = micnote_v1_DeviceState_DEVICE_STATE_IDLE,
        .recording_count = 2,
        .flags = MN_BLE_STATUS_SYNC_PENDING,
    };

    TEST_ASSERT(psa_crypto_init() == PSA_SUCCESS, "psa");
    TEST_ASSERT(settings_subsys_init() == 0, "settings");
    TEST_ASSERT(fs_mount(&mnt) == 0, "mount");
    TEST_ASSERT(mn_recstore_init(DIR) == 0, "recstore");
    TEST_ASSERT(mn_hosts_init() == 0, "hosts");
    TEST_ASSERT(mn_keys_fallback(&fallback) >= 0, "fallback key");
    record_memo(0x0000000100000001ULL);
    record_memo(0x0000000100000002ULL);

    cfg.device_info.proto_version = 1;
    memcpy(cfg.device_info.device_id, puck_device_id, 8);
    strcpy(cfg.device_info.board, "nrf52_bsim");
    TEST_ASSERT(mn_ble_init(&cfg) == 0, "ble init");
    mn_ble_status_set(&status);
    TEST_ASSERT(mn_ble_advertise(MN_ADV_FAST, true) == 0, "advertise");
    TEST_PRINT("puck: advertising, psm 0x%04x", mn_ble_psm());

    bool was_connected = false;
    int64_t confirm_since = -1;
    bool passed = false;

    for (;;) {
        k_msleep(20);
        const bool connected = mn_ble_connected();

        if (was_connected && !connected) {
            /* As main.c will after a sync: slow advertising, so the host
             * can come back; FAST again for the next test step. */
            (void)mn_ble_advertise(MN_ADV_FAST, recordings_left() > 0);
        }
        was_connected = connected;

        if (mn_ble_confirm_pending()) {
            if (confirm_since < 0) {
                confirm_since = k_uptime_get();
            } else if (k_uptime_get() - confirm_since >= 300) {
                TEST_PRINT("puck: button pressed");
                mn_ble_pair_press();
                confirm_since = -1;
            }
        } else {
            confirm_since = -1;
        }

        if (!passed && mn_hosts_count() == 1 && recordings_left() == 0) {
            passed = true;
            TEST_PASS("puck: paired one host, everything synced and acked");
        }
    }
}
