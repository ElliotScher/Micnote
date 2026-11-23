/*
 * Synthetic traces: the Earth's field (about 50 µT) with ±2 µT of sensor
 * noise, sampled at 20 Hz, and a MagSafe ring that adds a few mT when the
 * puck is on it. A snap comes with a knock of a few g.
 */
#include <zephyr/ztest.h>

#include "snap.h"

#define DT_MS 50 /* 20 Hz */

static const int32_t earth[3] = {20, -5, 45};
static const int32_t ring[3] = {400, -300, 2800}; /* added by the magnet */

static struct mn_snap s;
static int64_t now;
static uint32_t lcg = 1;
static int n_snapped, n_charger, n_unsnapped;
static int64_t snapped_at, unsnapped_at;

static int32_t noise(void)
{
    lcg = lcg * 1103515245u + 12345u;
    return (int32_t)((lcg >> 16) % 5) - 2;
}

static void reset(void *fixture)
{
    static const struct mn_snap_cfg cfg = MN_SNAP_CFG_DEFAULT;

    ARG_UNUSED(fixture);
    mn_snap_init(&s, &cfg);
    now = 0;
    lcg = 1;
    n_snapped = n_charger = n_unsnapped = 0;
    snapped_at = unsnapped_at = -1;
}

/* `ms` of samples at earth + frac * ring (frac in percent) + offset. */
static void run(int64_t ms, int frac, const int32_t offset[3], bool moving, bool on_charger)
{
    for (const int64_t end = now + ms; now < end; now += DT_MS) {
        int32_t b[3];

        for (int i = 0; i < 3; i++) {
            b[i] = earth[i] + ring[i] * frac / 100 + (offset ? offset[i] : 0) + noise();
        }
        switch (mn_snap_sample(&s, b, moving, on_charger, now)) {
            case MN_SNAP_SNAPPED:
                n_snapped++;
                snapped_at = now;
                break;
            case MN_SNAP_SNAPPED_CHARGER:
                n_charger++;
                snapped_at = now;
                break;
            case MN_SNAP_UNSNAPPED:
                n_unsnapped++;
                unsnapped_at = now;
                break;
            case MN_SNAP_NONE:
                break;
        }
    }
}

static void off_ring(int64_t ms)
{
    run(ms, 0, NULL, false, false);
}

static void on_ring(int64_t ms)
{
    run(ms, 100, NULL, false, false);
}

static void knock(uint32_t mg)
{
    mn_snap_shock(&s, mg, now);
}

ZTEST(snap, test_quiet_desk)
{
    off_ring(60000);
    zassert_equal(n_snapped + n_charger + n_unsnapped, 0);
    zassert_true(s.dev_ut < 10, "noise read as %u µT", s.dev_ut);
}

ZTEST(snap, test_snap_then_release)
{
    off_ring(5000);
    const int64_t t_snap = now;

    knock(3500);
    on_ring(3000);
    zassert_equal(n_snapped, 1);
    /* Declared once the field has been changed for 300 ms. */
    zassert_equal(snapped_at, t_snap + 300);

    off_ring(3000);
    zassert_equal(n_unsnapped, 1);
    zassert_true(unsnapped_at - t_snap - 3000 >= 1000 && unsnapped_at - t_snap - 3000 <= 1050,
                 "unsnapped %lld ms after leaving", (long long)(unsnapped_at - t_snap - 3000));
    zassert_equal(n_snapped, 1);
}

ZTEST(snap, test_magnet_without_knock)
{
    /* Set down gently next to a laptop's magnets: field, no shock. */
    off_ring(5000);
    knock(1500); /* a shock, but under 2 g */
    on_ring(3000);
    zassert_equal(n_snapped, 0);
}

ZTEST(snap, test_knock_without_magnet)
{
    off_ring(2000);
    knock(8000);
    off_ring(3000);
    zassert_equal(n_snapped, 0);
}

ZTEST(snap, test_knock_just_inside_window)
{
    off_ring(5000);
    knock(3000);
    off_ring(450); /* shock 450 ms before the field moves */
    on_ring(2000);
    zassert_equal(n_snapped, 1);
}

ZTEST(snap, test_knock_too_early)
{
    off_ring(5000);
    knock(3000);
    off_ring(600); /* shock 600 ms before the field moves */
    on_ring(2000);
    zassert_equal(n_snapped, 0);
}

ZTEST(snap, test_knock_during_confirmation)
{
    off_ring(5000);
    on_ring(150);
    knock(3000);
    on_ring(2000);
    zassert_equal(n_snapped, 1);
}

ZTEST(snap, test_brief_field_change)
{
    /* Waved past a magnet: 200 ms of field, with a knock. */
    off_ring(5000);
    knock(3000);
    on_ring(200);
    off_ring(3000);
    zassert_equal(n_snapped, 0);
}

ZTEST(snap, test_on_charger)
{
    off_ring(5000);
    knock(3000);
    run(2000, 100, NULL, false, true);
    zassert_equal(n_snapped, 0);
    zassert_equal(n_charger, 1);
    run(2000, 0, NULL, false, true);
    zassert_equal(n_unsnapped, 1);
}

