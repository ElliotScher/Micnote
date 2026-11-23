#include "dispatch.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <mbedtls/platform_util.h>
#include <psa/crypto.h>

#include "hosts.h"
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
    l->state = MN_LINK_NONE;
    l->slot = -1;
    l->pair_key = PSA_KEY_ID_NULL;
    l->link_key = PSA_KEY_ID_NULL;
    mn_framer_init(&l->framer);
}

static bool pairing_open(const struct mn_link *l)
{
    return l->cfg->pairing_open != NULL && l->cfg->pairing_open();
}

/* Ends a pairing in progress: its keys go, the LEDs stop. */
static void end_pairing(struct mn_link *l)
{
    const bool was_pairing = l->state == MN_LINK_PAIRING;

    psa_destroy_key(l->pair_key);
    psa_destroy_key(l->link_key);
    l->pair_key = PSA_KEY_ID_NULL;
    l->link_key = PSA_KEY_ID_NULL;
    l->confirm_pending = false;
    if (was_pairing && l->cfg->show_sas != NULL) {
        l->cfg->show_sas(NULL);
    }
}

void mn_link_close(struct mn_link *l)
{
    end_pairing(l);
    l->state = MN_LINK_NONE;
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

static int on_hello(struct mn_link *l, const micnote_v1_Hello *hello)
{
    /* A Hello starts the link over, whatever state it was in. */
    end_pairing(l);
    l->state = MN_LINK_NONE;
    if (hello->proto_version != 1) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_PROTO_VERSION);
    }
    l->slot = mn_hosts_find(hello->host_id);
    if (l->slot < 0 && !pairing_open(l)) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED);
    }
    micnote_v1_HelloAck *ack = &response(rx.req_id, micnote_v1_Frame_hello_ack_tag)->body.hello_ack;

    ack->has_device_info = true;
    ack->device_info = l->cfg->device_info;
    memcpy(l->host_id, hello->host_id, 8);
    memcpy(l->n_h, hello->host_nonce, MN_PAIR_NONCE_SIZE);
    if (psa_generate_random(l->n_d, MN_PAIR_NONCE_SIZE) != PSA_SUCCESS) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_STORAGE);
    }
    memcpy(ack->device_nonce, l->n_d, MN_PAIR_NONCE_SIZE);
    if (l->slot >= 0) {
        if (mn_pair_proof(mn_hosts_link_key(l->slot), MN_PROOF_PUCK, l->n_h, l->n_d,
                          ack->device_proof.bytes) != 0) {
            return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_STORAGE);
        }
        ack->device_proof.size = MN_PAIR_PROOF_SIZE;
    }
    l->state = MN_LINK_HELLO;
    return send_tx(l);
}

static int on_auth(struct mn_link *l, const micnote_v1_Auth *auth)
{
    if (l->slot < 0) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    }
    const int ret = mn_pair_proof_verify(mn_hosts_link_key(l->slot), MN_PROOF_HOST, l->n_h, l->n_d,
                                         auth->host_proof);

    if (ret != 0) {
        /* Wrong proof (or a key that can't be used): back to the start. */
        l->state = MN_LINK_NONE;
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED);
    }
    l->state = MN_LINK_SESSION;
    return send_ok(l, rx.req_id);
}

static int on_pair_begin(struct mn_link *l, const micnote_v1_PairBegin *req)
{
    uint8_t hash[32];
    uint8_t sas[MN_SAS_SYMBOLS];
    size_t len = 0;

    if (!pairing_open(l)) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    }
    /* The key must be the one Hello's host_id names. */
    if (psa_hash_compute(PSA_ALG_SHA_256, req->host_pub, 32, hash, sizeof(hash), &len) !=
            PSA_SUCCESS ||
        memcmp(hash, l->host_id, 8) != 0) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST);
    }
    if (l->slot < 0 && mn_hosts_count() >= MN_HOSTS_MAX) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_HOSTS_FULL);
    }
    micnote_v1_PairChallenge *ch =
        &response(rx.req_id, micnote_v1_Frame_pair_challenge_tag)->body.pair_challenge;

    if (mn_pair_key_generate(&l->pair_key, ch->device_pub) != 0 ||
        psa_generate_random(ch->device_nonce, MN_PAIR_NONCE_SIZE) != PSA_SUCCESS) {
        end_pairing(l);
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_STORAGE);
    }
    /* From here on, n_d is the PairChallenge nonce (protocol section 8). */
    memcpy(l->n_d, ch->device_nonce, MN_PAIR_NONCE_SIZE);
    const int ret =
        mn_pair_derive(l->pair_key, ch->device_pub, req->host_pub, l->host_id,
                       l->cfg->device_info.device_id, l->n_h, l->n_d, &l->link_key, sas);

    if (ret != 0) {
        end_pairing(l);
        return send_error(l, rx.req_id,
                          ret == -EINVAL ? micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST
                                         : micnote_v1_ErrorCode_ERROR_CODE_STORAGE);
    }
    memcpy(l->host_pub, req->host_pub, 32);
    strncpy(l->host_name, req->host_name, sizeof(l->host_name) - 1);
    l->host_name[sizeof(l->host_name) - 1] = '\0';
    l->state = MN_LINK_PAIRING;
    if (l->cfg->show_sas != NULL) {
        l->cfg->show_sas(sas);
    }
    mbedtls_platform_zeroize(sas, sizeof(sas));
    return send_tx(l);
}

