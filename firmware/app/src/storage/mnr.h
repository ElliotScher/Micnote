/*
 * The .mnr recording file format (docs/protocol.md section 6): byte layout
 * only. Nothing here touches flash or crypto; recstore.c writes the bytes,
 * crypto/ seals the chunks. All integers are little-endian.
 *
 *   header = core (immutable) | wrap table (fixed size, rewritable)
 *   then chunk records: seq u32 | ct_len u16 | ciphertext[ct_len] | tag[16]
 *
 * Chunks authenticate only the core (AAD = SHA-256(core) | seq), so the wrap
 * table can be rewritten in place when the device fallback wrap is replaced
 * by a host's at first pairing.
 */
#ifndef MICNOTE_STORAGE_MNR_H_
#define MICNOTE_STORAGE_MNR_H_

#include <stddef.h>
#include <stdint.h>

#define MN_MNR_VERSION 1
#define MN_MNR_CODEC_OPUS 1
#define MN_MNR_FLAG_STEREO 0x01U

/* Up to 4 paired hosts; the device fallback wrap only exists while none is. */
#define MN_MNR_MAX_WRAPS 4

/* magic 4 | version 1 | hdr_len 2 | flags 1 | rec_id 8 | codec 1 | rate_hz 2
 * | frame_ms 1 | start_boot 4 | start_uptime_ms 8 | start_unix_ms 8 */
#define MN_MNR_CORE_SIZE 40
/* host_id 8 | eph_pub 32 | wrapped_cek 32 | tag 16 */
#define MN_MNR_WRAP_SIZE 88
/* core | n_wraps 1 | MN_MNR_MAX_WRAPS wrap slots (unused slots are zero) */
#define MN_MNR_HEADER_SIZE (MN_MNR_CORE_SIZE + 1 + MN_MNR_MAX_WRAPS * MN_MNR_WRAP_SIZE)
/* The largest hdr_len a reader accepts: FetchDone.header's max_size. A later
 * v1 writer may append fields after the wrap table, up to this. */
#define MN_MNR_HEADER_MAX 512

#define MN_MNR_CHUNK_HDR_SIZE 6
#define MN_MNR_TAG_SIZE 16
/* Plaintext chunks flush at 4 KB; CCM ciphertext is the same length. */
#define MN_MNR_MAX_CT 4096

struct mn_mnr_wrap {
    uint8_t host_id[8];
    /* X25519 ephemeral public key, one per wrap so a wrap can be replaced. */
    uint8_t eph_pub[32];
    uint8_t wrapped_cek[32];
    uint8_t tag[16];
};

struct mn_mnr_header {
    uint8_t flags;
    uint64_t rec_id;
    uint8_t codec;
    uint16_t rate_hz;
    uint8_t frame_ms;
    uint32_t start_boot;
    uint64_t start_uptime_ms;
    uint64_t start_unix_ms;
    uint8_t n_wraps;
    struct mn_mnr_wrap wraps[MN_MNR_MAX_WRAPS];
};

struct mn_mnr_chunk_hdr {
    uint32_t seq;
    uint16_t ct_len;
};

/* Writes MN_MNR_HEADER_SIZE bytes. Returns that size, -ENOSPC if cap is too
 * small, or -EINVAL if n_wraps > MN_MNR_MAX_WRAPS. */
int mn_mnr_header_write(const struct mn_mnr_header *h, uint8_t *buf, size_t cap);

/* Parses a header from the start of buf. Returns hdr_len (the bytes to skip
 * to reach the first chunk record), or:
 * -ENODATA if len is shorter than the header claims or needs,
 * -ENOTSUP for a version other than MN_MNR_VERSION,
 * -EBADMSG for a bad magic, an hdr_len outside MN_MNR_HEADER_SIZE ..
 *  MN_MNR_HEADER_MAX, or n_wraps > MN_MNR_MAX_WRAPS. */
int mn_mnr_header_parse(const uint8_t *buf, size_t len, struct mn_mnr_header *out);

void mn_mnr_chunk_hdr_write(const struct mn_mnr_chunk_hdr *c, uint8_t out[MN_MNR_CHUNK_HDR_SIZE]);

/* Returns 0, or -EBADMSG if ct_len is 0 (every chunk has a type byte) or
 * above MN_MNR_MAX_CT. */
int mn_mnr_chunk_hdr_parse(const uint8_t in[MN_MNR_CHUNK_HDR_SIZE], struct mn_mnr_chunk_hdr *out);

#endif /* MICNOTE_STORAGE_MNR_H_ */
