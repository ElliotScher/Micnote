/*
 * Button gestures (stage 1 §4.11), as a pure state machine: feed it debounced
 * button edges and the time, and it calls back with each gesture.
 *
 *   TAP     press shorter than MN_LONG_MS, then no second press within
 *           MN_DOUBLE_GAP_MS of the release; reported when that gap ends
 *   DOUBLE  a second press within MN_DOUBLE_GAP_MS; reported at that press
 *   LONG    held MN_LONG_MS; reported while still held, so the marker buzz
 *           comes at once
 *   PAIR    released after MN_PAIR_MS but before MN_RESET_MS
 *   RESET   held MN_RESET_MS; reported while still held
 *
 * A hold reports LONG first and then, depending on when it ends, PAIR or
 * RESET. The recognizer doesn't know the device state: the caller decides
 * what each gesture means (LONG is a marker only while recording, RESET needs
 * USB power, and so on).
 *
 * Timers: after every call, mn_gestures_deadline() says when to call
 * mn_gestures_poll() next. A late poll is fine: mn_gestures_input() runs any
 * expired timers first, so gestures come out in order.
 *
 * Debouncing is the input driver's job (gpio-keys' debounce-interval-ms).
 */
#ifndef MICNOTE_UI_GESTURES_H_
#define MICNOTE_UI_GESTURES_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MN_DOUBLE_GAP_MS 400
#define MN_LONG_MS 1000
#define MN_PAIR_MS 5000
#define MN_RESET_MS 20000

enum mn_gesture {
    MN_GESTURE_TAP,
    MN_GESTURE_DOUBLE,
    MN_GESTURE_LONG,
    MN_GESTURE_PAIR,
    MN_GESTURE_RESET,
};

typedef void (*mn_gesture_fn)(enum mn_gesture gesture, void *ctx);

enum mn_gestures_state {
    MN_GESTURES_IDLE,
    MN_GESTURES_HELD,    /* first press, still down */
    MN_GESTURES_GAP,     /* short press released; waiting for a second */
    MN_GESTURES_SWALLOW, /* second press of a double, still down */
};

struct mn_gestures {
    enum mn_gestures_state state;
    int64_t since_ms; /* when the current state began */
    bool long_sent;
    bool reset_sent;
    mn_gesture_fn cb;
    void *ctx;
};

void mn_gestures_init(struct mn_gestures *g, mn_gesture_fn cb, void *ctx);

/* A debounced edge: pressed is the button's new state. An edge that doesn't
 * change the state is ignored. */
void mn_gestures_input(struct mn_gestures *g, bool pressed, int64_t now_ms);

/* Runs the timers due by now_ms. */
void mn_gestures_poll(struct mn_gestures *g, int64_t now_ms);

/* When the next timer is due, or -1 if none is pending. */
int64_t mn_gestures_deadline(const struct mn_gestures *g);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_UI_GESTURES_H_ */
