/*
 * The encoder thread (stage 1 §4.1, priority 5): pulls 10 ms PCM blocks from
 * a capture source, pre-processes them to mono (preproc.h), encodes them
 * (encoder.h) and gives each Opus packet to a sink.
 *
 * The source is pdm_capture.c on the boards (Zephyr's DMIC API) and a fake
 * in the tests. The DMIC API is a pull interface: dmic_read() waits for the
 * next filled block from the driver's memory slab, and the caller frees it
 * back. The source here has the same shape.
 *
 * The sink runs on the encoder thread. Every packet that is late holds up the
 * next read, and the driver drops blocks once its slab (8 blocks, 80 ms) is
 * full, so the sink mustn't block for long: the app's sink queues packets for
 * the storage thread rather than writing flash itself.
 *
 * One recording at a time: mn_audio_start() fails with -EBUSY while one runs.
 */
#ifndef MICNOTE_AUDIO_PIPELINE_H_
#define MICNOTE_AUDIO_PIPELINE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MN_AUDIO_BLOCK_MS 10
#define MN_AUDIO_BLOCK_FRAMES (MN_ENC_RATE / 1000 * MN_AUDIO_BLOCK_MS)
#define MN_AUDIO_PRIORITY 5
/* §4.2: the encode measured 24.3 KB at complexity 3 on a Cortex-M33. */
#define MN_AUDIO_STACK_SIZE 32768

struct mn_audio_source {
    /* Waits for the next block of MN_AUDIO_BLOCK_FRAMES frames of `channels`
     * interleaved samples. Returns 0 with *buf set; -EAGAIN if none came in
     * time (the thread checks for a stop and asks again); -ENODATA when the
     * source has ended (tests); any other negative errno is a fault and ends
     * the recording. */
    int (*read)(void *ctx, int16_t **buf);
    /* Gives a block from read() back. */
    void (*release)(void *ctx, int16_t *buf);
    void *ctx;
    uint8_t channels; /* 1 or 2 */
};

struct mn_audio_config {
    uint16_t gain_q8;
    int complexity;
    mn_enc_sink_t sink;
    void *sink_ctx;
};

/* Starts the encoder thread. 0, -EBUSY, or an error from mn_enc_init(). */
int mn_audio_start(const struct mn_audio_source *src, const struct mn_audio_config *cfg);

/* Asks the thread to stop, waits for it to encode what it has (including the
 * last partial frame) and end. Returns 0 or the first error the recording
 * hit; -EALREADY if nothing was started. Also the way to collect a thread
 * that ended on its own (an error, or -ENODATA from the source). */
int mn_audio_stop(void);

/* Like mn_audio_stop(), but doesn't ask the thread to stop: waits for it to
 * end by itself (the source returned -ENODATA or an error). For sources that
 * end, such as the simulator's demo memos; a stop right after a start would
 * usually beat the lower-priority thread to its first read. */
int mn_audio_wait(void);

/* True once a started thread has ended by itself (end of input or an error)
 * and is waiting to be collected with mn_audio_stop() or mn_audio_wait(). */
bool mn_audio_done(void);

/* ms of audio encoded since the start: the offset for a marker placed now.
 * Safe from any thread. */
uint32_t mn_audio_position_ms(void);

/* Blocks read from the source since the start. Diagnostics. */
uint32_t mn_audio_blocks(void);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_AUDIO_PIPELINE_H_ */
