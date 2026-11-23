/*
 * Recording store (stage 1 section 4.4): writes encrypted .mnr recordings to
 * a mounted filesystem (LittleFS), and repairs the ones a power cut left
 * open.
 *
 *   <dir>/<rec_id as 16 hex digits>.part   while recording
 *   <dir>/<rec_id as 16 hex digits>.mnr    once closed or recovered
 *
 * Audio goes into a 4 KB plaintext chunk in RAM; each full chunk is sealed
 * and appended with fs_sync(), so a power cut loses at most the chunk being
 * filled (about 2 s). The CEK lives only in a PSA key while the recording is
 * open: after mn_rec_close() the puck can no longer read it, and that's why
 * recovery works on ciphertext only (see mn_recstore_recover()).
 *
 * struct mn_rec is over 8 KB: keep it in static storage.
 * psa_crypto_init() must have succeeded first.
 */
#ifndef MICNOTE_STORAGE_RECSTORE_H_
#define MICNOTE_STORAGE_RECSTORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>

#include <psa/crypto.h>

#include "mnr.h"
#include "wrap.h"

/* Room for "<dir>/<16 hex>.part" with a dir of up to 40 characters. */
#define MN_RECSTORE_PATH_MAX 64

/* Plaintext chunk types (first byte of every chunk). */
#define MN_CHUNK_AUDIO 0x01
#define MN_CHUNK_TRAILER 0x02
/* In an audio chunk, a length of 0xFFFF introduces a marker instead of a
 * packet: 0xFFFF | type u8 | offset_ms u32. */
#define MN_CHUNK_MARKER_SENTINEL 0xFFFF
#define MN_CHUNK_MARKER_SIZE 7
/* The trailer chunk: type | duration_ms u32 | marker_count u32. */
#define MN_CHUNK_TRAILER_SIZE 9

struct mn_rec_recipient {
    uint8_t pub[MN_X25519_KEY_SIZE];
    uint8_t host_id[8];
};

struct mn_rec_params {
    uint64_t rec_id;
    uint8_t flags;
    uint8_t frame_ms;
    uint32_t start_boot;
    uint64_t start_uptime_ms;
    uint64_t start_unix_ms;
    /* Paired hosts, or just the device fallback key: 1 ..= MN_MNR_MAX_WRAPS. */
    uint8_t n_recipients;
    struct mn_rec_recipient recipients[MN_MNR_MAX_WRAPS];
};

/* The randomness one recording needs. Filled by mn_rec_secrets_random() in
 * the app, or with fixed bytes by tests; mn_rec_open() always wipes it. */
struct mn_rec_secrets {
    uint8_t cek[MN_CEK_SIZE];
    uint8_t eph_priv[MN_MNR_MAX_WRAPS][MN_X25519_KEY_SIZE];
};

struct mn_rec {
    struct fs_file_t file;
    char path[MN_RECSTORE_PATH_MAX];
    uint64_t rec_id;
    uint32_t seq;
    uint32_t duration_ms;
    uint32_t marker_count;
    uint8_t frame_ms;
    psa_key_id_t cek;
    uint8_t core_hash[32];
    size_t pt_len;
    uint8_t pt[MN_MNR_MAX_CT];
    uint8_t record[MN_MNR_CHUNK_HDR_SIZE + MN_MNR_MAX_CT + MN_MNR_TAG_SIZE];
    bool open;
};

/* Creates dir if it doesn't exist. */
int mn_recstore_init(const char *dir);

int mn_rec_secrets_random(struct mn_rec_secrets *s);

/* Wraps the CEK to every recipient, writes and syncs the header to
 * <dir>/<id>.part. Returns 0, -EEXIST if that recording exists, -EINVAL for
 * bad params, or a negative errno from the crypto or filesystem. */
int mn_rec_open(struct mn_rec *r, const char *dir, const struct mn_rec_params *p,
                struct mn_rec_secrets *s);

/* Appends one Opus packet (1 ..= 0xFFFE bytes); adds frame_ms to the
 * duration. */
int mn_rec_append(struct mn_rec *r, const uint8_t *packet, size_t len);

/* Records an item marker (stage 1 section 4.2). */
int mn_rec_marker(struct mn_rec *r, uint8_t type, uint32_t offset_ms);

/* Flushes the last audio, writes the trailer chunk, renames .part to .mnr,
 * and destroys the CEK. On failure the .part file stays for recovery. */
int mn_rec_close(struct mn_rec *r);

/*
 * Run at boot, before recording. For every .part file:
 * - truncates it after the last complete chunk record (checked by length and
 *   sequence only: the puck can't authenticate chunks without the CEK);
 * - renames it to .mnr. It has no trailer, which the host reads as "ended by
 *   a power cut" and works out the duration from the audio itself.
 * A .part whose header never made it to flash holds no key wraps, so nothing
 * in it can ever be decrypted: that one file is deleted.
 * Returns the number of recordings recovered, or a negative errno.
 */
int mn_recstore_recover(const char *dir);

/* ---- reading, for sync (docs/protocol.md section 5) ---------------------- */

/* What the puck knows about a stored recording without decrypting it. The
 * duration and marker count are in the encrypted trailer, so only the host
 * learns them. */
struct mn_recstore_info {
    uint64_t rec_id;
    uint32_t start_boot;
    uint64_t start_uptime_ms;
    uint64_t start_unix_ms;
    uint32_t size_bytes;
    /* Number of chunk records; last_seq is chunks - 1. */
    uint32_t chunks;
    /* False while it is still a .part file. */
    bool complete;
};

/* Up to max recordings with rec_id > after_id, in ascending order; *more is
 * set if there are others after them. Returns how many, or a negative errno. */
int mn_recstore_list(const char *dir, uint64_t after_id, struct mn_recstore_info *out, size_t max,
                     bool *more);

/* Reads a stored recording record by record, for Fetch. */
struct mn_rec_reader {
    struct fs_file_t file;
    off_t off;
    uint32_t next_seq;
    bool open;
};

/* Opens recording rec_id (finished or still being written) and reads its
 * header into header[cap]. Returns the header length, -ENOENT, or another
 * negative errno. */
int mn_rec_reader_open(struct mn_rec_reader *r, const char *dir, uint64_t rec_id, uint8_t *header,
                       size_t cap);

/* The next chunk record with seq >= from_seq: its seq and stored payload
 * (ciphertext + tag). Returns the payload length, 0 at the end, or a negative
 * errno. */
int mn_rec_reader_next(struct mn_rec_reader *r, uint32_t from_seq, uint32_t *seq, uint8_t *payload,
                       size_t cap);

void mn_rec_reader_close(struct mn_rec_reader *r);

/* Deletes finished recording rec_id if sha256 matches the SHA-256 of its
 * whole file (the Ack rule). Returns 0, -ENOENT, -EBUSY if it's still being
 * written, -EBADMSG on a hash mismatch, or another negative errno. */
int mn_recstore_ack(const char *dir, uint64_t rec_id, const uint8_t sha256[32]);

#endif /* MICNOTE_STORAGE_RECSTORE_H_ */
