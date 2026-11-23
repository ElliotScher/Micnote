#include "patterns.h"

#include <zephyr/ztest.h>

static void expect_rgb(struct mn_rgb got, uint8_t r, uint8_t g, uint8_t b)
{
    zassert_true(got.r == r && got.g == g && got.b == b, "got %u,%u,%u want %u,%u,%u", got.r, got.g,
                 got.b, r, g, b);
}

/* Counts on-periods (rising edges) of a one-shot LED pattern, sampled every ms. */
static int led_blinks(const struct mn_led_pattern *p, struct mn_rgb color)
{
    struct mn_rgb c;
    bool was_on = false;
    int blinks = 0;

    for (uint32_t t = 0; mn_led_at(p, t, &c); t++) {
        const bool on = c.r != 0 || c.g != 0 || c.b != 0;

        if (on) {
            zassert_true(c.r == color.r && c.g == color.g && c.b == color.b, "wrong colour");
        }
        blinks += on && !was_on;
        was_on = on;
    }
    return blinks;
}

static int haptic_pulses(const struct mn_haptic_pattern *p)
{
    bool on;
    bool was_on = false;
    int pulses = 0;

    for (uint32_t t = 0; mn_haptic_at(p, t, &on); t++) {
        pulses += on && !was_on;
        was_on = on;
    }
    return pulses;
}

ZTEST(patterns, test_every_event_has_an_led_pattern)
{
    for (int e = 0; e < MN_UI_EVENT_COUNT; e++) {
        const struct mn_led_pattern *p = mn_ui_led((enum mn_ui_event)e);

        zassert_not_null(p, "event %d", e);
        zassert_true(p->n_steps > 0 && mn_led_length_ms(p) > 0, "event %d", e);
    }
    zassert_is_null(mn_ui_led(MN_UI_EVENT_COUNT));
    zassert_is_null(mn_ui_haptic(MN_UI_EVENT_COUNT));
}

/* The §4.11 table, as counts. */
ZTEST(patterns, test_table)
{
    const struct mn_rgb red = {255, 0, 0};
    const struct mn_rgb green = {0, 255, 0};
    const struct mn_rgb white = {255, 255, 255};
    const struct mn_rgb amber = {255, 120, 0};

    zassert_equal(led_blinks(mn_ui_led(MN_UI_REC_START), red), 1);
    zassert_equal(led_blinks(mn_ui_led(MN_UI_REC_STOP), green), 1);
    zassert_equal(mn_led_length_ms(mn_ui_led(MN_UI_REC_STOP)), 500);
    zassert_equal(led_blinks(mn_ui_led(MN_UI_MARKER), white), 1);
    zassert_equal(led_blinks(mn_ui_led(MN_UI_SYNC_DONE), green), 2);
    zassert_equal(led_blinks(mn_ui_led(MN_UI_STORAGE_WARN), amber), 2);
    zassert_equal(led_blinks(mn_ui_led(MN_UI_STORAGE_FULL), red), 3);

    zassert_equal(mn_led_length_ms(mn_ui_led(MN_UI_RECORDING)), 3000);
    zassert_true(mn_ui_led(MN_UI_RECORDING)->loop);
    zassert_equal(mn_led_length_ms(mn_ui_led(MN_UI_LOW_BATTERY)), 10000);
    zassert_true(mn_ui_led(MN_UI_LOW_BATTERY)->loop);
    zassert_true(mn_ui_led(MN_UI_PAIRING)->loop);

    zassert_equal(haptic_pulses(mn_ui_haptic(MN_UI_REC_START)), 1);
    zassert_equal(haptic_pulses(mn_ui_haptic(MN_UI_REC_STOP)), 1);
    zassert_equal(haptic_pulses(mn_ui_haptic(MN_UI_MARKER)), 2);
    zassert_equal(haptic_pulses(mn_ui_haptic(MN_UI_SYNC_DONE)), 1);
    zassert_equal(haptic_pulses(mn_ui_haptic(MN_UI_STORAGE_FULL)), 1);
    /* Long is longer than short. */
    zassert_true(mn_haptic_length_ms(mn_ui_haptic(MN_UI_SYNC_DONE)) >
                 mn_haptic_length_ms(mn_ui_haptic(MN_UI_REC_START)));
    zassert_is_null(mn_ui_haptic(MN_UI_RECORDING));
    zassert_is_null(mn_ui_haptic(MN_UI_STORAGE_WARN));
    zassert_is_null(mn_ui_haptic(MN_UI_PAIRING));
    zassert_is_null(mn_ui_haptic(MN_UI_LOW_BATTERY));
}

ZTEST(patterns, test_one_shot_ends_off)
{
    const struct mn_led_pattern *p = mn_ui_led(MN_UI_REC_STOP);
    struct mn_rgb c;

    zassert_true(mn_led_at(p, 499, &c));
    expect_rgb(c, 0, 255, 0);
    zassert_false(mn_led_at(p, 500, &c));
    expect_rgb(c, 0, 0, 0);
}

