#include "gestures.h"

#include <zephyr/ztest.h>

static struct mn_gestures g;
static enum mn_gesture got[8];
static size_t n_got;

static void record(enum mn_gesture gesture, void *ctx)
{
    zassert_equal_ptr(ctx, &g);
    zassert_true(n_got < ARRAY_SIZE(got), "too many gestures");
    got[n_got++] = gesture;
}

static void reset(void *fixture)
{
    ARG_UNUSED(fixture);
    mn_gestures_init(&g, record, &g);
    n_got = 0;
}

static void press(int64_t t)
{
    mn_gestures_input(&g, true, t);
}

static void release(int64_t t)
{
    mn_gestures_input(&g, false, t);
}

/* Polls at every deadline up to t, as the firmware's timer would. */
static void run_until(int64_t t)
{
    for (int64_t due = mn_gestures_deadline(&g); due >= 0 && due <= t;
         due = mn_gestures_deadline(&g)) {
        mn_gestures_poll(&g, due);
    }
    mn_gestures_poll(&g, t);
}

#define expect(...)                                                           \
    do {                                                                      \
        const enum mn_gesture want[] = {__VA_ARGS__};                         \
        zassert_equal(n_got, ARRAY_SIZE(want), "got %zu gestures", n_got);    \
        for (size_t i = 0; i < ARRAY_SIZE(want); i++) {                       \
            zassert_equal(got[i], want[i], "gesture %zu: got %d", i, got[i]); \
        }                                                                     \
    } while (0)

ZTEST(gestures, test_idle_has_no_deadline)
{
    zassert_equal(mn_gestures_deadline(&g), -1);
    run_until(100000);
    zassert_equal(n_got, 0);
}

ZTEST(gestures, test_tap_waits_for_the_gap)
{
    press(1000);
    release(1150);
    zassert_equal(mn_gestures_deadline(&g), 1150 + MN_DOUBLE_GAP_MS);
    run_until(1150 + MN_DOUBLE_GAP_MS - 1);
    zassert_equal(n_got, 0, "tap reported before the double-press gap ended");
    run_until(1150 + MN_DOUBLE_GAP_MS);
    expect(MN_GESTURE_TAP);
    zassert_equal(mn_gestures_deadline(&g), -1);
}

ZTEST(gestures, test_double)
{
    press(0);
    release(100);
    press(100 + MN_DOUBLE_GAP_MS - 1);
    expect(MN_GESTURE_DOUBLE);
    /* Holding the second press doesn't make it a long press. */
    run_until(30000);
    release(30000);
    run_until(40000);
    expect(MN_GESTURE_DOUBLE);
}

ZTEST(gestures, test_second_press_after_gap_is_two_taps)
{
    press(0);
    release(100);
    press(100 + MN_DOUBLE_GAP_MS); /* gap expires first, even without a poll */
    release(200 + MN_DOUBLE_GAP_MS);
    run_until(10000);
    expect(MN_GESTURE_TAP, MN_GESTURE_TAP);
}

ZTEST(gestures, test_triple_press_is_double_then_tap)
{
    press(0);
    release(100);
    press(200);
    release(300);
    press(400);
    release(500);
    run_until(10000);
    expect(MN_GESTURE_DOUBLE, MN_GESTURE_TAP);
}

ZTEST(gestures, test_long_fires_while_held)
{
    press(0);
    run_until(MN_LONG_MS - 1);
    zassert_equal(n_got, 0);
    run_until(MN_LONG_MS);
    expect(MN_GESTURE_LONG);
    release(MN_LONG_MS + 500);
    run_until(10000);
    expect(MN_GESTURE_LONG);
}

ZTEST(gestures, test_just_short_of_long_is_tap)
{
    press(0);
    release(MN_LONG_MS - 1);
    run_until(10000);
    expect(MN_GESTURE_TAP);
}

ZTEST(gestures, test_late_poll_keeps_order)
{
    /* Nobody polled during the hold: the release still reports LONG first. */
    press(0);
    release(MN_PAIR_MS + 10);
    expect(MN_GESTURE_LONG, MN_GESTURE_PAIR);
}

ZTEST(gestures, test_pair_on_release)
{
    press(0);
    run_until(MN_PAIR_MS + 2000);
    expect(MN_GESTURE_LONG);
    release(MN_PAIR_MS + 2000);
    expect(MN_GESTURE_LONG, MN_GESTURE_PAIR);
}

ZTEST(gestures, test_just_short_of_pair_is_only_long)
{
    press(0);
    release(MN_PAIR_MS - 1);
    run_until(30000);
    expect(MN_GESTURE_LONG);
}

ZTEST(gestures, test_reset_while_held_and_no_pair)
{
    press(0);
    run_until(MN_RESET_MS - 1);
    expect(MN_GESTURE_LONG);
    run_until(MN_RESET_MS);
    expect(MN_GESTURE_LONG, MN_GESTURE_RESET);
    zassert_equal(mn_gestures_deadline(&g), -1);
    release(MN_RESET_MS + 3000);
    run_until(60000);
    expect(MN_GESTURE_LONG, MN_GESTURE_RESET);
}

ZTEST(gestures, test_one_late_poll_fires_every_due_timer)
{
    press(0);
    mn_gestures_poll(&g, MN_RESET_MS + 1);
    expect(MN_GESTURE_LONG, MN_GESTURE_RESET);
}

ZTEST(gestures, test_repeated_edges_ignored)
{
    release(0); /* up while already up */
    press(10);
    press(20); /* down while already down */
    release(100);
    release(110);
    run_until(10000);
    expect(MN_GESTURE_TAP);
}

ZTEST(gestures, test_repeated_press_during_double_ignored)
{
    press(0);
    release(100);
    press(200);
    press(250);
    release(300);
    run_until(10000);
    expect(MN_GESTURE_DOUBLE);
}

ZTEST(gestures, test_hold_after_tap_gap_is_long)
{
    press(0);
    release(100);
    run_until(1000);
    press(2000);
    run_until(2000 + MN_LONG_MS);
    expect(MN_GESTURE_TAP, MN_GESTURE_LONG);
}

ZTEST(gestures, test_large_times)
{
    /* Uptime starts near 0, but nothing assumes it stays small. */
    const int64_t t0 = INT64_C(1) << 40;

    press(t0);
    release(t0 + 50);
    run_until(t0 + 1000);
    expect(MN_GESTURE_TAP);
}

ZTEST_SUITE(gestures, NULL, NULL, reset, NULL, NULL);
