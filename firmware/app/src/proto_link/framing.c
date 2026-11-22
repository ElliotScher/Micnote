#include "framing.h"

#include <errno.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

void mn_framer_init(struct mn_framer *f)
{
    f->len = 0;
    f->err = 0;
}

size_t mn_framer_push(struct mn_framer *f, const uint8_t *data, size_t n)
{
    const size_t space = sizeof(f->buf) - f->len;
    const size_t take = n < space ? n : space;

    memcpy(&f->buf[f->len], data, take);
    f->len += take;
    return take;
}

static int fail(struct mn_framer *f, int err)
{
    f->err = err;
    f->len = 0;
    return err;
}

int mn_framer_next(struct mn_framer *f, micnote_v1_Frame *out)
{
    if (f->err != 0) {
        return f->err;
    }

    /* The varint length prefix: 7 bits per byte, low bits first, high bit
     * set on every byte but the last. */
    size_t length = 0;
    size_t prefix = 0;

    for (;; prefix++) {
        if (prefix == f->len) {
            return 0;
        }
        if (prefix == MN_FRAME_MAX_PREFIX) {
            return fail(f, -EPROTO);
        }
        const uint8_t byte = f->buf[prefix];

        length |= (size_t)(byte & 0x7FU) << (7U * prefix);
        if ((byte & 0x80U) == 0U) {
            prefix++;
            break;
        }
    }
    if (length > micnote_v1_Frame_size) {
        return fail(f, -EMSGSIZE);
    }
    if (f->len - prefix < length) {
        return 0;
    }

    /* micnote_v1_Frame_init_zero is all zeros; memset avoids building a
     * 4 KB temporary on the stack. */
    memset(out, 0, sizeof(*out));
    pb_istream_t stream = pb_istream_from_buffer(&f->buf[prefix], length);

    if (!pb_decode(&stream, micnote_v1_Frame_fields, out)) {
        return fail(f, -EBADMSG);
    }

    /* Keep whatever follows this frame for the next call. */
    const size_t used = prefix + length;

    memmove(f->buf, &f->buf[used], f->len - used);
    f->len -= used;
    return 1;
}

int mn_frame_encode(const micnote_v1_Frame *in, uint8_t *buf, size_t cap)
{
    pb_ostream_t stream = pb_ostream_from_buffer(buf, cap);

    /* PB_ENCODE_DELIMITED writes the varint length prefix first. */
    if (!pb_encode_ex(&stream, micnote_v1_Frame_fields, in, PB_ENCODE_DELIMITED)) {
        return -EINVAL;
    }
    return (int)stream.bytes_written;
}
