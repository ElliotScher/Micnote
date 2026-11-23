/*
 * Pre-processing between the PDM capture and the encoder (stage 1 §4.2):
 *
 *   stereo (L+R)/2 mono mix -> DC-blocking high-pass (one pole, ~80 Hz)
 *   -> fixed gain with saturation
 *
 * The PDM peripheral's output carries a DC offset and very low-frequency
 * rumble (handling noise, the mic's own offset). Opus would spend bits on
 * it, and the gain stage would clip sooner, so it goes first.
 *
 * The filter is y[n] = x[n] - x[n-1] + a * y[n-1] with a = 1 - 2*pi*fc/fs.
 * Its state keeps 8 fractional bits, so small signals don't get stuck on a
 * rounding error (a "limit cycle"). Integer only, no floating point.
 */
#ifndef MICNOTE_AUDIO_PREPROC_H_
#define MICNOTE_AUDIO_PREPROC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Gain is Q8: 256 is unity, 512 is +6 dB. */
#define MN_PREPROC_GAIN_UNITY 256

struct mn_preproc {
    int32_t x_prev;    /* the previous mono input sample */
    int32_t y_prev_q8; /* the previous filter output, Q8 */
    uint16_t gain_q8;
};

void mn_preproc_init(struct mn_preproc *p, uint16_t gain_q8);

/*
 * Turns n frames of `channels` interleaved samples (1 or 2) into n mono
 * samples. in and out may be the same buffer: each output sample is written
 * only after its input frame has been read.
 */
void mn_preproc_run(struct mn_preproc *p, const int16_t *in, size_t n, uint8_t channels,
                    int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_AUDIO_PREPROC_H_ */
