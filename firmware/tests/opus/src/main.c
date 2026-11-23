/*
 * libopus at the stage 1 §4.2 settings: VOIP, 16 kHz mono, 20 ms frames,
 * 16 kbps VBR, complexity 3. The input is the shared synthetic voice
 * (tests/common/voice.h), so every platform feeds the encoder the same
 * samples, and the fixed-point encoder must then give the same packets.
 */
#include <opus.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "voice.h"

#define RATE 16000
#define FRAME 320  /* 20 ms */
#define FRAMES 150 /* 3 s */
#define SAMPLES (FRAME * FRAMES)
#define MAX_PACKET 128
#define BITRATE 16000
#ifndef COMPLEXITY
#define COMPLEXITY 3
#endif
#define STACK_BUDGET 32768 /* §4.2's encoder thread stack */
/* The test thread gets more than the budget, so going over it shows up as
 * a number in the log rather than a stack-overflow fault. */
#define STACK_BYTES (4 * STACK_BUDGET)
#define STATE_BYTES (32 * 1024)

/*
 * CRC-32 of the packet stream (each packet as u16 length + bytes) for this
 * input and these settings. It changes only when libopus's output changes,
 * so a new libopus pin may need a new value; a mismatch on one platform
 * only means that platform's build of libopus computes differently.
 */
#define PACKETS_CRC 0x21f1f4c2u

static int16_t pcm_in[SAMPLES];
static int16_t pcm_out[SAMPLES];
static uint8_t packets[FRAMES][MAX_PACKET];
static int packet_len[FRAMES];

static uint8_t enc_state[STATE_BYTES] __aligned(8);
static uint8_t dec_state[STATE_BYTES] __aligned(8);

/* ---- encode on a thread with the budgeted stack ----------------------- */

K_THREAD_STACK_DEFINE(enc_stack, STACK_BYTES);
static struct k_thread enc_thread;
static int enc_result;

static void encode_all(void *a, void *b, void *c)
{
    OpusEncoder *enc = (OpusEncoder *)enc_state;
    int err = opus_encoder_init(enc, RATE, 1, OPUS_APPLICATION_VOIP);

    if (err == OPUS_OK) {
        err = opus_encoder_ctl(enc, OPUS_SET_BITRATE(BITRATE));
    }
    if (err == OPUS_OK) {
        err = opus_encoder_ctl(enc, OPUS_SET_VBR(1));
    }
    if (err == OPUS_OK) {
        err = opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(COMPLEXITY));
    }
    for (int f = 0; err == OPUS_OK && f < FRAMES; f++) {
        packet_len[f] = opus_encode(enc, &pcm_in[f * FRAME], FRAME, packets[f], MAX_PACKET);
        if (packet_len[f] < 0) {
            err = packet_len[f];
        }
    }
    enc_result = err;
}

static size_t stack_used;

static void *setup(void)
{
    size_t unused;

    zassert_true(opus_encoder_get_size(1) <= STATE_BYTES);
    zassert_true(opus_decoder_get_size(1) <= STATE_BYTES);

    mn_voice(pcm_in, SAMPLES);
    k_thread_create(&enc_thread, enc_stack, K_THREAD_STACK_SIZEOF(enc_stack), encode_all, NULL,
                    NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
    k_thread_join(&enc_thread, K_FOREVER);
    zassert_ok(k_thread_stack_space_get(&enc_thread, &unused));
    stack_used = K_THREAD_STACK_SIZEOF(enc_stack) - unused;

    TC_PRINT("%s\n", opus_get_version_string());
    TC_PRINT("encoder state %d B, decoder state %d B, encode stack %zu B\n",
             opus_encoder_get_size(1), opus_decoder_get_size(1), stack_used);
    return NULL;
}

/* ---- the checks -------------------------------------------------------- */

ZTEST(opus, test_encodes_within_budget)
{
    zassert_equal(enc_result, OPUS_OK, "encode failed: %d", enc_result);
    if (IS_ENABLED(CONFIG_ARCH_POSIX)) {
        /* native_sim runs threads on host stacks, not on enc_stack. */
        ztest_test_skip();
    }
    /* Leave a quarter of the budget for whatever the encoder thread does
     * around opus_encode() (handing packets on, logging). */
    zassert_true(stack_used <= STACK_BUDGET * 3 / 4, "encode used %zu B of stack", stack_used);
}

ZTEST(opus, test_bitrate_near_target)
{
    int total = 0;

    for (int f = 0; f < FRAMES; f++) {
        zassert_true(packet_len[f] > 0 && packet_len[f] <= MAX_PACKET);
        total += packet_len[f];
    }

    int bps = total * 8 * 1000 / (FRAMES * 20);

    TC_PRINT("%d bytes in %d packets: %d bps, %d B per packet\n", total, FRAMES, bps,
             total / FRAMES);
    /* VBR on a signal that's silent 30% of the time: allow ±40%. */
    zassert_within(bps, BITRATE, BITRATE * 2 / 5, "%d bps", bps);
}

ZTEST(opus, test_decodes_back)
{
    OpusDecoder *dec = (OpusDecoder *)dec_state;
    OpusEncoder *enc = (OpusEncoder *)enc_state;
    opus_int32 delay;
    int64_t sig = 0;
    int64_t err = 0;

    zassert_ok(opus_decoder_init(dec, RATE, 1));
    for (int f = 0; f < FRAMES; f++) {
        int n = opus_decode(dec, packets[f], packet_len[f], &pcm_out[f * FRAME], FRAME, 0);

        zassert_equal(n, FRAME, "frame %d decoded to %d", f, n);
    }

    /* The output lags the input by the encoder's lookahead. Compare from
     * 0.5 s on, after the codec has settled. */
    zassert_ok(opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&delay)));
    for (int n = RATE / 2; n + delay < SAMPLES; n++) {
        int32_t d = pcm_out[n + delay] - pcm_in[n];

        sig += (int32_t)pcm_in[n] * pcm_in[n];
        err += d * d;
    }

    /* A speech codec at ~12 kbps keeps how a voice sounds, not its exact
     * waveform: this signal comes back at about 6 dB SNR. Misaligned by a
     * few ms, or decoded from garbage, the error is larger than the signal
     * itself, so 3 dB separates "the same voice" from "something else". */
    TC_PRINT("lookahead %d samples, signal/error energy %lld\n", (int)delay,
             (long long)(err ? sig / err : -1));
    zassert_true(err * 2 < sig, "SNR under 3 dB: signal %lld, error %lld", (long long)sig,
                 (long long)err);
}

ZTEST(opus, test_packets_bit_exact)
{
    uint32_t crc = 0;

    for (int f = 0; f < FRAMES; f++) {
        uint8_t len[2] = {(uint8_t)packet_len[f], (uint8_t)(packet_len[f] >> 8)};

        crc = crc32_ieee_update(crc, len, sizeof(len));
        crc = crc32_ieee_update(crc, packets[f], packet_len[f]);
    }
    TC_PRINT("packet stream CRC-32 0x%08x\n", crc);
    zassert_equal(crc, PACKETS_CRC, "packet stream CRC-32 0x%08x", crc);
}

ZTEST_SUITE(opus, NULL, setup, NULL, NULL, NULL);
