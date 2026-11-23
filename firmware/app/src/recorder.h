/*
 * The recorder: one recording from start to stop (stage 1 §4.1, §4.2, §4.9).
 *
 *   capture source -> encoder thread (prio 5, audio/pipeline.c)
 *     -> ring buffer -> storage thread (prio 6) -> recstore -> flash
 *
 * The encoder thread never writes flash: its sink puts each packet in a ring
 * buffer, and the storage thread appends from there. A LittleFS block erase
 * or a sync can take tens of ms, longer than the capture's 80 ms of slack
 * when two come together; the ring holds about 4 s of audio. Markers go
 * through the same ring, so they land in the stream exactly where they were
 * placed.
 *
 * The policy (§4.9) is applied here: mn_recorder_start() refuses when
 * storage is full or the battery is under 5%, and mn_recorder_poll()
 * finalizes the active recording when either becomes true while recording,
 * or when the encoder thread ended on its own (a capture or storage fault).
 * A finalized recording is closed normally, with its trailer, so the host
 * sees the audio up to that point.
 *
 * The mn_recorder_* functions are for one caller thread (main's state
 * machine); they aren't safe to call concurrently with each other.
 * psa_crypto_init() and mn_recstore_init() must have run first.
 */
#ifndef MICNOTE_RECORDER_H_
#define MICNOTE_RECORDER_H_

#include <stdbool.h>
#include <stdint.h>

#include "audio/pipeline.h"
#include "storage/policy.h"
#include "storage/recstore.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MN_RECORDER_RING_SIZE 8192
#define MN_RECORDER_STORAGE_PRIORITY 6

enum mn_recorder_end {
    MN_REC_END_NONE,         /* still recording (or idle) */
    MN_REC_END_USER,         /* mn_recorder_stop(), or a source that ran out */
    MN_REC_END_LOW_BATTERY,  /* battery under 5% */
    MN_REC_END_STORAGE_FULL, /* §4.9 FULL */
    MN_REC_END_FAULT,        /* capture, encoder or storage error */
};

struct mn_recorder_config {
    const char *dir; /* the recstore directory */
    struct mn_audio_source source;
    uint16_t gain_q8;
    int complexity;
};

/* Starts the storage thread. Once, at boot. */
int mn_recorder_init(const struct mn_recorder_config *cfg);

/*
 * Starts a recording with these params (ID, timestamps, recipients). Returns
 * 0; -EBUSY if one is active; -ENOSPC if storage is FULL; -EPERM if the
 * battery is under 5%; or an error from recstore or the encoder.
 */
int mn_recorder_start(const struct mn_rec_params *p, enum mn_storage_state storage,
                      uint8_t battery_pct);

/* Stops the active recording: encodes what's left, writes it, closes the
 * file. Returns 0 or the first error the recording hit; -EALREADY if idle. */
int mn_recorder_stop(void);

/* Marks an item at the current position (§4.2). -EALREADY if idle,
 * -ENOBUFS if the ring is full. */
int mn_recorder_marker(uint8_t type);

/*
 * Call periodically while recording (main does it on each battery or
 * storage update) with the current battery and storage state. Finalizes the
 * active recording if the policy says so or the encoder thread died, and
 * returns why; MN_REC_END_NONE if nothing happened. *err (if not NULL) gets
 * the recording's error: 0 if it closed cleanly, always non-zero for
 * MN_REC_END_FAULT.
 */
enum mn_recorder_end mn_recorder_poll(enum mn_storage_state storage, uint8_t battery_pct, int *err);

bool mn_recorder_active(void);

/* The active recording's ID, or 0. */
uint64_t mn_recorder_rec_id(void);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_RECORDER_H_ */
