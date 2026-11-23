#include "preproc.h"

#include <stdlib.h>

#include <zephyr/ztest.h>

#include "voice.h"

#define RATE 16000

static struct mn_preproc p;
static int16_t in[2 * RATE];
static int16_t out[RATE];

/* n samples of a sine at freq_hz with the given peak, in channel ch of
 * `channels` (the other channel, if any, gets the same). */
static void sine(int16_t *buf, size_t n, uint8_t channels, int32_t freq_hz, int32_t peak)
{
    uint32_t phase = 0;
    const uint32_t step = (uint32_t)(((uint64_t)freq_hz << 32) / RATE);

    for (size_t i = 0; i < n; i++) {
        int16_t s = (int16_t)((mn_sine_q15(phase) * peak) >> 15);

        for (uint8_t c = 0; c < channels; c++) {
            buf[i * channels + c] = s;
        }
        phase += step;
    }
}

/* Peak of |out| over the last half of n samples, after the filter settled. */
static int32_t settled_peak(size_t n)
{
    int32_t peak = 0;

    for (size_t i = n / 2; i < n; i++) {
        peak = MAX(peak, abs(out[i]));
    }
    return peak;
}

ZTEST(preproc, test_stereo_mixes_to_mean)
{
    const int16_t lr[] = {1000, 3000, -32768, -32768, 32767, 32767};

    mn_preproc_init(&p, MN_PREPROC_GAIN_UNITY);
    mn_preproc_run(&p, lr, 1, 2, out);
    /* The filter's first output is its first input. */
    zassert_equal(out[0], 2000);

    /* Full-scale samples of the same sign don't overflow the mix. */
    mn_preproc_init(&p, MN_PREPROC_GAIN_UNITY);
    mn_preproc_run(&p, &lr[2], 1, 2, out);
    zassert_equal(out[0], -32768);
    mn_preproc_init(&p, MN_PREPROC_GAIN_UNITY);
    mn_preproc_run(&p, &lr[4], 1, 2, out);
    zassert_equal(out[0], 32767);
}

ZTEST(preproc, test_removes_dc)
{
    for (size_t i = 0; i < RATE; i++) {
        in[i] = 10000;
    }
    mn_preproc_init(&p, MN_PREPROC_GAIN_UNITY);
    mn_preproc_run(&p, in, RATE, 1, out);
    /* The time constant is 1/(2*pi*80) = 2 ms; after 1 s nothing is left. */
    for (size_t i = RATE - 100; i < RATE; i++) {
        zassert_true(abs(out[i]) <= 1, "out[%zu] = %d", i, out[i]);
    }
}

ZTEST(preproc, test_passes_voice_band)
{
    sine(in, RATE, 1, 1000, 16000);
    mn_preproc_init(&p, MN_PREPROC_GAIN_UNITY);
    mn_preproc_run(&p, in, RATE, 1, out);
    /* 1 kHz is 0.997 of the way through an 80 Hz one-pole high-pass. */
    zassert_within(settled_peak(RATE), 16000, 16000 / 50, "%d", settled_peak(RATE));
}

ZTEST(preproc, test_cuts_rumble)
{
    sine(in, RATE, 1, 20, 16000);
    mn_preproc_init(&p, MN_PREPROC_GAIN_UNITY);
    mn_preproc_run(&p, in, RATE, 1, out);
    /* 20 Hz: 20 / sqrt(20^2 + 80^2) = 0.24. */
    zassert_within(settled_peak(RATE), 16000 * 24 / 100, 16000 / 20, "%d", settled_peak(RATE));
}

ZTEST(preproc, test_gain)
{
    sine(in, RATE, 2, 1000, 8000);
    mn_preproc_init(&p, 2 * MN_PREPROC_GAIN_UNITY);
    mn_preproc_run(&p, in, RATE, 2, out);
    zassert_within(settled_peak(RATE), 16000, 16000 / 50, "%d", settled_peak(RATE));
}

ZTEST(preproc, test_gain_saturates)
{
    static int16_t ref[RATE];

    sine(in, RATE, 1, 1000, 16000);
    mn_preproc_init(&p, MN_PREPROC_GAIN_UNITY);
    mn_preproc_run(&p, in, RATE, 1, ref);
    mn_preproc_init(&p, 16 * MN_PREPROC_GAIN_UNITY);
    mn_preproc_run(&p, in, RATE, 1, out);

    /* Wherever 16x the unity output is far out of range, the sample is
     * clipped to the rail on the same side, never wrapped to the other. */
    for (size_t i = 0; i < RATE; i++) {
        if (ref[i] > 2100) {
            zassert_equal(out[i], INT16_MAX, "out[%zu] = %d", i, out[i]);
        } else if (ref[i] < -2100) {
            zassert_equal(out[i], INT16_MIN, "out[%zu] = %d", i, out[i]);
        }
    }
}

ZTEST(preproc, test_in_place)
{
    mn_voice(in, RATE);
    mn_preproc_init(&p, MN_PREPROC_GAIN_UNITY);
    mn_preproc_run(&p, in, RATE, 1, out);

    mn_preproc_init(&p, MN_PREPROC_GAIN_UNITY);
    mn_preproc_run(&p, in, RATE, 1, in);
    zassert_mem_equal(in, out, RATE * sizeof(int16_t));
}

ZTEST_SUITE(preproc, NULL, NULL, NULL, NULL, NULL);
