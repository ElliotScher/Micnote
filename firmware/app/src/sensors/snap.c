#include "snap.h"

#include <string.h>

void mn_snap_init(struct mn_snap *s, const struct mn_snap_cfg *cfg)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    s->changed_since_ms = -1;
    s->back_since_ms = -1;
}

void mn_snap_shock(struct mn_snap *s, uint32_t peak_mg, int64_t t_ms)
{
    if (peak_mg >= s->cfg.g_snap_mg) {
        s->have_shock = true;
        s->shock_ms = t_ms;
    }
}

void mn_snap_baseline(const struct mn_snap *s, int32_t out_ut[3])
{
    for (int i = 0; i < 3; i++) {
        out_ut[i] = (int32_t)(s->base_q[i] / ((int64_t)1 << s->cfg.ema_shift));
    }
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* A shock between shock_window_ms before the field changed and now. */
static bool shock_matches(const struct mn_snap *s, int64_t t_ms)
{
    return s->have_shock && s->shock_ms >= s->changed_since_ms - s->cfg.shock_window_ms &&
           s->shock_ms <= t_ms;
}

static enum mn_snap_event while_free(struct mn_snap *s, bool on_charger, int64_t t_ms)
{
    if (s->dev_ut <= s->cfg.t_snap_ut) {
        s->changed_since_ms = -1;
        return MN_SNAP_NONE;
    }
    if (s->changed_since_ms < 0) {
        s->changed_since_ms = t_ms;
    }
    if (t_ms - s->changed_since_ms < s->cfg.confirm_ms || !shock_matches(s, t_ms)) {
        return MN_SNAP_NONE;
    }
    s->snapped = true;
    s->changed_since_ms = -1;
    s->back_since_ms = -1;
    s->have_shock = false; /* used up */
    return on_charger ? MN_SNAP_SNAPPED_CHARGER : MN_SNAP_SNAPPED;
}

static enum mn_snap_event while_snapped(struct mn_snap *s, int64_t t_ms)
{
    if (s->dev_ut >= s->cfg.t_snap_ut / 2) {
        s->back_since_ms = -1;
        return MN_SNAP_NONE;
    }
    if (s->back_since_ms < 0) {
        s->back_since_ms = t_ms;
    }
    if (t_ms - s->back_since_ms < s->cfg.release_ms) {
        return MN_SNAP_NONE;
    }
    s->snapped = false;
    s->back_since_ms = -1;
    return MN_SNAP_UNSNAPPED;
}

enum mn_snap_event mn_snap_sample(struct mn_snap *s, const int32_t field_ut[3], bool moving,
                                  bool on_charger, int64_t t_ms)
{
    const int64_t scale = (int64_t)1 << s->cfg.ema_shift;

    if (!s->have_base) {
        for (int i = 0; i < 3; i++) {
            s->base_q[i] = (int64_t)field_ut[i] * scale;
        }
        s->have_base = true;
    }

    uint64_t dev2 = 0;
    int32_t base[3];

    mn_snap_baseline(s, base);
    for (int i = 0; i < 3; i++) {
        const int64_t d = (int64_t)field_ut[i] - base[i];

        dev2 += (uint64_t)(d * d);
    }
    s->dev_ut = isqrt64(dev2);

    const enum mn_snap_event ev =
        s->snapped ? while_snapped(s, t_ms) : while_free(s, on_charger, t_ms);

    /* The baseline follows slow drift only while nothing is going on: not
     * snapped, not moving, and not in the middle of confirming a change
     * (which would soak up part of the ring's field). A change that outlasts
     * the confirmation without a knock gets absorbed like any drift. */
    const bool confirming =
        s->changed_since_ms >= 0 && t_ms - s->changed_since_ms < s->cfg.confirm_ms;

    if (!s->snapped && !moving && !confirming && ev == MN_SNAP_NONE) {
        for (int i = 0; i < 3; i++) {
            s->base_q[i] += (int64_t)field_ut[i] - base[i];
        }
    }
    return ev;
}