static int on_pair_confirm(struct mn_link *l, const micnote_v1_PairConfirm *req)
{
    if (!req->user_confirmed) {
        end_pairing(l);
        l->state = MN_LINK_NONE;
        return send_ok(l, rx.req_id);
    }
    /* The answer waits for the button (mn_link_pair_press). */
    l->confirm_pending = true;
    l->confirm_req_id = rx.req_id;
    return 0;
}

int mn_link_pair_press(struct mn_link *l)
{
    if (l->state != MN_LINK_PAIRING || !l->confirm_pending) {
        return -EALREADY;
    }
    const uint32_t req_id = l->confirm_req_id;
    const int slot = mn_hosts_add(l->host_id, l->host_pub, l->host_name, l->link_key);

    end_pairing(l);
    if (slot < 0) {
        l->state = MN_LINK_NONE;
        return send_error(l, req_id,
                          slot == -ENOSPC ? micnote_v1_ErrorCode_ERROR_CODE_HOSTS_FULL
                                          : micnote_v1_ErrorCode_ERROR_CODE_STORAGE);
    }
    /* Recordings made before this host was paired, still on the fallback
     * key, are now the host's. A failure leaves them for the next pairing:
     * nothing is lost, and the pairing itself stands. */
    if (l->cfg->fallback != NULL) {
        struct mn_rec_recipient to;

        memcpy(to.pub, l->host_pub, 32);
        memcpy(to.host_id, l->host_id, 8);
        (void)mn_recstore_rewrap_all(l->cfg->rec_dir, l->cfg->fallback->id,
                                     l->cfg->fallback->host_id, &to);
    }
    l->slot = slot;
    l->state = MN_LINK_SESSION;
    response(req_id, micnote_v1_Frame_pair_done_tag)->body.pair_done.host_slot = (uint32_t)slot;
    return send_tx(l);
}

int mn_link_pair_cancel(struct mn_link *l)
{
    if (l->state != MN_LINK_PAIRING) {
        return 0;
    }
    const bool pending = l->confirm_pending;
    const uint32_t req_id = l->confirm_req_id;

    end_pairing(l);
    l->state = MN_LINK_NONE;
    return pending ? send_error(l, req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE) : 0;
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
    s->state = pairing_open(l) ? micnote_v1_DeviceState_DEVICE_STATE_PAIRING
                               : micnote_v1_DeviceState_DEVICE_STATE_CONNECTED;
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

    for (int i = 0; i < MN_HOSTS_MAX; i++) {
        const struct mn_host *h = mn_hosts_get(i);

        if (h != NULL) {
            micnote_v1_HostInfo *e = &list->hosts[list->hosts_count++];

            memcpy(e->host_id, h->host_id, 8);
            strncpy(e->name, h->name, sizeof(e->name) - 1);
            e->slot = (uint32_t)i;
        }
    }
    return send_tx(l);
}

static int on_unpair(struct mn_link *l, const micnote_v1_Unpair *req)
{
    const int ret = mn_hosts_remove(req->host_id);

    if (ret == -ENOENT) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    }
    if (ret < 0) {
        return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_STORAGE);
    }
    return send_ok(l, rx.req_id);
}

/* Requests before the session: Hello is handled by the caller. */
static int dispatch_hello(struct mn_link *l)
{
    switch (rx.which_body) {
        case micnote_v1_Frame_get_status_tag:
            return on_get_status(l);
        case micnote_v1_Frame_auth_tag:
            return on_auth(l, &rx.body.auth);
        case micnote_v1_Frame_pair_begin_tag:
            return on_pair_begin(l, &rx.body.pair_begin);
        default:
            return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    }
}

static int dispatch_session(struct mn_link *l)
{
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
        case micnote_v1_Frame_unpair_tag:
            return on_unpair(l, &rx.body.unpair);
        case micnote_v1_Frame_auth_tag:
        case micnote_v1_Frame_pair_begin_tag:
        case micnote_v1_Frame_pair_confirm_tag:
            return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
        default:
            /* No body, or a response type. */
            return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST);
    }
}

static int dispatch(struct mn_link *l)
{
    if (rx.which_body == micnote_v1_Frame_hello_tag) {
        return on_hello(l, &rx.body.hello);
    }
    switch (l->state) {
        case MN_LINK_HELLO:
            return dispatch_hello(l);
        case MN_LINK_PAIRING:
            if (l->confirm_pending) {
                return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BUSY);
            }
            if (rx.which_body == micnote_v1_Frame_pair_confirm_tag) {
                return on_pair_confirm(l, &rx.body.pair_confirm);
            }
            return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
        case MN_LINK_SESSION:
            return dispatch_session(l);
        default:
            return send_error(l, rx.req_id, micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
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
                mn_link_close(l);
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
