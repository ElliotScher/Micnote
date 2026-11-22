/*
 * Link framing (docs/protocol.md section 2): every message is a varint
 * length followed by an encoded micnote.v1.Frame. The same byte stream runs
 * over L2CAP, the GATT fallback and USB CDC, and a frame may arrive split
 * across any number of transport packets, so decoding is incremental.
 * The host's mirror is core/src/protocol/framing.{hpp,cpp}.
 *
 * No heap: the reassembly buffer lives inside struct mn_framer, sized for
 * the largest frame. Keep the struct and every micnote_v1_Frame (about
 * 4.2 KB each) in static storage, not on a thread stack.
 */
#ifndef MICNOTE_PROTO_LINK_FRAMING_H_
#define MICNOTE_PROTO_LINK_FRAMING_H_

#include <stddef.h>
#include <stdint.h>

#include "micnote.pb.h"

/* Bytes in the length prefix of the largest frame. A longer prefix is
 * rejected rather than parsed. */
#define MN_FRAME_MAX_PREFIX 2
/* The largest frame on the wire: prefix plus body. */
#define MN_FRAME_MAX_WIRE (MN_FRAME_MAX_PREFIX + micnote_v1_Frame_size)

_Static_assert(micnote_v1_Frame_size < (1U << (7U * MN_FRAME_MAX_PREFIX)),
               "MN_FRAME_MAX_PREFIX no longer covers micnote_v1_Frame_size");

/* Reassembles frames from one link's byte stream. After any error the stream
 * has lost sync: every later mn_framer_next() returns the same error, and
 * the caller should drop the link. */
struct mn_framer {
    uint8_t buf[MN_FRAME_MAX_WIRE];
    size_t len; /* bytes in buf */
    int err;    /* 0, or the sticky negative errno */
};

void mn_framer_init(struct mn_framer *f);

/* Copies as much of data as fits and returns how many bytes it took. The
 * buffer holds one largest frame, so if this takes fewer than n bytes, drain
 * mn_framer_next() and push the rest. */
size_t mn_framer_push(struct mn_framer *f, const uint8_t *data, size_t n);

/* Returns 1 with *out filled, 0 if more bytes are needed, or a sticky error:
 * -EPROTO if the length prefix is longer than MN_FRAME_MAX_PREFIX bytes,
 * -EMSGSIZE if the length is larger than micnote_v1_Frame_size,
 * -EBADMSG if the body isn't a valid Frame. */
int mn_framer_next(struct mn_framer *f, micnote_v1_Frame *out);

/* Writes the length prefix and encoded frame to buf. Returns the number of
 * bytes written, or -EINVAL if nanopb can't encode it into cap bytes (e.g.
 * a string without its NUL). MN_FRAME_MAX_WIRE is always enough. */
int mn_frame_encode(const micnote_v1_Frame *in, uint8_t *buf, size_t cap);

#endif /* MICNOTE_PROTO_LINK_FRAMING_H_ */
