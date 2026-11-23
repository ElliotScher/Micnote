#include "patterns.h"

#include <zephyr/sys/util.h>

#define OFF {0, 0, 0}
#define RED {255, 0, 0}
#define GREEN {0, 255, 0}
#define BLUE {0, 0, 255}
#define WHITE {255, 255, 255}
#define AMBER {255, 120, 0}

#define HOLD(c, t) {.color = c, .ms = (t)}
#define FADE(c, t) {.color = c, .ms = (t), .fade = true}

/* ---- LED tables (§4.11) ------------------------------------------------- */

static const struct mn_led_step rec_start[] = {HOLD(RED, 1000)};
static const struct mn_led_step recording[] = {HOLD(RED, 150), HOLD(OFF, 2850)};
static const struct mn_led_step rec_stop[] = {HOLD(GREEN, 500)};
static const struct mn_led_step marker[] = {HOLD(WHITE, 120)};
static const struct mn_led_step sync_done[] = {HOLD(GREEN, 150), HOLD(OFF, 150), HOLD(GREEN, 150)};
static const struct mn_led_step storage_warn[] = {HOLD(AMBER, 200), HOLD(OFF, 200),
                                                  HOLD(AMBER, 200)};
static const struct mn_led_step storage_full[] = {HOLD(RED, 100), HOLD(OFF, 100), HOLD(RED, 100),
                                                  HOLD(OFF, 100), HOLD(RED, 100)};
static const struct mn_led_step pairing[] = {FADE(BLUE, 1000), FADE(OFF, 1000)};
static const struct mn_led_step low_battery[] = {HOLD(RED, 100), HOLD(OFF, 9900)};

#define PATTERN(s, l) {.steps = (s), .n_steps = ARRAY_SIZE(s), .loop = (l)}

static const struct mn_led_pattern leds[MN_UI_EVENT_COUNT] = {
    [MN_UI_REC_START] = PATTERN(rec_start, false),
    [MN_UI_RECORDING] = PATTERN(recording, true),
    [MN_UI_REC_STOP] = PATTERN(rec_stop, false),
    [MN_UI_MARKER] = PATTERN(marker, false),
    [MN_UI_SYNC_DONE] = PATTERN(sync_done, false),
    [MN_UI_STORAGE_WARN] = PATTERN(storage_warn, false),
    [MN_UI_STORAGE_FULL] = PATTERN(storage_full, false),
    [MN_UI_PAIRING] = PATTERN(pairing, true),
    [MN_UI_LOW_BATTERY] = PATTERN(low_battery, true),
};

/* ---- haptic tables ------------------------------------------------------ */

static const uint16_t one_short[] = {40};
static const uint16_t two_short[] = {40, 100, 40};
static const uint16_t one_long[] = {400};

#define BUZZ(s) {.ms = (s), .n = ARRAY_SIZE(s)}

static const struct mn_haptic_pattern buzz_one_short = BUZZ(one_short);
static const struct mn_haptic_pattern buzz_two_short = BUZZ(two_short);
static const struct mn_haptic_pattern buzz_one_long = BUZZ(one_long);

static const struct mn_haptic_pattern *const haptics[MN_UI_EVENT_COUNT] = {
    [MN_UI_REC_START] = &buzz_one_short,   [MN_UI_REC_STOP] = &buzz_one_short,
    [MN_UI_MARKER] = &buzz_two_short,      [MN_UI_SYNC_DONE] = &buzz_one_long,
    [MN_UI_STORAGE_FULL] = &buzz_one_long,
};

const struct mn_led_pattern *mn_ui_led(enum mn_ui_event event)
{
    return (unsigned int)event < MN_UI_EVENT_COUNT ? &leds[event] : NULL;
}

const struct mn_haptic_pattern *mn_ui_haptic(enum mn_ui_event event)
{
    return (unsigned int)event < MN_UI_EVENT_COUNT ? haptics[event] : NULL;
}

/* ---- players ------------------------------------------------------------ */

uint32_t mn_led_length_ms(const struct mn_led_pattern *p)
{
    uint32_t total = 0;

    for (size_t i = 0; i < p->n_steps; i++) {
        total += p->steps[i].ms;
    }
    return total;
}

uint32_t mn_haptic_length_ms(const struct mn_haptic_pattern *p)
{
    uint32_t total = 0;

    for (size_t i = 0; i < p->n; i++) {
        total += p->ms[i];
    }
    return total;
}

static uint8_t lerp(uint8_t from, uint8_t to, uint32_t num, uint32_t den)
{
    return (uint8_t)((int32_t)from + ((int32_t)to - (int32_t)from) * (int32_t)num / (int32_t)den);
}

bool mn_led_at(const struct mn_led_pattern *p, uint32_t t_ms, struct mn_rgb *out)
{
    const struct mn_rgb off = OFF;
    const uint32_t length = mn_led_length_ms(p);

    *out = off;
    if (length == 0 || (!p->loop && t_ms >= length)) {
        return false;
    }
    if (p->loop) {
        t_ms %= length;
    }
    for (size_t i = 0; i < p->n_steps; i++) {
        const struct mn_led_step *s = &p->steps[i];

        if (t_ms >= s->ms) {
            t_ms -= s->ms;
            continue;
        }
        if (!s->fade) {
            *out = s->color;
            return true;
        }
        /* The colour this ramp starts from: the step before it, wrapping
         * round for a loop. */
        const struct mn_rgb from = i > 0     ? p->steps[i - 1].color
                                   : p->loop ? p->steps[p->n_steps - 1].color
                                             : off;

        out->r = lerp(from.r, s->color.r, t_ms, s->ms);
        out->g = lerp(from.g, s->color.g, t_ms, s->ms);
        out->b = lerp(from.b, s->color.b, t_ms, s->ms);
        return true;
    }
    return false; /* unreachable: t_ms < length */
}

bool mn_haptic_at(const struct mn_haptic_pattern *p, uint32_t t_ms, bool *on)
{
    *on = false;
    for (size_t i = 0; i < p->n; i++) {
        if (t_ms < p->ms[i]) {
            *on = (i % 2) == 0;
            return true;
        }
        t_ms -= p->ms[i];
    }
    return false;
}

/* ---- status reply ------------------------------------------------------- */

uint8_t mn_ui_blinks(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    return pct <= 20 ? 1 : (uint8_t)((pct + 19) / 20);
}

struct mn_led_pattern mn_ui_status(uint8_t battery_pct, uint8_t storage_free_pct,
                                   struct mn_led_step steps[MN_STATUS_MAX_STEPS])
{
    static const struct mn_led_step green_on = HOLD(GREEN, 200);
    static const struct mn_led_step blue_on = HOLD(BLUE, 200);
    static const struct mn_led_step gap = HOLD(OFF, 300);
    static const struct mn_led_step pause = HOLD(OFF, 1000);
    size_t n = 0;

    /* Each group: blink, gap, blink, ... with no gap after the last blink. */
    for (uint8_t i = mn_ui_blinks(battery_pct); i > 0; i--) {
        steps[n++] = green_on;
        if (i > 1) {
            steps[n++] = gap;
        }
    }
    steps[n++] = pause;
    for (uint8_t i = mn_ui_blinks(storage_free_pct); i > 0; i--) {
        steps[n++] = blue_on;
        if (i > 1) {
            steps[n++] = gap;
        }
    }
    return (struct mn_led_pattern){.steps = steps, .n_steps = n, .loop = false};
}
