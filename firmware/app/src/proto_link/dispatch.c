#include "dispatch.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <psa/crypto.h>

#include "recstore.h"

/* Frames are over 4 KB each: static, and one link at a time. */
static micnote_v1_Frame rx;
static micnote_v1_Frame tx;
static uint8_t tx_buf[MN_FRAME_MAX_WIRE];
static struct mn_recstore_info infos[16];

void mn_link_init(struct mn_link *l, const struct mn_link_config *cfg, mn_link_send_fn send,
                  void *send_ctx)
{
    memset(l, 0, sizeof(*l));
    l->cfg = cfg;
    l->send = send;
    l->send_ctx = send_ctx;
    mn_framer_init(&l->framer);
}

static micnote_v1_Frame *response(uint32_t req_id, pb_size_t tag)
{
    memset(&tx, 0, sizeof(tx));
    tx.req_id = req_id;
    tx.which_body = tag;
    return &tx;
}

static int send_tx(struct mn_link *l)
{
    const int n = mn_frame_encode(&tx, tx_buf, sizeof(tx_buf));

    if (n < 0) {
        return n;
    }
    return l->send(tx_buf, (size_t)n, l->send_ctx);
}

static int send_error(struct mn_link *l, uint32_t req_id, micnote_v1_ErrorCode code)
{
    response(req_id, micnote_v1_Frame_error_tag)->body.error.code = code;
    return send_tx(l);
}

static int send_ok(struct mn_link *l, uint32_t req_id)
{
    response(req_id, micnote_v1_Frame_ok_tag);
    return send_tx(l);
}

static micnote_v1_ErrorCode storage_error(int err)
{
    return err == -ENOENT ? micnote_v1_ErrorCode_ERROR_CODE_NOT_FOUND
                          : micnote_v1_ErrorCode_ERROR_CODE_STORAGE;
}

static bool paired(const struct mn_link_config *cfg, const uint8_t host_id[8])
{
    for (size_t i = 0; i < cfg->n_hosts; i++) {
        if (memcmp(cfg->host_ids[i], host_id, 8) == 0) {
            return true;
        }
    }
    return false;
}

static int on_hello(struct mn_link *l, const micnote_v1_Hello *hello)
{
    if (hello->proto_version != 1) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_PROTO_VERSION);
    }
    if (!paired(l->cfg, hello->host_id)) {
        l->session = false;
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED);
    }
    micnote_v1_HelloAck *ack = &response(rx.req_id, micnote_v1_Frame_hello_ack_tag)->body.hello_ack;

    ack->has_device_info = true;
    ack->device_info = l->cfg->device_info;
    if (psa_generate_random(ack->device_nonce, sizeof(ack->device_nonce)) != PSA_SUCCESS) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_STORAGE);
    }
    l->session = true;
    return send_tx(l);
}

static int on_set_time(struct mn_link *l, const micnote_v1_SetTime *t)
{
    if (t->unix_ms == 0) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST);
    }
    if (l->cfg->set_time != NULL) {
        l->cfg->set_time(t->unix_ms, t->tz_offset_min);
    }
    return send_ok(l, rx.req_id);
}

static int on_get_status(struct mn_link *l)
{
    bool more = true;
    uint64_t after = 0;
    uint32_t count = 0;
    uint64_t used = 0;

    /* Count recordings and their bytes, a page at a time. */
    while (more) {
        const int n = mn_recstore_list(l->cfg->rec_dir, after, infos, ARRAY_SIZE(infos), &more);

        if (n < 0) {
            return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_STORAGE);
        }
        for (int i = 0; i < n; i++) {
            used += infos[i].size_bytes;
        }
        count += (uint32_t)n;
        if (n > 0) {
            after = infos[n - 1].rec_id;
        }
    }
    micnote_v1_DeviceStatus *s =
        &response(rx.req_id, micnote_v1_Frame_device_status_tag)->body.device_status;

    if (l->cfg->fill_status != NULL) {
        l->cfg->fill_status(s);
    }
    s->recording_count = count;
    s->storage_used_kb = (uint32_t)((used + 1023) / 1024);
    s->state = micnote_v1_DeviceState_DEVICE_STATE_CONNECTED;
    return send_tx(l);
}

static int on_list(struct mn_link *l, const micnote_v1_ListRecordings *req)
{
    bool more = false;
    const int n = mn_recstore_list(l->cfg->rec_dir, req->after_id, infos, ARRAY_SIZE(infos), &more);

    if (n < 0) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_STORAGE);
    }
    micnote_v1_RecordingList *list =
        &response(rx.req_id, micnote_v1_Frame_recording_list_tag)->body.recording_list;

    for (int i = 0; i < n; i++) {
        micnote_v1_RecordingInfo *e = &list->entries[i];

        e->id = infos[i].rec_id;
        e->start_boot = infos[i].start_boot;
        e->start_uptime_ms = infos[i].start_uptime_ms;
        e->start_unix_ms = infos[i].start_unix_ms;
        e->size_bytes = infos[i].size_bytes;
        e->last_seq = infos[i].chunks > 0 ? infos[i].chunks - 1 : 0;
        e->complete = infos[i].complete;
        /* duration_ms and marker_count stay 0: they're in the encrypted
         * trailer, which only the host can read. */
    }
    list->entries_count = (pb_size_t)n;
    list->more = more;
    return send_tx(l);
}

