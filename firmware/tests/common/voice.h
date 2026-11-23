/*
 * A synthetic voice for the audio suites (tests/opus, tests/audio), made with
 * integer arithmetic only, so every platform produces the same samples and
 * the fixed-point encoder must then give the same packets.
 *
 * Pitch glides 120 -> 220 Hz and back over the whole signal, harmonics fall
 * at 1/k up to 3.5 kHz, there are four "syllables" a second, and a little
 * noise. 16 kHz mono.
 */
#ifndef MICNOTE_TESTS_COMMON_VOICE_H_
#define MICNOTE_TESTS_COMMON_VOICE_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#define MN_VOICE_RATE 16000

/* A sine-like wave in Q15: a parabola per half period, 4x(1 - x). Its shape
 * is off a true sine by a few percent, which doesn't matter for a test
 * voice, and it needs no libm (whose sin() differs between C libraries). */
static inline int32_t mn_sine_q15(uint32_t phase)
{
    int32_t u = (int32_t)((phase & 0x7fffffffu) >> 16); /* 0..32767 */
    int32_t y = MIN((4 * u * (32768 - u)) >> 15, 32767);

    return (phase & 0x80000000u) ? -y : y;
}

/* Fills out[0 .. n). The glide is a triangle over all n samples, so a
 * different n gives a different signal. */
static inline void mn_voice(int16_t *out, size_t n)
{
    const int32_t total = (int32_t)n;
    const int32_t syl = MN_VOICE_RATE / 4;
    const int32_t voiced = syl * 7 / 10;
    uint32_t phase = 0;
    uint32_t noise = 12345;

    for (int32_t i = 0; i < total; i++) {
        /* f0 in milli-Hz */
        int32_t tri = i < total / 2 ? i : total - i;
        int64_t f0_mhz = 120000 + (int64_t)100000 * tri / (total / 2);
        int32_t f0_hz = (int32_t)(f0_mhz / 1000);
        int32_t acc = 0;

        phase += (uint32_t)((f0_mhz << 32) / ((int64_t)MN_VOICE_RATE * 1000));
        for (int k = 1; k * f0_hz < 3500; k++) {
            acc += mn_sine_q15(phase * (uint32_t)k) / k;
        }

        /* syllable envelope: 4 Hz, raised half-sine, on 70% of the time */
        int32_t pos = i % syl;
        int32_t env = 0;

        if (pos < voiced) {
            env = mn_sine_q15((uint32_t)(((uint64_t)pos << 31) / voiced));
        }

        noise = noise * 1664525u + 1013904223u;
        int32_t s = (int32_t)(((int64_t)acc * env) >> 18) + ((int32_t)(noise >> 16) - 32768) / 128;

        out[i] = (int16_t)CLAMP(s, INT16_MIN, INT16_MAX);
    }
}

#endif /* MICNOTE_TESTS_COMMON_VOICE_H_ */
