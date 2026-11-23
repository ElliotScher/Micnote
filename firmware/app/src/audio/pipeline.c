#include "pipeline.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "preproc.h"

K_THREAD_STACK_DEFINE(audio_stack, MN_AUDIO_STACK_SIZE);
static struct k_thread audio_thread;

static struct mn_enc enc;
static struct mn_preproc pre;
static struct mn_audio_source source;
/* Mono output of one block. preproc can work in place, but the block
 * belongs to the source (the DMIC slab), so it isn't written to. */
static int16_t mono[MN_AUDIO_BLOCK_FRAMES];

static atomic_t running;  /* a thread was started and not yet collected */
static atomic_t stopping; /* mn_audio_stop() asked it to end */
static atomic_t done;     /* the thread has returned */
static atomic_t position_ms;
static atomic_t blocks;
static int result;

static void audio_main(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    int rc = 0;

    while (rc == 0 && !atomic_get(&stopping)) {
        int16_t *buf;
        int r = source.read(source.ctx, &buf);

        if (r == -EAGAIN) {
            continue;
        }
        if (r == -ENODATA) {
            break;
        }
        if (r < 0) {
            rc = r;
            break;
        }

        mn_preproc_run(&pre, buf, MN_AUDIO_BLOCK_FRAMES, source.channels, mono);
        source.release(source.ctx, buf);
        atomic_inc(&blocks);
        rc = mn_enc_push(&enc, mono, MN_AUDIO_BLOCK_FRAMES);
        atomic_set(&position_ms, (atomic_val_t)mn_enc_position_ms(&enc));
    }

    if (rc == 0) {
        rc = mn_enc_flush(&enc);
    }
    result = rc;
    atomic_set(&done, 1);
}

int mn_audio_start(const struct mn_audio_source *src, const struct mn_audio_config *cfg)
{
    if (src == NULL || cfg == NULL || src->read == NULL || src->release == NULL ||
        (src->channels != 1 && src->channels != 2)) {
        return -EINVAL;
    }
    if (!atomic_cas(&running, 0, 1)) {
        return -EBUSY;
    }

    int rc = mn_enc_init(&enc, cfg->complexity, cfg->sink, cfg->sink_ctx);

    if (rc < 0) {
        atomic_set(&running, 0);
        return rc;
    }
    mn_preproc_init(&pre, cfg->gain_q8);
    source = *src;
    result = 0;
    atomic_set(&stopping, 0);
    atomic_set(&done, 0);
    atomic_set(&position_ms, 0);
    atomic_set(&blocks, 0);

    k_thread_create(&audio_thread, audio_stack, K_THREAD_STACK_SIZEOF(audio_stack), audio_main,
                    NULL, NULL, NULL, MN_AUDIO_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&audio_thread, "encoder");
    return 0;
}

static int collect(bool ask_to_stop)
{
    if (!atomic_get(&running)) {
        return -EALREADY;
    }
    if (ask_to_stop) {
        atomic_set(&stopping, 1);
    }
    k_thread_join(&audio_thread, K_FOREVER);
    atomic_set(&running, 0);
    return result;
}

int mn_audio_stop(void)
{
    return collect(true);
}

int mn_audio_wait(void)
{
    return collect(false);
}

bool mn_audio_done(void)
{
    return atomic_get(&running) && atomic_get(&done);
}

uint32_t mn_audio_position_ms(void)
{
    return (uint32_t)atomic_get(&position_ms);
}

uint32_t mn_audio_blocks(void)
{
    return (uint32_t)atomic_get(&blocks);
}