ZTEST(patterns, test_loop_repeats)
{
    const struct mn_led_pattern *p = mn_ui_led(MN_UI_RECORDING);
    struct mn_rgb c;

    zassert_true(mn_led_at(p, 0, &c));
    expect_rgb(c, 255, 0, 0);
    zassert_true(mn_led_at(p, 1000, &c));
    expect_rgb(c, 0, 0, 0);
    /* An hour in, still pulsing on the 3 s beat. */
    zassert_true(mn_led_at(p, 3600000 + 100, &c));
    expect_rgb(c, 255, 0, 0);
    zassert_true(mn_led_at(p, 3600000 + 200, &c));
    expect_rgb(c, 0, 0, 0);
}

ZTEST(patterns, test_breathe_ramps_both_ways)
{
    const struct mn_led_pattern *p = mn_ui_led(MN_UI_PAIRING);
    struct mn_rgb c;

    /* Up from the loop's last colour (off), then back down from blue. */
    zassert_true(mn_led_at(p, 0, &c));
    expect_rgb(c, 0, 0, 0);
    zassert_true(mn_led_at(p, 500, &c));
    expect_rgb(c, 0, 0, 127);
    zassert_true(mn_led_at(p, 999, &c));
    zassert_true(c.b > 250);
    zassert_true(mn_led_at(p, 1000, &c));
    expect_rgb(c, 0, 0, 255);
    zassert_true(mn_led_at(p, 1500, &c));
    expect_rgb(c, 0, 0, 128);
    zassert_true(mn_led_at(p, 2000, &c));
    expect_rgb(c, 0, 0, 0);
}

ZTEST(patterns, test_one_shot_fade_starts_from_off)
{
    const struct mn_led_step steps[] = {{.color = {200, 100, 0}, .ms = 100, .fade = true}};
    const struct mn_led_pattern p = {.steps = steps, .n_steps = 1, .loop = false};
    struct mn_rgb c;

    zassert_true(mn_led_at(&p, 50, &c));
    expect_rgb(c, 100, 50, 0);
}

ZTEST(patterns, test_empty_patterns)
{
    const struct mn_led_pattern led = {.steps = NULL, .n_steps = 0, .loop = true};
    const struct mn_haptic_pattern buzz = {.ms = NULL, .n = 0};
    struct mn_rgb c;
    bool on = true;

    zassert_false(mn_led_at(&led, 0, &c));
    zassert_false(mn_haptic_at(&buzz, 0, &on));
    zassert_false(on);
}

ZTEST(patterns, test_haptic_timing)
{
    const struct mn_haptic_pattern *p = mn_ui_haptic(MN_UI_MARKER);
    bool on;

    zassert_true(mn_haptic_at(p, 0, &on) && on);
    zassert_true(mn_haptic_at(p, 39, &on) && on);
    zassert_true(mn_haptic_at(p, 40, &on) && !on);
    zassert_true(mn_haptic_at(p, 140, &on) && on);
    zassert_false(mn_haptic_at(p, 180, &on));
    zassert_false(on);
}

ZTEST(patterns, test_blinks)
{
    zassert_equal(mn_ui_blinks(0), 1);
    zassert_equal(mn_ui_blinks(20), 1);
    zassert_equal(mn_ui_blinks(21), 2);
    zassert_equal(mn_ui_blinks(40), 2);
    zassert_equal(mn_ui_blinks(41), 3);
    zassert_equal(mn_ui_blinks(80), 4);
    zassert_equal(mn_ui_blinks(81), 5);
    zassert_equal(mn_ui_blinks(100), 5);
    zassert_equal(mn_ui_blinks(255), 5);
}

ZTEST(patterns, test_status)
{
    const struct mn_rgb green = {0, 255, 0};
    const struct mn_rgb blue = {0, 0, 255};
    struct mn_led_step steps[MN_STATUS_MAX_STEPS];

    /* Both at the maximum: exactly fills the buffer. */
    struct mn_led_pattern p = mn_ui_status(100, 100, steps);

    zassert_equal(p.n_steps, MN_STATUS_MAX_STEPS);
    zassert_false(p.loop);

    /* 3 green (battery 50 %), then 1 blue (5 % free). */
    p = mn_ui_status(50, 5, steps);
    zassert_equal(p.n_steps, 3 + 2 + 1 + 1);

    struct mn_rgb c;
    int green_blinks = 0;
    int blue_blinks = 0;
    bool was_on = false;

    for (uint32_t t = 0; mn_led_at(&p, t, &c); t++) {
        const bool on = c.g != 0 || c.b != 0;

        if (on && !was_on) {
            if (c.g == green.g && c.b == 0) {
                zassert_equal(blue_blinks, 0, "green after blue");
                green_blinks++;
            } else {
                zassert_true(c.b == blue.b && c.g == 0);
                blue_blinks++;
            }
        }
        was_on = on;
    }
    zassert_equal(green_blinks, 3);
    zassert_equal(blue_blinks, 1);
}

ZTEST_SUITE(patterns, NULL, NULL, NULL, NULL, NULL);
