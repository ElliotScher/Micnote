#include "mnr.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/toolchain.h>

static const uint8_t magic[4] = {'M', 'N', 'R', '1'};

/* Field offsets, matching the layout comments in mnr.h. */
enum {
    OFF_MAGIC = 0,
    OFF_VERSION = 4,
    OFF_HDR_LEN = 5,
    OFF_FLAGS = 7,
    OFF_REC_ID = 8,
    OFF_CODEC = 16,
    OFF_RATE_HZ = 17,
    OFF_FRAME_MS = 19,
    OFF_START_BOOT = 20,
    OFF_START_UPTIME_MS = 24,
    OFF_START_UNIX_MS = 32,
    OFF_N_WRAPS = MN_MNR_CORE_SIZE,
    OFF_WRAPS = MN_MNR_CORE_SIZE + 1,
};

BUILD_ASSERT(OFF_START_UNIX_MS + 8 == MN_MNR_CORE_SIZE, "core layout and size disagree");
BUILD_ASSERT(sizeof(((struct mn_mnr_wrap *)0)->host_id) +
                     sizeof(((struct mn_mnr_wrap *)0)->eph_pub) +
                     sizeof(((struct mn_mnr_wrap *)0)->wrapped_cek) +
                     sizeof(((struct mn_mnr_wrap *)0)->tag) ==
                 MN_MNR_WRAP_SIZE,
             "wrap fields and MN_MNR_WRAP_SIZE disagree");
BUILD_ASSERT(MN_MNR_HEADER_SIZE <= MN_MNR_HEADER_MAX, "header no longer fits FetchDone");

/* Each wrap field is a byte array, so it is copied as-is: no struct padding
 * or byte order reaches the file. */
static void wrap_write(const struct mn_mnr_wrap *w, uint8_t *p)
{
    memcpy(p, w->host_id, sizeof(w->host_id));
    p += sizeof(w->host_id);
    memcpy(p, w->eph_pub, sizeof(w->eph_pub));
    p += sizeof(w->eph_pub);
    memcpy(p, w->wrapped_cek, sizeof(w->wrapped_cek));
    p += sizeof(w->wrapped_cek);
    memcpy(p, w->tag, sizeof(w->tag));
}

static void wrap_parse(const uint8_t *p, struct mn_mnr_wrap *w)
{
    memcpy(w->host_id, p, sizeof(w->host_id));
    p += sizeof(w->host_id);
    memcpy(w->eph_pub, p, sizeof(w->eph_pub));
    p += sizeof(w->eph_pub);
    memcpy(w->wrapped_cek, p, sizeof(w->wrapped_cek));
    p += sizeof(w->wrapped_cek);
    memcpy(w->tag, p, sizeof(w->tag));
}

int mn_mnr_header_write(const struct mn_mnr_header *h, uint8_t *buf, size_t cap)
{
    if (h->n_wraps > MN_MNR_MAX_WRAPS) {
        return -EINVAL;
    }
    if (cap < MN_MNR_HEADER_SIZE) {
        return -ENOSPC;
    }

    /* Unused wrap slots are zero, so the same header always has the same
     * bytes (test vectors depend on it). */
    memset(buf, 0, MN_MNR_HEADER_SIZE);

    memcpy(&buf[OFF_MAGIC], magic, sizeof(magic));
    buf[OFF_VERSION] = MN_MNR_VERSION;
    sys_put_le16(MN_MNR_HEADER_SIZE, &buf[OFF_HDR_LEN]);
    buf[OFF_FLAGS] = h->flags;
    sys_put_le64(h->rec_id, &buf[OFF_REC_ID]);
    buf[OFF_CODEC] = h->codec;
    sys_put_le16(h->rate_hz, &buf[OFF_RATE_HZ]);
    buf[OFF_FRAME_MS] = h->frame_ms;
    sys_put_le32(h->start_boot, &buf[OFF_START_BOOT]);
    sys_put_le64(h->start_uptime_ms, &buf[OFF_START_UPTIME_MS]);
    sys_put_le64(h->start_unix_ms, &buf[OFF_START_UNIX_MS]);

    buf[OFF_N_WRAPS] = h->n_wraps;
    for (uint8_t i = 0; i < h->n_wraps; i++) {
        wrap_write(&h->wraps[i], &buf[OFF_WRAPS + i * MN_MNR_WRAP_SIZE]);
    }
    return MN_MNR_HEADER_SIZE;
}

int mn_mnr_header_parse(const uint8_t *buf, size_t len, struct mn_mnr_header *out)
{
    /* Enough to read magic, version and hdr_len. */
    if (len < OFF_FLAGS) {
        return -ENODATA;
    }
    if (memcmp(&buf[OFF_MAGIC], magic, sizeof(magic)) != 0) {
        return -EBADMSG;
    }
    if (buf[OFF_VERSION] != MN_MNR_VERSION) {
        return -ENOTSUP;
    }
    const uint16_t hdr_len = sys_get_le16(&buf[OFF_HDR_LEN]);

    if (hdr_len < MN_MNR_HEADER_SIZE || hdr_len > MN_MNR_HEADER_MAX) {
        return -EBADMSG;
    }
    if (len < hdr_len) {
        return -ENODATA;
    }

    const uint8_t n_wraps = buf[OFF_N_WRAPS];

    if (n_wraps > MN_MNR_MAX_WRAPS) {
        return -EBADMSG;
    }

    memset(out, 0, sizeof(*out));
    out->flags = buf[OFF_FLAGS];
    out->rec_id = sys_get_le64(&buf[OFF_REC_ID]);
    out->codec = buf[OFF_CODEC];
    out->rate_hz = sys_get_le16(&buf[OFF_RATE_HZ]);
    out->frame_ms = buf[OFF_FRAME_MS];
    out->start_boot = sys_get_le32(&buf[OFF_START_BOOT]);
    out->start_uptime_ms = sys_get_le64(&buf[OFF_START_UPTIME_MS]);
    out->start_unix_ms = sys_get_le64(&buf[OFF_START_UNIX_MS]);
    out->n_wraps = n_wraps;
    for (uint8_t i = 0; i < n_wraps; i++) {
        wrap_parse(&buf[OFF_WRAPS + i * MN_MNR_WRAP_SIZE], &out->wraps[i]);
    }
    return hdr_len;
}

void mn_mnr_chunk_hdr_write(const struct mn_mnr_chunk_hdr *c, uint8_t out[MN_MNR_CHUNK_HDR_SIZE])
{
    sys_put_le32(c->seq, &out[0]);
    sys_put_le16(c->ct_len, &out[4]);
}

int mn_mnr_chunk_hdr_parse(const uint8_t in[MN_MNR_CHUNK_HDR_SIZE], struct mn_mnr_chunk_hdr *out)
{
    const uint16_t ct_len = sys_get_le16(&in[4]);

    if (ct_len == 0 || ct_len > MN_MNR_MAX_CT) {
        return -EBADMSG;
    }
    out->seq = sys_get_le32(&in[0]);
    out->ct_len = ct_len;
    return 0;
}
