/*
 * LED and haptic patterns for the UI events of stage 1 §4.11, as data, plus
 * pure players that say what the LED or the motor should be doing t ms into
 * a pattern. The drivers (led.c, haptic.c) sample those; this file never
 * touches hardware.
 *
 * Colours are full scale; the LED driver scales brightness for the board.
 * Which pattern wins when two overlap (a marker blink during the recording
 * pulse, say) is the caller's choice: play the one-shot, then go back to the
 * loop.
 */
#ifndef MICNOTE_UI_PATTERNS_H_
#define MICNOTE_UI_PATTERNS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mn_rgb {
    uint8_t r, g, b;
};

/* Holds `color` for `ms`, or, with `fade`, ramps to it from the previous
 * step's colour (off before the first step of a one-shot). */
struct mn_led_step {
    struct mn_rgb color;
    uint16_t ms;
    bool fade;
};

struct mn_led_pattern {
    const struct mn_led_step *steps;
    size_t n_steps;
    bool loop;
};

/* Alternating on/off durations, starting with on. */
struct mn_haptic_pattern {
    const uint16_t *ms;
    size_t n;
};

enum mn_ui_event {
    MN_UI_REC_START,    /* solid red; then MN_UI_RECORDING */
    MN_UI_RECORDING,    /* loop: a red pulse every 3 s */
    MN_UI_REC_STOP,     /* green 0.5 s */
    MN_UI_MARKER,       /* white blink */
    MN_UI_SYNC_DONE,    /* green double blink */
    MN_UI_STORAGE_WARN, /* amber, 2 blinks */
    MN_UI_STORAGE_FULL, /* red, 3 fast blinks */
    MN_UI_PAIRING,      /* loop: blue breathe */
    MN_UI_LOW_BATTERY,  /* loop: a red blink every 10 s */
    MN_UI_EVENT_COUNT,
};

/* The LED pattern for an event. Never NULL for a valid event. */
const struct mn_led_pattern *mn_ui_led(enum mn_ui_event event);

/* The haptic pattern for an event, or NULL if it has none. */
const struct mn_haptic_pattern *mn_ui_haptic(enum mn_ui_event event);

/* Total length of one pass of a pattern, in ms. */
uint32_t mn_led_length_ms(const struct mn_led_pattern *p);
uint32_t mn_haptic_length_ms(const struct mn_haptic_pattern *p);

/* The LED colour t_ms into the pattern. Returns false (and off) once a
 * one-shot pattern has finished; a loop never finishes. */
bool mn_led_at(const struct mn_led_pattern *p, uint32_t t_ms, struct mn_rgb *out);

/* Whether the motor is on t_ms into the pattern. Returns false (and off) once
 * it has finished. */
bool mn_haptic_at(const struct mn_haptic_pattern *p, uint32_t t_ms, bool *on);

/* The status reply to a double-press: 1-5 green blinks for the battery, a
 * pause, then 1-5 blue blinks for free storage (more blinks = more left, for
 * both). Writes into steps, which needs MN_STATUS_MAX_STEPS entries, and
 * returns the pattern over them. */
#define MN_STATUS_MAX_STEPS 19 /* 5 blinks + 4 gaps, a pause, 5 blinks + 4 gaps */
struct mn_led_pattern mn_ui_status(uint8_t battery_pct, uint8_t storage_free_pct,
                                   struct mn_led_step steps[MN_STATUS_MAX_STEPS]);

/* 1-5 blinks for a 0-100 % level: 1 for 0-20 %, ..., 5 for 81-100 %. */
uint8_t mn_ui_blinks(uint8_t pct);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_UI_PATTERNS_H_ */
