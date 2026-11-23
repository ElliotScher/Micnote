/*
 * The puck simulator. At boot it mounts the recording store (flash.bin in
 * the working directory), recovers any recording a previous run left open,
 * and, if the store is empty, records two demo memos (real Opus from the
 * encoder thread, on a synthetic voice), encrypted to the paired hosts or,
 * on a fresh puck, to the device fallback key: pairing then moves them to
 * the new host. Then it serves the control protocol on uart1,
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
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

#include <psa/crypto.h>

#include "dispatch.h"
#include "hosts.h"
#include "keys.h"
#include "pipeline.h"
#include "preproc.h"
#include "recstore.h"
#include "timekeeping.h"
#include "voice.h"

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

static struct mn_fallback_key fallback;

/* The simulator has no button. Stand-ins: the PAIRING window is open
 * whenever no host is paired (a fresh puck, as if the button were held for
 * 5 s), and the button is "pressed" half a second after a host confirms. */
#define SIM_PRESS_DELAY_MS 500

static bool pairing_open(void)
{
    return mn_hosts_count() == 0;
}

static void show_sas(const uint8_t sas[MN_SAS_SYMBOLS])
{
    static const char *const names[MN_SAS_COLORS] = {"red",    "green", "blue",
                                                     "yellow", "cyan",  "magenta"};

    if (sas == NULL) {
        printk("sim: LED off\n");
        return;
    }
    printk("sim: LED shows %s %s %s %s\n", names[sas[0]], names[sas[1]], names[sas[2]],
           names[sas[3]]);
}

static void set_time(uint64_t unix_ms, int32_t tz_offset_min)
{
    mn_time_set(unix_ms, tz_offset_min);
    printk("sim: host set the clock\n");
}

static void fill_status(micnote_v1_DeviceStatus *s)
{
    s->battery_pct = 100;
    s->battery_mv = 4150;
    s->charge_source = micnote_v1_ChargeSource_CHARGE_SOURCE_USB;
    s->has_fw_version = true;
    s->fw_version.minor = 1;
    s->boot_count = mn_time_boot_count();
    s->uptime_ms = (uint64_t)k_uptime_get();
    s->rtc_valid = mn_time_valid();
    s->unix_ms = mn_time_unix_ms();
    s->storage_total_kb = 1024;
}

static struct mn_link_config cfg = {
    .rec_dir = DIR,
    .fallback = &fallback,
    .fill_status = fill_status,
    .set_time = set_time,
    .pairing_open = pairing_open,
    .show_sas = show_sas,
};
static struct mn_link link;

/* ---- demo recordings ---------------------------------------------------- */

static struct mn_rec rec;

/* The demo memos are real audio through the firmware's encoder thread
 * (audio/pipeline.c): the tests' synthetic voice, served in 10 ms blocks by
 * a source shaped like the DMIC driver. */
#define DEMO_MAX_PACKETS 600
static int16_t demo_voice[DEMO_MAX_PACKETS * MN_ENC_FRAME_SAMPLES];

struct demo_source {
    size_t blocks;
    size_t next;
};

static int demo_read(void *ctx, int16_t **buf)
{
    struct demo_source *d = ctx;

    if (d->next == d->blocks) {
        return -ENODATA;
    }
    *buf = &demo_voice[d->next++ * MN_AUDIO_BLOCK_FRAMES];
    return 0;
}

static void demo_release(void *ctx, int16_t *buf)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(buf);
}

/* Appends each packet, with an item marker every 2 s. */
static int demo_sink(void *ctx, const uint8_t *packet, size_t len)
{
    struct mn_rec *r = ctx;
    const uint32_t ms = r->duration_ms;

    if (ms > 0 && ms % 2000 == 0) {
        int ret = mn_rec_marker(r, 1, ms);

        if (ret < 0) {
            return ret;
        }
    }
    return mn_rec_append(r, packet, len);
}

