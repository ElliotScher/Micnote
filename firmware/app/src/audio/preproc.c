#include "preproc.h"

#include <zephyr/sys/util.h>

/* a = 1 - 2*pi*80/16000 = 0.96858, in Q15. */
#define HPF_A_Q15 31738

void mn_preproc_init(struct mn_preproc *p, uint16_t gain_q8)
{
    p->x_prev = 0;
    p->y_prev_q8 = 0;
    p->gain_q8 = gain_q8;
}

void mn_preproc_run(struct mn_preproc *p, const int16_t *in, size_t n, uint8_t channels,
                    int16_t *out)
{
    for (size_t i = 0; i < n; i++) {
        int32_t x = channels == 2 ? (in[2 * i] + in[2 * i + 1]) >> 1 : in[i];

        /* |y| stays under 2^17 (a step of 2^16 at most, decaying), so y in
         * Q8 fits easily; the product with a needs 64 bits. */
        int32_t y_q8 =
            ((x - p->x_prev) << 8) + (int32_t)(((int64_t)HPF_A_Q15 * p->y_prev_q8) >> 15);

        p->x_prev = x;
        p->y_prev_q8 = y_q8;

        /* Q8 output times Q8 gain: shift out 16 bits, rounding to nearest. */
        int64_t s = ((int64_t)y_q8 * p->gain_q8 + (1 << 15)) >> 16;

        out[i] = (int16_t)CLAMP(s, INT16_MIN, INT16_MAX);
    }
}