ZTEST(snap, test_release_needs_a_full_second)
{
    off_ring(5000);
    knock(3000);
    on_ring(2000);
    zassert_equal(n_snapped, 1);
    /* Slides off for 900 ms, then back on: still snapped. */
    off_ring(900);
    on_ring(2000);
    zassert_equal(n_unsnapped, 0);
}

ZTEST(snap, test_hysteresis)
{
    off_ring(5000);
    knock(3000);
    on_ring(2000);
    /* Slipped part way off: 40 % of the ring is still above t_snap/2. */
    run(5000, 40, NULL, false, false);
    zassert_equal(n_unsnapped, 0);
    run(2000, 10, NULL, false, false);
    zassert_equal(n_unsnapped, 1);
}

ZTEST(snap, test_shock_used_once)
{
    off_ring(5000);
    knock(3000);
    on_ring(2000);
    off_ring(2000);
    zassert_equal(n_unsnapped, 1);
    /* Back on the ring without a new knock: not a new snap. */
    on_ring(2000);
    zassert_equal(n_snapped, 1);
}

ZTEST(snap, test_second_snap)
{
    off_ring(5000);
    knock(3000);
    on_ring(2000);
    off_ring(2000);
    knock(2500);
    on_ring(2000);
    zassert_equal(n_snapped, 2);
}

ZTEST(snap, test_slow_drift_tracked)
{
    /* 1.5 mT of drift over 2 minutes (a magnetised bag, temperature):
     * the baseline follows, so a knock at the end isn't a snap. */
    int32_t drift[3] = {0, 0, 0};

    for (int step = 0; step < 120; step++) {
        drift[2] = step * 1500 / 120;
        run(1000, 0, drift, false, false);
    }
    zassert_true(s.dev_ut < 100, "baseline lagging by %u µT", s.dev_ut);
    knock(3000);
    run(2000, 0, drift, false, false);
    zassert_equal(n_snapped, 0);
}

ZTEST(snap, test_baseline_frozen_while_moving)
{
    int32_t base[3];
    const int32_t shift[3] = {0, 500, 0};

    off_ring(5000);
    /* Carried around: the field changes, but the baseline holds. */
    run(10000, 0, shift, true, false);
    mn_snap_baseline(&s, base);
    zassert_true(base[1] > -15 && base[1] < 5, "baseline moved to %d", base[1]);
    zassert_true(s.dev_ut > 450);
    /* Put down: now it follows. */
    run(30000, 0, shift, false, false);
    mn_snap_baseline(&s, base);
    zassert_true(base[1] > 480, "baseline at %d", base[1]);
}

ZTEST(snap, test_baseline_frozen_while_snapped)
{
    int32_t base[3];

    off_ring(5000);
    knock(3000);
    on_ring(60000); /* a minute on the phone */
    mn_snap_baseline(&s, base);
    zassert_true(base[2] > 40 && base[2] < 50, "baseline at %d", base[2]);
    zassert_equal(n_unsnapped, 0);
}

ZTEST(snap, test_unknocked_magnet_absorbed)
{
    /* Left next to a magnet with no knock: after a while it's the new
     * normal, and taking the puck away from it isn't an unsnap either. */
    off_ring(5000);
    on_ring(60000);
    zassert_true(s.dev_ut < 50, "still %u µT off", s.dev_ut);
    knock(3000);
    on_ring(2000);
    zassert_equal(n_snapped, 0);
    zassert_equal(n_unsnapped, 0);
}

ZTEST(snap, test_first_sample_is_baseline)
{
    int32_t base[3];
    const int32_t b[3] = {-30000, 12345, 80000};

    zassert_equal(mn_snap_sample(&s, b, false, false, 0), MN_SNAP_NONE);
    mn_snap_baseline(&s, base);
    zassert_equal(base[0], -30000);
    zassert_equal(base[1], 12345);
    zassert_equal(base[2], 80000);
    zassert_equal(s.dev_ut, 0);
}

ZTEST(snap, test_full_scale_fields)
{
    /* ±80 mT (TMAG5273's top range) on every axis: no overflow. */
    const int32_t lo[3] = {-80000, -80000, -80000};
    const int32_t hi[3] = {80000, 80000, 80000};

    mn_snap_sample(&s, lo, false, false, 0);
    mn_snap_shock(&s, 4000, 50);
    for (int64_t t = 50; t <= 500; t += DT_MS) {
        if (mn_snap_sample(&s, hi, true, false, t) == MN_SNAP_SNAPPED) {
            n_snapped++;
        }
    }
    /* |(160, 160, 160) mT| = 277.1 mT */
    zassert_true(s.dev_ut >= 277128 && s.dev_ut <= 277129, "dev %u", s.dev_ut);
    zassert_equal(n_snapped, 1);
}

ZTEST_SUITE(snap, NULL, NULL, reset, NULL, NULL);