/* A memo of n_packets 20 ms packets. */
static int record_demo(uint32_t n_packets)
{
    static struct mn_rec_params p;
    static struct mn_rec_secrets s;
    struct demo_source src_ctx = {.blocks = n_packets * 2};
    const struct mn_audio_source src = {
        .read = demo_read,
        .release = demo_release,
        .ctx = &src_ctx,
        .channels = 1,
    };
    const struct mn_audio_config cfg = {
        .gain_q8 = MN_PREPROC_GAIN_UNITY,
        .complexity = MN_ENC_COMPLEXITY_DEFAULT,
        .sink = demo_sink,
        .sink_ctx = &rec,
    };
    int ret;

    memset(&p, 0, sizeof(p));
    p.rec_id = mn_time_next_rec_id();
    if (p.rec_id == 0) {
        return -ENOSPC;
    }
    p.frame_ms = MN_ENC_FRAME_MS;
    p.start_boot = mn_time_boot_count();
    p.start_uptime_ms = (uint64_t)k_uptime_get();
    p.start_unix_ms = mn_time_unix_ms();
    /* To every paired host, or to the fallback key before the first
     * pairing, which then moves them to that host (§4.5). */
    p.n_recipients = (uint8_t)mn_hosts_recipients(p.recipients);
    if (p.n_recipients == 0) {
        p.n_recipients = 1;
        memcpy(p.recipients[0].pub, fallback.pub, sizeof(fallback.pub));
        memcpy(p.recipients[0].host_id, fallback.host_id, sizeof(fallback.host_id));
    }
    mn_voice(demo_voice, n_packets * MN_ENC_FRAME_SAMPLES);
    ret = mn_rec_secrets_random(&s);
    if (ret == 0) {
        ret = mn_rec_open(&rec, DIR, &p, &s);
    }
    if (ret == 0) {
        ret = mn_audio_start(&src, &cfg);
        /* The source ends by itself: wait for that, don't stop it. */
        int stopped = ret == 0 ? mn_audio_wait() : ret;

        ret = mn_rec_close(&rec);
        if (stopped < 0) {
            ret = stopped;
        }
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
    printk("sim: store empty, recording two demo memos\n");
    int ret = record_demo(250);

    if (ret == 0) {
        ret = record_demo(600);
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
    /* Settings before timekeeping, timekeeping before storage, storage
     * before keys (§4.1). */
    ret = settings_subsys_init();
    if (ret == 0) {
        ret = mn_time_init();
    }
    if (ret == 0) {
        ret = mn_hosts_init();
    }
    if (ret == 0) {
        ret = mn_keys_fallback(&fallback);
        if (ret == 1) {
            printk("sim: first boot, made the device fallback key\n");
        }
        ret = ret < 0 ? ret : 0;
    }
    if (ret == 0) {
        printk("sim: boot %u\n", mn_time_boot_count());
        ret = fs_mount(&mnt);
    }
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

    printk("sim: %u host(s) paired%s\n", (unsigned)mn_hosts_count(),
           pairing_open() ? "; pairing window open" : "");
    cfg.device_info.proto_version = 1;
    memcpy(cfg.device_info.device_id, (const uint8_t[]){'m', 'n', 's', 'i', 'm', 0, 0, 1}, 8);
    cfg.device_info.has_fw_version = true;
    cfg.device_info.fw_version.minor = 1;
    strcpy(cfg.device_info.board, "native_sim");
    mn_link_init(&link, &cfg, uart_send, NULL);
    printk("sim: serving the Micnote protocol on uart_1\n");

    for (;;) {
        static int64_t confirm_since = -1;
        const int n = uart_fifo_read(link_uart, buf, sizeof(buf));

        /* The simulated button press. */
        if (link.confirm_pending) {
            if (confirm_since < 0) {
                confirm_since = k_uptime_get();
            } else if (k_uptime_get() - confirm_since >= SIM_PRESS_DELAY_MS) {
                printk("sim: button pressed\n");
                if (mn_link_pair_press(&link) == 0) {
                    printk("sim: paired; %u host(s)\n", (unsigned)mn_hosts_count());
                }
                confirm_since = -1;
            }
        } else {
            confirm_since = -1;
        }
        if (n <= 0) {
            k_msleep(1);
            continue;
        }
        ret = mn_link_rx(&link, buf, (size_t)n);
        if (ret < 0) {
            /* Lost sync or the host went away: start a fresh link. */
            printk("sim: link reset (%d)\n", ret);
            mn_link_close(&link);
            mn_link_init(&link, &cfg, uart_send, NULL);
        }
    }
    return 0;
}
