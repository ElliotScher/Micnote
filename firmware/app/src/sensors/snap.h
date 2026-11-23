/*
 * Snap detection (stage 1 §4.12) as a pure state machine: feed it
 * magnetometer samples, accelerometer shocks and the charger line, and it
 * says when the puck snapped onto a MagSafe ring and when it came off.
 *
 * - Baseline B0: an exponential moving average of the field, moving 1/2^ema_shift
 *   of the way per sample, and only while not snapped, not moving and not
 *   confirming a change (the confirm_ms after the field first jumps). The
 *   first sample sets it.
 * - SNAPPED: |B - B0| > t_snap_ut for at least confirm_ms, with a shock of at
 *   least g_snap_mg between shock_window_ms before the field first jumped
 *   and now. The knock comes as the magnet grabs, so it can arrive just
 *   before the field moves or during the confirmation.
 * - UNSNAPPED: |B - B0| < t_snap_ut / 2 for release_ms.
 * - On the charger: a snap while the Qi power-good line is active is
 *   SNAPPED_CHARGER, which the caller doesn't treat as a phone (no ADV_FAST).
 *
 * Which samples get here (20 Hz for 2 s after a motion wake-up) is the
 * sensor driver's business; this file only sees the stream. Fields are in
 * µT (1 mT = 1000 µT), accelerations in mg.
 */
#ifndef MICNOTE_SENSORS_SNAP_H_
#define MICNOTE_SENSORS_SNAP_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mn_snap_cfg {
    uint32_t t_snap_ut;       /* field change that counts as a snap */
    uint32_t g_snap_mg;       /* shock that must go with it */
    uint32_t confirm_ms;      /* how long the field must stay changed */
    uint32_t shock_window_ms; /* how early the shock may come */
    uint32_t release_ms;      /* how long the field must be back for UNSNAPPED */
    uint8_t ema_shift;        /* baseline speed: 1/2^shift per sample */
};

/* §4.12's starting values; to be tuned on an iPhone and a Pixel. */
#define MN_SNAP_CFG_DEFAULT     \
    {                           \
        .t_snap_ut = 1000,      \
        .g_snap_mg = 2000,      \
        .confirm_ms = 300,      \
        .shock_window_ms = 500, \
        .release_ms = 1000,     \
        .ema_shift = 6,         \
    }

enum mn_snap_event {
    MN_SNAP_NONE,
    MN_SNAP_SNAPPED,
    MN_SNAP_SNAPPED_CHARGER,
    MN_SNAP_UNSNAPPED,
};

struct mn_snap {
    struct mn_snap_cfg cfg;
    bool have_base;
    int64_t base_q[3]; /* B0 in µT, scaled by 2^ema_shift */
    bool snapped;
    int64_t changed_since_ms; /* field beyond t_snap since; -1 if not */
    int64_t back_since_ms;    /* field within t_snap/2 since; -1 if not */
    bool have_shock;
    int64_t shock_ms; /* the latest shock of at least g_snap */
    uint32_t dev_ut;  /* |B - B0| at the last sample, for trace logging */
};

void mn_snap_init(struct mn_snap *s, const struct mn_snap_cfg *cfg);

/* An accelerometer shock with this peak, at t_ms. Shocks and samples must
 * come in time order. */
void mn_snap_shock(struct mn_snap *s, uint32_t peak_mg, int64_t t_ms);

/* A magnetometer sample. `moving` is the accelerometer's activity flag (it
 * freezes the baseline); `on_charger` is the Qi power-good line. */
enum mn_snap_event mn_snap_sample(struct mn_snap *s, const int32_t field_ut[3], bool moving,
                                  bool on_charger, int64_t t_ms);

/* B0, in µT (for trace logging and tests). */
void mn_snap_baseline(const struct mn_snap *s, int32_t out_ut[3]);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_SENSORS_SNAP_H_ */