static int on_fetch(struct mn_link *l, const micnote_v1_Fetch *req)
{
    static struct mn_rec_reader reader;
    static uint8_t header[MN_MNR_HEADER_MAX];
    const uint64_t id = req->recording_id;
    const uint32_t req_id = rx.req_id;
    uint32_t last_seq = 0;
    int ret = mn_rec_reader_open(&reader, l->cfg->rec_dir, id, header, sizeof(header));

    if (ret < 0) {
        return send_error(l, req_id, storage_error(ret));
    }
    const size_t hdr_len = (size_t)ret;

    for (;;) {
        micnote_v1_Chunk *c = &response(req_id, micnote_v1_Frame_chunk_tag)->body.chunk;
        uint32_t seq;

        ret = mn_rec_reader_next(&reader, req->from_seq, &seq, c->payload.bytes,
                                 sizeof(c->payload.bytes));
        if (ret <= 0) {
            break;
        }
        c->recording_id = id;
        c->seq = seq;
        c->payload.size = (pb_size_t)ret;
        last_seq = seq;
        ret = send_tx(l);
        if (ret < 0) {
            mn_rec_reader_close(&reader);
            return ret;
        }
    }
    mn_rec_reader_close(&reader);
    if (ret < 0) {
        return send_error(l, req_id, micnote_v1_ErrorCode_ERROR_CODE_STORAGE);
    }
    micnote_v1_FetchDone *done =
        &response(req_id, micnote_v1_Frame_fetch_done_tag)->body.fetch_done;

    done->recording_id = id;
    done->last_seq = last_seq;
    memcpy(done->header.bytes, header, hdr_len);
    done->header.size = (pb_size_t)hdr_len;
    return send_tx(l);
}

static int on_ack(struct mn_link *l, const micnote_v1_Ack *req)
{
    const int ret = mn_recstore_ack(l->cfg->rec_dir, req->recording_id, req->sha256);

    switch (ret) {
        case 0:
            return send_ok(l, rx.req_id);
        case -EBADMSG:
            return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_HASH_MISMATCH);
        case -EBUSY:
            return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
        default:
            return send_error(l, rx.req_id, storage_error(ret));
    }
}

static int on_list_hosts(struct mn_link *l)
{
    micnote_v1_HostList *list =
        &response(rx.req_id, micnote_v1_Frame_host_list_tag)->body.host_list;

    for (size_t i = 0; i < l->cfg->n_hosts; i++) {
        memcpy(list->hosts[i].host_id, l->cfg->host_ids[i], 8);
        list->hosts[i].slot = (uint32_t)i;
    }
    list->hosts_count = (pb_size_t)l->cfg->n_hosts;
    return send_tx(l);
}

static int dispatch(struct mn_link *l)
{
    if (rx.which_body == micnote_v1_Frame_hello_tag) {
        return on_hello(l, &rx.body.hello);
    }
    if (!l->session) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    }
    switch (rx.which_body) {
        case micnote_v1_Frame_set_time_tag:
            return on_set_time(l, &rx.body.set_time);
        case micnote_v1_Frame_get_status_tag:
            return on_get_status(l);
        case micnote_v1_Frame_list_recordings_tag:
            return on_list(l, &rx.body.list_recordings);
        case micnote_v1_Frame_fetch_tag:
            return on_fetch(l, &rx.body.fetch);
        case micnote_v1_Frame_ack_tag:
            return on_ack(l, &rx.body.ack);
        case micnote_v1_Frame_list_hosts_tag:
            return on_list_hosts(l);
        case micnote_v1_Frame_pair_begin_tag:
        case micnote_v1_Frame_pair_confirm_tag:
            /* Pairing lands in stage 1 PR 5; until then the puck is never in
             * the PAIRING state. */
            return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
        default:
            /* No body, a response type, or Unpair (PR 5). */
            return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST);
    }
}

int mn_link_rx(struct mn_link *l, const uint8_t *data, size_t len)
{
    while (len > 0) {
        const size_t took = mn_framer_push(&l->framer, data, len);

        data += took;
        len -= took;
        for (;;) {
            const int got = mn_framer_next(&l->framer, &rx);

            if (got < 0) {
                l->session = false;
                return got;
            }
            if (got == 0) {
                break;
            }
            /* A failed send means the link is going away; the next
             * mn_link_init() starts over. */
            const int ret = dispatch(l);

            if (ret < 0) {
                return ret;
            }
        }
    }
    return 0;
}
