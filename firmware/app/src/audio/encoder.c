#include "encoder.h"

#include <errno.h>
#include <opus.h>
#include <string.h>

#include <zephyr/sys/util.h>

int mn_enc_init(struct mn_enc *e, int complexity, mn_enc_sink_t sink, void *ctx)
{
    OpusEncoder *enc = (OpusEncoder *)e->state;

    if (sink == NULL || complexity < 0 || complexity > 10) {
        return -EINVAL;
    }
    if (opus_encoder_get_size(1) > (int)sizeof(e->state)) {
        return -ENOMEM;
    }

    e->fill = 0;
    e->samples = 0;
    e->packets = 0;
    e->err = 0;
    e->sink = sink;
    e->ctx = ctx;

    if (opus_encoder_init(enc, MN_ENC_RATE, 1, OPUS_APPLICATION_VOIP) != OPUS_OK ||
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(MN_ENC_BITRATE)) != OPUS_OK ||
        opus_encoder_ctl(enc, OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity)) != OPUS_OK) {
        return -EIO;
    }
    return 0;
}

static int encode_frame(struct mn_enc *e)
{
    uint8_t packet[MN_ENC_MAX_PACKET];
    opus_int32 len = opus_encode((OpusEncoder *)e->state, e->frame, MN_ENC_FRAME_SAMPLES, packet,
                                 sizeof(packet));

    e->fill = 0;
    if (len < 0) {
        return -EIO;
    }
    /* A 1-byte packet is Opus's "nothing to say" (DTX is off, so that
     * shouldn't happen); it still decodes to a frame of silence and keeps the
     * timing, so it is stored like any other. */
    int rc = e->sink(e->ctx, packet, (size_t)len);

    if (rc < 0) {
        return rc;
    }
    e->packets++;
    return 0;
}

int mn_enc_push(struct mn_enc *e, const int16_t *pcm, size_t n)
{
    while (e->err == 0 && n > 0) {
        size_t take = MIN(n, MN_ENC_FRAME_SAMPLES - e->fill);

        memcpy(&e->frame[e->fill], pcm, take * sizeof(*pcm));
        e->fill += take;
        e->samples += take;
        pcm += take;
        n -= take;
        if (e->fill == MN_ENC_FRAME_SAMPLES) {
            e->err = encode_frame(e);
        }
    }
    return e->err;
}

int mn_enc_flush(struct mn_enc *e)
{
    if (e->err == 0 && e->fill > 0) {
        memset(&e->frame[e->fill], 0, (MN_ENC_FRAME_SAMPLES - e->fill) * sizeof(e->frame[0]));
        e->err = encode_frame(e);
    }
    return e->err;
}

uint32_t mn_enc_position_ms(const struct mn_enc *e)
{
    return (uint32_t)(e->samples * 1000 / MN_ENC_RATE);
}
