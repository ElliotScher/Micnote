/*
 * Timekeeping (stage 1 §4.10): the boot counter, the wall clock a host sets,
 * and recording IDs.
 *
 * - boot_count lives in settings (NVS) under "mn/boot" and goes up by one at
 *   every mn_time_init().
 * - The wall clock is invalid until a host sends SetTime; after that,
 *   unix_ms = base_unix_ms + (uptime - base_uptime_ms). It's lost at reset.
 * - Recording IDs are (boot_count << 32) | n, with n = 1, 2, ... per boot, so
 *   they never repeat and sort in recording order.
 *
 * The clock math is the pure mn_clock_* functions; the mn_time_* functions
 * are one global instance of it on k_uptime_get().
 */
#ifndef MICNOTE_TIMEKEEPING_H_
#define MICNOTE_TIMEKEEPING_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mn_clock {
    bool valid;
    uint64_t base_unix_ms;
    int64_t base_uptime_ms;
    int32_t tz_offset_min;
};

/* Sets the clock: unix_ms is the time at uptime_ms. unix_ms == 0 is "no
 * time" and leaves the clock invalid. */
void mn_clock_set(struct mn_clock *c, uint64_t unix_ms, int32_t tz_offset_min, int64_t uptime_ms);

/* The Unix time at uptime_ms, or 0 while the clock isn't set. */
uint64_t mn_clock_unix_ms(const struct mn_clock *c, int64_t uptime_ms);

/* Loads boot_count from settings, adds one and saves it. Call once at boot,
 * after settings_subsys_init(). 0 or a negative errno; on error the device
 * still runs, with boot_count 0 for this boot. */
int mn_time_init(void);

uint32_t mn_time_boot_count(void);

/* The next recording ID for this boot, or 0 if this boot has used all 2^32 - 1
 * (or mn_time_init() failed: without a boot count, IDs could repeat). */
uint64_t mn_time_next_rec_id(void);

/* The global wall clock, on k_uptime_get(). */
void mn_time_set(uint64_t unix_ms, int32_t tz_offset_min);
bool mn_time_valid(void);
uint64_t mn_time_unix_ms(void);
int32_t mn_time_tz_offset_min(void);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_TIMEKEEPING_H_ */
