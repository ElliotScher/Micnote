/*
 * The puck simulator. At boot it mounts the recording store (flash.bin in
 * the working directory), recovers any recording a previous run left open,
 * and, if the store is empty, records two demo memos encrypted to the
 * test-vector host key host_a. Then it serves the control protocol on uart1,
 * which native_sim connects to a pseudo-terminal it names at startup:
 *
 *   uart_1 connected to pseudotty: /dev/pts/N
 *
 * Point micnote-cli at that path (see README.md).
 */
#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

#include <psa/crypto.h>

#include "dispatch.h"
#include "recstore.h"

#define MNT "/lfs"
#define DIR MNT "/rec"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);
static struct fs_mount_t mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &lfs_cfg,
    .storage_dev = (void *)PARTITION_ID(rec_partition),
    .mnt_point = MNT,
};

static const struct device *const link_uart = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* host_a from proto/testvectors (RFC 7748 section 6.1 "Bob"): the one
 * "paired" host until pairing exists. */
static const uint8_t host_a_pub[32] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
    0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d, 0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f};
static const uint8_t host_a_id[8] = {0xf3, 0x5e, 0x56, 0x16, 0x16, 0x0a, 0x30, 0xbf};

static uint64_t clock_offset_ms; /* unix_ms - uptime, once a host sets it */

static void set_time(uint64_t unix_ms, int32_t tz_offset_min)
{
    ARG_UNUSED(tz_offset_min);
    clock_offset_ms = unix_ms - (uint64_t)k_uptime_get();
    printk("sim: host set the clock\n");
}

static void fill_status(micnote_v1_DeviceStatus *s)
{
    s->battery_pct = 100;
    s->battery_mv = 4150;
    s->charge_source = micnote_v1_ChargeSource_CHARGE_SOURCE_USB;
    s->has_fw_version = true;
    s->fw_version.minor = 1;
    s->boot_count = 1;
    s->uptime_ms = (uint64_t)k_uptime_get();
    s->rtc_valid = clock_offset_ms != 0;
    s->unix_ms = s->rtc_valid ? clock_offset_ms + s->uptime_ms : 0;
    s->storage_total_kb = 1024;
}

static struct mn_link_config cfg = {
    .rec_dir = DIR,
    .n_hosts = 1,
    .fill_status = fill_status,
    .set_time = set_time,
};
static struct mn_link link;

/* ---- demo recordings ---------------------------------------------------- */

static struct mn_rec rec;

/* n_packets synthetic 40-byte "Opus packets" (20 ms each), with an item
 * marker every 100. */
static int record_demo(uint64_t rec_id, uint32_t n_packets)
{
    static struct mn_rec_params p;
    static struct mn_rec_secrets s;
    uint8_t packet[40];
    int ret;

    memset(&p, 0, sizeof(p));
    p.rec_id = rec_id;
    p.frame_ms = 20;
    p.start_boot = 1;
    p.start_uptime_ms = (uint64_t)k_uptime_get();
    p.n_recipients = 1;
    memcpy(p.recipients[0].pub, host_a_pub, sizeof(host_a_pub));
    memcpy(p.recipients[0].host_id, host_a_id, sizeof(host_a_id));
    ret = mn_rec_secrets_random(&s);
    if (ret == 0) {
        ret = mn_rec_open(&rec, DIR, &p, &s);
    }
    for (uint32_t i = 0; ret == 0 && i < n_packets; i++) {
        if (i > 0 && i % 100 == 0) {
            ret = mn_rec_marker(&rec, 1, i * 20);
        }
        memset(packet, (int)i, sizeof(packet));
        if (ret == 0) {
            ret = mn_rec_append(&rec, packet, sizeof(packet));
        }
    }
    if (ret == 0) {
        ret = mn_rec_close(&rec);
    }
    return ret;
}

static int seed_if_empty(void)
{
    static struct mn_recstore_info info;
    bool more;
    const int n = mn_recstore_list(DIR, 0, &info, 1, &more);

    if (n != 0) {
        return n < 0 ? n : 0;
    }
    printk("sim: store empty, recording two demo memos for host_a\n");
    int ret = record_demo(0x0000000100000001ULL, 250);

    if (ret == 0) {
        ret = record_demo(0x0000000100000002ULL, 600);
    }
    return ret;
}

/* ---- the link ----------------------------------------------------------- */

/* uart_fifo_fill on native_sim writes straight to the pseudo-terminal and
 * returns what it accepted, so a full buffer means "wait", not data loss. */
static int uart_send(const uint8_t *bytes, size_t len, void *ctx)
{
    ARG_UNUSED(ctx);
    int idle_ms = 0;

    while (len > 0) {
        const int n = uart_fifo_fill(link_uart, bytes, (int)MIN(len, 1024));

        if (n > 0) {
            bytes += n;
            len -= (size_t)n;
            idle_ms = 0;
            continue;
        }
        if (++idle_ms > 5000) {
            return -EIO; /* nobody is reading */
        }
        k_msleep(1);
    }
    return 0;
}

int main(void)
{
    static uint8_t buf[512];
    int ret;

    if (!device_is_ready(link_uart)) {
        printk("sim: uart1 not ready\n");
        return 0;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        printk("sim: PSA init failed\n");
        return 0;
    }
    ret = fs_mount(&mnt);
    if (ret == 0) {
        ret = mn_recstore_init(DIR);
    }
    if (ret == 0) {
        ret = mn_recstore_recover(DIR);
        if (ret > 0) {
            printk("sim: recovered %d recording(s) after a power cut\n", ret);
            ret = 0;
        }
    }
    if (ret == 0) {
        ret = seed_if_empty();
    }
    if (ret < 0) {
        printk("sim: storage failed: %d\n", ret);
        return 0;
    }

    memcpy(cfg.host_ids[0], host_a_id, sizeof(host_a_id));
    cfg.device_info.proto_version = 1;
    cfg.device_info.has_fw_version = true;
    cfg.device_info.fw_version.minor = 1;
    strcpy(cfg.device_info.board, "native_sim");
    mn_link_init(&link, &cfg, uart_send, NULL);
    printk("sim: serving the Micnote protocol on uart_1\n");

    for (;;) {
        const int n = uart_fifo_read(link_uart, buf, sizeof(buf));

        if (n <= 0) {
            k_msleep(1);
            continue;
        }
        ret = mn_link_rx(&link, buf, (size_t)n);
        if (ret < 0) {
            /* Lost sync or the host went away: start a fresh link. */
            printk("sim: link reset (%d)\n", ret);
            mn_link_init(&link, &cfg, uart_send, NULL);
        }
    }
    return 0;
}
