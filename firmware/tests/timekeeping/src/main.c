#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

#include "timekeeping.h"

#define T0 1700000000000ULL /* 2023-11-14, in Unix ms */

/* ---- mn_clock: the pure math ------------------------------------------ */

ZTEST(clock, test_unset_is_zero)
{
    struct mn_clock c = {0};

    zassert_false(c.valid);
    zassert_equal(mn_clock_unix_ms(&c, 12345), 0);
}

ZTEST(clock, test_set_zero_stays_invalid)
{
    struct mn_clock c;

    mn_clock_set(&c, T0, 60, 10);
    mn_clock_set(&c, 0, 60, 20);
    zassert_false(c.valid);
    zassert_equal(c.tz_offset_min, 0);
    zassert_equal(mn_clock_unix_ms(&c, 30), 0);
}

ZTEST(clock, test_advances_with_uptime)
{
    struct mn_clock c;
    const int64_t month_ms = 30LL * 24 * 3600 * 1000;

    mn_clock_set(&c, T0, -300, 5000);
    zassert_true(c.valid);
    zassert_equal(c.tz_offset_min, -300);
    zassert_equal(mn_clock_unix_ms(&c, 5000), T0);
    zassert_equal(mn_clock_unix_ms(&c, 6250), T0 + 1250);
    /* A month of uptime: nothing overflows 32 bits. */
    zassert_equal(mn_clock_unix_ms(&c, 5000 + month_ms), T0 + (uint64_t)month_ms);
}

ZTEST(clock, test_earlier_uptime_gives_base)
{
    struct mn_clock c;

    mn_clock_set(&c, T0, 0, 5000);
    zassert_equal(mn_clock_unix_ms(&c, 4999), T0);
}

ZTEST(clock, test_set_again_replaces_base)
{
    struct mn_clock c;

    mn_clock_set(&c, T0, 0, 1000);
    mn_clock_set(&c, T0 + 100000, 120, 2000);
    zassert_equal(mn_clock_unix_ms(&c, 3000), T0 + 101000);
    zassert_equal(c.tz_offset_min, 120);
}

ZTEST_SUITE(clock, NULL, NULL, NULL, NULL, NULL);

/* ---- mn_time: boot count, recording IDs, the global clock ------------- */

static int read_u32(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                    void *param)
{
    if (key == NULL && len == sizeof(uint32_t)) {
        zassert_equal(read_cb(cb_arg, param, sizeof(uint32_t)), sizeof(uint32_t));
    }
    return 0;
}

/* What's on flash, not the module's copy. */
static uint32_t stored_boot(void)
{
    uint32_t v = 0;

    zassert_ok(settings_load_subtree_direct("mn/boot", read_u32, &v));
    return v;
}

static void *setup(void)
{
    zassert_ok(settings_subsys_init());
    return NULL;
}

/* Every test starts from a puck that has never booted. */
static void wipe(void *fixture)
{
    ARG_UNUSED(fixture);
    zassert_ok(settings_delete("mn/boot"));
}

ZTEST(time, test_first_boot_is_one)
{
    zassert_ok(mn_time_init());
    zassert_equal(mn_time_boot_count(), 1);
    zassert_equal(stored_boot(), 1);
}

ZTEST(time, test_each_boot_counts_up)
{
    for (uint32_t i = 1; i <= 5; i++) {
        zassert_ok(mn_time_init());
        zassert_equal(mn_time_boot_count(), i);
    }
    zassert_equal(stored_boot(), 5);
}

ZTEST(time, test_counter_stops_at_max)
{
    const uint32_t max = UINT32_MAX;

    zassert_ok(settings_save_one("mn/boot", &max, sizeof(max)));
    zassert_ok(mn_time_init());
    zassert_equal(mn_time_boot_count(), UINT32_MAX);
    zassert_equal(stored_boot(), UINT32_MAX);
}

ZTEST(time, test_wrong_size_starts_over)
{
    const uint16_t junk = 7;

    zassert_ok(settings_save_one("mn/boot", &junk, sizeof(junk)));
    zassert_ok(mn_time_init());
    zassert_equal(mn_time_boot_count(), 1);
}

ZTEST(time, test_other_keys_ignored)
{
    const uint32_t v = 41;

    /* A key under mn/boot/ isn't the boot count. */
    zassert_ok(settings_save_one("mn/boot/x", &v, sizeof(v)));
    zassert_ok(mn_time_init());
    zassert_equal(mn_time_boot_count(), 1);
    zassert_ok(settings_delete("mn/boot/x"));
}

ZTEST(time, test_rec_ids_per_boot)
{
    zassert_ok(mn_time_init());
    zassert_equal(mn_time_next_rec_id(), (1ULL << 32) | 1);
    zassert_equal(mn_time_next_rec_id(), (1ULL << 32) | 2);

    /* Next boot: the counter restarts under the new boot count, and every ID
     * sorts after the last boot's. */
    zassert_ok(mn_time_init());
    const uint64_t id = mn_time_next_rec_id();

    zassert_equal(id, (2ULL << 32) | 1);
    zassert_true(id > ((1ULL << 32) | 2));
}

ZTEST(time, test_clock_invalid_after_boot)
{
    mn_time_set(T0, 60);
    zassert_ok(mn_time_init());
    zassert_false(mn_time_valid());
    zassert_equal(mn_time_unix_ms(), 0);
}

ZTEST(time, test_clock_runs)
{
    zassert_ok(mn_time_init());
    mn_time_set(T0, -480);
    zassert_true(mn_time_valid());
    zassert_equal(mn_time_tz_offset_min(), -480);

    const uint64_t before = mn_time_unix_ms();

    zassert_true(before >= T0 && before < T0 + 10);
    k_msleep(250); /* rounded up to whole 10 ms ticks on native_sim */
    const uint64_t after = mn_time_unix_ms();

    zassert_true(after - before >= 250 && after - before <= 270, "advanced %llu ms",
                 (unsigned long long)(after - before));
}

ZTEST_SUITE(time, NULL, setup, wipe, NULL, NULL);
