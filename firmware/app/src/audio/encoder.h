/*
 * The Opus encoder at the stage 1 §4.2 settings (VOIP, 16 kHz mono, 20 ms
 * frames, 16 kbps VBR), behind a push interface: feed it mono samples in
 * blocks of any size (the capture gives 10 ms ones), and every time a whole
 * 20 ms frame has built up it is encoded and the packet goes to the sink.
 *
 * Not thread-safe; pipeline.c runs it on the encoder thread. Opus's scratch
 * memory is on the calling thread's stack (VAR_ARRAYS): give that thread the
 * 32 KB that §4.2 budgets.
 *
 * struct mn_enc holds the whole encoder state (about 15 KB): keep it in
 * static storage.
 */
#ifndef MICNOTE_AUDIO_ENCODER_H_
#define MICNOTE_AUDIO_ENCODER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MN_ENC_RATE 16000
#define MN_ENC_FRAME_MS 20
#define MN_ENC_FRAME_SAMPLES (MN_ENC_RATE / 1000 * MN_ENC_FRAME_MS)
#define MN_ENC_BITRATE 16000
#define MN_ENC_COMPLEXITY_DEFAULT 3
/* Far above what 16 kbps VBR produces (about 40 bytes); Opus fits the packet
 * to the buffer if it ever had to. */
#define MN_ENC_MAX_PACKET 256
/* opus_encoder_get_size(1) is 15228 bytes for libopus 1.6.1 fixed point;
 * mn_enc_init() checks it. */
#define MN_ENC_STATE_SIZE 16384

/* Takes one packet. A negative return stops the encoder: mn_enc_push() and
 * mn_enc_flush() return it. */
typedef int (*mn_enc_sink_t)(void *ctx, const uint8_t *packet, size_t len);

struct mn_enc {
    uint8_t state[MN_ENC_STATE_SIZE] __attribute__((aligned(8)));
    int16_t frame[MN_ENC_FRAME_SAMPLES];
    size_t fill;
    uint64_t samples; /* pushed so far */
    uint32_t packets; /* given to the sink so far */
    int err;          /* first error; sticky */
    mn_enc_sink_t sink;
    void *ctx;
};

/* complexity 0 ..= 10. Returns 0, -ENOMEM if this libopus needs more state
 * than MN_ENC_STATE_SIZE, -EINVAL, or -EIO for an Opus error. */
int mn_enc_init(struct mn_enc *e, int complexity, mn_enc_sink_t sink, void *ctx);

/* Adds n mono samples; encodes every frame they complete. 0 or the first
 * error (Opus: -EIO; or the sink's), after which it does nothing. */
int mn_enc_push(struct mn_enc *e, const int16_t *pcm, size_t n);

/* Pads a part-filled frame with silence and encodes it, so the recording
 * keeps the last up to 19 ms. Nothing to do on a frame boundary. */
int mn_enc_flush(struct mn_enc *e);

/* How much audio has been pushed, in ms: the offset for an item marker
 * placed now (§4.2). */
uint32_t mn_enc_position_ms(const struct mn_enc *e);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_AUDIO_ENCODER_H_ */
