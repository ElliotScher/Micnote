#include "gestures.h"

#include <string.h>

void mn_gestures_init(struct mn_gestures *g, mn_gesture_fn cb, void *ctx)
{
    memset(g, 0, sizeof(*g));
    g->state = MN_GESTURES_IDLE;
    g->cb = cb;
    g->ctx = ctx;
}

static void enter(struct mn_gestures *g, enum mn_gestures_state state, int64_t now_ms)
{
    g->state = state;
    g->since_ms = now_ms;
    g->long_sent = false;
    g->reset_sent = false;
}

int64_t mn_gestures_deadline(const struct mn_gestures *g)
{
    switch (g->state) {
        case MN_GESTURES_HELD:
            if (!g->long_sent) {
                return g->since_ms + MN_LONG_MS;
            }
            if (!g->reset_sent) {
                return g->since_ms + MN_RESET_MS;
            }
            return -1;
        case MN_GESTURES_GAP:
            return g->since_ms + MN_DOUBLE_GAP_MS;
        default:
            return -1;
    }
}

void mn_gestures_poll(struct mn_gestures *g, int64_t now_ms)
{
    /* Each step either fires one timer or finds none due; at most three
     * (LONG, RESET, or the tap gap) can be due at once. */
    for (;;) {
        const int64_t due = mn_gestures_deadline(g);

        if (due < 0 || now_ms < due) {
            return;
        }
        if (g->state == MN_GESTURES_GAP) {
            enter(g, MN_GESTURES_IDLE, due);
            g->cb(MN_GESTURE_TAP, g->ctx);
        } else if (!g->long_sent) {
            g->long_sent = true;
            g->cb(MN_GESTURE_LONG, g->ctx);
        } else {
            g->reset_sent = true;
            g->cb(MN_GESTURE_RESET, g->ctx);
        }
    }
}

static void on_press(struct mn_gestures *g, int64_t now_ms)
{
    switch (g->state) {
        case MN_GESTURES_IDLE:
            enter(g, MN_GESTURES_HELD, now_ms);
            break;
        case MN_GESTURES_GAP:
            enter(g, MN_GESTURES_SWALLOW, now_ms);
            g->cb(MN_GESTURE_DOUBLE, g->ctx);
            break;
        default:
            break; /* already down */
    }
}

static void on_release(struct mn_gestures *g, int64_t now_ms)
{
    switch (g->state) {
        case MN_GESTURES_HELD: {
            const int64_t held = now_ms - g->since_ms;

            if (held < MN_LONG_MS) {
                enter(g, MN_GESTURES_GAP, now_ms);
                return;
            }
            const bool pair = held >= MN_PAIR_MS && !g->reset_sent;

            enter(g, MN_GESTURES_IDLE, now_ms);
            if (pair) {
                g->cb(MN_GESTURE_PAIR, g->ctx);
            }
            break;
        }
        case MN_GESTURES_SWALLOW:
            enter(g, MN_GESTURES_IDLE, now_ms);
            break;
        default:
            break; /* already up */
    }
}

void mn_gestures_input(struct mn_gestures *g, bool pressed, int64_t now_ms)
{
    mn_gestures_poll(g, now_ms);
    if (pressed) {
        on_press(g, now_ms);
    } else {
        on_release(g, now_ms);
    }
}
