#include "timekeeping.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#define BOOT_KEY "mn/boot"

void mn_clock_set(struct mn_clock *c, uint64_t unix_ms, int32_t tz_offset_min, int64_t uptime_ms)
{
    memset(c, 0, sizeof(*c));
    if (unix_ms == 0) {
        return;
    }
    c->valid = true;
    c->base_unix_ms = unix_ms;
    c->base_uptime_ms = uptime_ms;
    c->tz_offset_min = tz_offset_min;
}

uint64_t mn_clock_unix_ms(const struct mn_clock *c, int64_t uptime_ms)
{
    if (!c->valid) {
        return 0;
    }
    /* Uptime only goes forward; a caller passing an earlier one gets the
     * base time rather than a wrapped-around one. */
    if (uptime_ms < c->base_uptime_ms) {
        return c->base_unix_ms;
    }
    return c->base_unix_ms + (uint64_t)(uptime_ms - c->base_uptime_ms);
}

static struct mn_clock wall;
static uint32_t boot_count;
static uint32_t rec_counter;
static bool have_boot_count;

/* settings_load_subtree_direct() callback: the stored boot count. */
static int load_boot(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                     void *param)
{
    uint32_t *out = param;

    if (key != NULL || len != sizeof(*out)) {
        return 0; /* not ours, or the wrong size: start over from 0 */
    }
    if (read_cb(cb_arg, out, sizeof(*out)) != sizeof(*out)) {
        *out = 0;
    }
    return 0;
}

int mn_time_init(void)
{
    uint32_t stored = 0;
    int ret;

    memset(&wall, 0, sizeof(wall));
    boot_count = 0;
    rec_counter = 0;
    have_boot_count = false;

    ret = settings_load_subtree_direct(BOOT_KEY, load_boot, &stored);
    if (ret < 0) {
        return ret;
    }
    /* 0 is reserved for "unknown", so a counter that ran out stays at the
     * maximum rather than wrapping to it. */
    const uint32_t next = stored == UINT32_MAX ? UINT32_MAX : stored + 1;

    ret = settings_save_one(BOOT_KEY, &next, sizeof(next));
    if (ret < 0) {
        return ret;
    }
    boot_count = next;
    have_boot_count = true;
    return 0;
}

uint32_t mn_time_boot_count(void)
{
    return boot_count;
}

uint64_t mn_time_next_rec_id(void)
{
    if (!have_boot_count || rec_counter == UINT32_MAX) {
        return 0;
    }
    rec_counter++;
    return ((uint64_t)boot_count << 32) | rec_counter;
}

void mn_time_set(uint64_t unix_ms, int32_t tz_offset_min)
{
    mn_clock_set(&wall, unix_ms, tz_offset_min, k_uptime_get());
}

bool mn_time_valid(void)
{
    return wall.valid;
}

uint64_t mn_time_unix_ms(void)
{
    return mn_clock_unix_ms(&wall, k_uptime_get());
}

int32_t mn_time_tz_offset_min(void)
{
    return wall.tz_offset_min;
}
