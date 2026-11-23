/*
 * The dispatcher, driven the way a host drives it: encoded request frames
 * in through mn_link_rx(), responses captured from the send callback and
 * decoded with a second framer. The recordings are written by the real
 * recstore on simulated flash.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <psa/crypto.h>

#include "dispatch.h"
#include "framing.h"
#include "hosts.h"
#include "keys.h"
#include "mnr.h"
#include "pairing.h"
#include "recstore.h"
#include "wrap.h"

#define MNT "/lfs"
#define DIR MNT "/rec"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);
static struct fs_mount_t mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &lfs_cfg,
    .storage_dev = (void *)PARTITION_ID(rec_partition),
    .mnt_point = MNT,
};

/* RFC 7748 section 6.1 Bob: test vectors' host_a. */
static const uint8_t host_pub[32] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
    0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d, 0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f};
static const uint8_t host_id[8] = {0xf3, 0x5e, 0x56, 0x16, 0x16, 0x0a, 0x30, 0xbf};

/* ---- the link under test ------------------------------------------------ */

static uint8_t sent[96 * 1024];
static size_t sent_len;
static int send_result;

static int capture(const uint8_t *bytes, size_t len, void *ctx)
{
    ARG_UNUSED(ctx);
    if (send_result != 0) {
        return send_result;
    }
    zassert_true(sent_len + len <= sizeof(sent), "capture buffer full");
    memcpy(&sent[sent_len], bytes, len);
    sent_len += len;
    return 0;
}

static uint64_t time_set;

static void set_time(uint64_t unix_ms, int32_t tz)
{
    ARG_UNUSED(tz);
    time_set = unix_ms;
}

static void fill_status(micnote_v1_DeviceStatus *s)
{
    s->battery_pct = 77;
}

static bool window_open;

static bool pairing_open(void)
{
    return window_open;
}

static uint8_t shown_sas[MN_SAS_SYMBOLS];
static int sas_shown;   /* show_sas() calls with colours */
static int sas_cleared; /* show_sas(NULL) calls */

static void show_sas(const uint8_t sas[MN_SAS_SYMBOLS])
{
    if (sas == NULL) {
        sas_cleared++;
        return;
    }
    memcpy(shown_sas, sas, MN_SAS_SYMBOLS);
    sas_shown++;
}

static struct mn_fallback_key fallback;

static struct mn_link_config cfg = {
    .rec_dir = DIR,
    .fill_status = fill_status,
    .set_time = set_time,
    .pairing_open = pairing_open,
    .show_sas = show_sas,
    .fallback = &fallback,
};
static struct mn_link link;

/* Responses are decoded from `sent` in order. */
static struct mn_framer host_framer;
static size_t read_pos;
static micnote_v1_Frame req;
static micnote_v1_Frame resp;
static uint8_t wire[MN_FRAME_MAX_WIRE];
static uint32_t next_req_id = 1;

static void send_request(pb_size_t tag)
{
    req.req_id = next_req_id++;
    req.which_body = tag;
    const int n = mn_frame_encode(&req, wire, sizeof(wire));

    zassert_true(n > 0);
    zassert_ok(mn_link_rx(&link, wire, (size_t)n));
}

/* The next response; fails the test if there is none. */
static micnote_v1_Frame *next(void)
{
    for (;;) {
        const int got = mn_framer_next(&host_framer, &resp);

        zassert_true(got >= 0, "host framer: %d", got);
        if (got == 1) {
            zassert_equal(resp.req_id, req.req_id, "response to the wrong request");
            return &resp;
        }
        zassert_true(read_pos < sent_len, "no response");
        read_pos += mn_framer_push(&host_framer, &sent[read_pos], sent_len - read_pos);
    }
}

static bool nothing_more(void)
{
    return read_pos == sent_len && host_framer.len == 0;
}

static void expect_error(micnote_v1_ErrorCode code)
{
    const micnote_v1_Frame *r = next();

    zassert_equal(r->which_body, micnote_v1_Frame_error_tag, "body %u", r->which_body);
    zassert_equal(r->body.error.code, code, "code %d", r->body.error.code);
}

static uint8_t n_h[16];

/* Hello as `id`; returns the HelloAck (or fails the test). */
static const micnote_v1_HelloAck *hello_as(const uint8_t id[8])
{
    memset(&req, 0, sizeof(req));
    req.body.hello.proto_version = 1;
    memcpy(req.body.hello.host_id, id, 8);
    memset(n_h, 0x5A, sizeof(n_h));
    memcpy(req.body.hello.host_nonce, n_h, sizeof(n_h));
    send_request(micnote_v1_Frame_hello_tag);
    const micnote_v1_Frame *r = next();

    zassert_equal(r->which_body, micnote_v1_Frame_hello_ack_tag, "body %u", r->which_body);
    return &r->body.hello_ack;
}

/* Auth with the slot's stored link key, as the host would with its copy. */
static void auth(int slot, const uint8_t n_d[16])
{
    memset(&req, 0, sizeof(req));
    zassert_ok(
        mn_pair_proof(mn_hosts_link_key(slot), MN_PROOF_HOST, n_h, n_d, req.body.auth.host_proof));
    send_request(micnote_v1_Frame_auth_tag);
    zassert_equal(next()->which_body, micnote_v1_Frame_ok_tag);
}

/* A full session for host_a: Hello, check the device's proof, Auth. */
static void hello(void)
{
    uint8_t n_d[16];
    uint8_t want[32];
    const micnote_v1_HelloAck *ack = hello_as(host_id);

    memcpy(n_d, ack->device_nonce, 16);
    zassert_equal(ack->device_proof.size, 32);
    zassert_ok(mn_pair_proof(mn_hosts_link_key(0), MN_PROOF_PUCK, n_h, n_d, want));
    zassert_mem_equal(ack->device_proof.bytes, want, 32);
    auth(0, n_d);
}

/* ---- recordings --------------------------------------------------------- */

static struct mn_rec rec;

/* A recording of n_packets 40-byte packets, wrapped to host_a. */
static void record(uint64_t id, uint32_t n_packets, bool close)
{
    static struct mn_rec_params p;
    static struct mn_rec_secrets s;
    uint8_t packet[40];

    memset(&p, 0, sizeof(p));
    p.rec_id = id;
    p.frame_ms = 20;
    p.start_boot = (uint32_t)(id >> 32);
    p.n_recipients = 1;
    memcpy(p.recipients[0].pub, host_pub, 32);
    memcpy(p.recipients[0].host_id, host_id, 8);
    memset(&s, (int)id, sizeof(s));
    zassert_ok(mn_rec_open(&rec, DIR, &p, &s));
    for (uint32_t i = 0; i < n_packets; i++) {
        memset(packet, (int)i, sizeof(packet));
        zassert_ok(mn_rec_append(&rec, packet, sizeof(packet)));
    }
    if (close) {
        zassert_ok(mn_rec_close(&rec));
    } else {
        /* Leave it as a .part, as if still recording. */
        fs_close(&rec.file);
        psa_destroy_key(rec.cek);
        rec.open = false;
    }
}

static uint8_t file_buf[64 * 1024];

static size_t read_file(uint64_t id, const char *ext)
{
    char path[64];
    struct fs_file_t f;

    snprintk(path, sizeof(path), DIR "/%08x%08x%s", (uint32_t)(id >> 32), (uint32_t)id, ext);
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_READ), "%s", path);
    const ssize_t n = fs_read(&f, file_buf, sizeof(file_buf));

    fs_close(&f);
    zassert_true(n > 0);
    return (size_t)n;
}

static bool exists(uint64_t id, const char *ext)
{
    char path[64];
    struct fs_dirent entry;

    snprintk(path, sizeof(path), DIR "/%08x%08x%s", (uint32_t)(id >> 32), (uint32_t)id, ext);
    return fs_stat(path, &entry) == 0;
}

/* Fetches from from_seq and rebuilds the file: header + chunk records. */
static size_t fetch(uint64_t id, uint32_t from_seq, uint8_t *out, uint32_t *chunks)
{
    static uint8_t records[64 * 1024];
    size_t rec_len = 0;

    memset(&req, 0, sizeof(req));
    req.body.fetch.recording_id = id;
    req.body.fetch.from_seq = from_seq;
    send_request(micnote_v1_Frame_fetch_tag);
    *chunks = 0;
    for (;;) {
        const micnote_v1_Frame *r = next();

        if (r->which_body == micnote_v1_Frame_chunk_tag) {
            const micnote_v1_Chunk *c = &r->body.chunk;
            const struct mn_mnr_chunk_hdr ch = {.seq = c->seq,
                                                .ct_len = (uint16_t)(c->payload.size - 16)};

            zassert_equal(c->recording_id, id);
            zassert_equal(c->seq, from_seq + *chunks, "chunks in order");
            mn_mnr_chunk_hdr_write(&ch, &records[rec_len]);
            memcpy(&records[rec_len + 6], c->payload.bytes, c->payload.size);
            rec_len += 6 + c->payload.size;
            (*chunks)++;
            continue;
        }
        zassert_equal(r->which_body, micnote_v1_Frame_fetch_done_tag, "body %u", r->which_body);
        zassert_equal(r->body.fetch_done.recording_id, id);
        if (*chunks > 0) {
            zassert_equal(r->body.fetch_done.last_seq, from_seq + *chunks - 1);
        }
        const size_t hdr_len = r->body.fetch_done.header.size;

        memcpy(out, r->body.fetch_done.header.bytes, hdr_len);
        memcpy(&out[hdr_len], records, rec_len);
        return hdr_len + rec_len;
    }
}

/* ---- fixtures ----------------------------------------------------------- */

/* Pairs host_a into slot 0 directly, with a link key from a throwaway
 * pairing key (the derivation itself is tests/pairing's). */
static void pair_host_a(void)
{
    psa_key_id_t d;
    psa_key_id_t link;
    uint8_t d_pub[32];
    uint8_t sas[4];

    zassert_ok(mn_pair_key_generate(&d, d_pub));
    zassert_ok(mn_pair_derive(d, d_pub, host_pub, host_id, cfg.device_info.device_id, n_h, n_h,
                              &link, sas));
    zassert_equal(mn_hosts_add(host_id, host_pub, "host_a", link), 0);
    psa_destroy_key(link);
    psa_destroy_key(d);
}

static void *setup(void)
{
    const struct flash_area *fa;

    zassert_ok(flash_area_open(PARTITION_ID(storage_partition), &fa));
    zassert_ok(flash_area_flatten(fa, 0, fa->fa_size));
    flash_area_close(fa);
    zassert_ok(settings_subsys_init());
    zassert_equal(psa_crypto_init(), PSA_SUCCESS);
    zassert_true(mn_keys_fallback(&fallback) >= 0);
    cfg.device_info.proto_version = 1;
    memcpy(cfg.device_info.device_id, (const uint8_t[]){1, 2, 3, 4, 5, 6, 7, 8}, 8);
    strcpy(cfg.device_info.board, "native_sim");
    return NULL;
}

static void before_each(void *fixture)
{
    const struct flash_area *fa;

    ARG_UNUSED(fixture);
    zassert_ok(flash_area_open(PARTITION_ID(rec_partition), &fa));
    zassert_ok(flash_area_flatten(fa, 0, fa->fa_size));
    flash_area_close(fa);
    zassert_ok(fs_mount(&mnt));
    zassert_ok(mn_recstore_init(DIR));

    /* Exactly one paired host, host_a, in slot 0. */
    zassert_ok(mn_hosts_init());
    for (int i = 0; i < MN_HOSTS_MAX; i++) {
        if (mn_hosts_get(i) != NULL) {
            zassert_ok(mn_hosts_remove(mn_hosts_get(i)->host_id));
        }
    }
    pair_host_a();

    mn_link_init(&link, &cfg, capture, NULL);
    mn_framer_init(&host_framer);
    sent_len = 0;
    read_pos = 0;
    send_result = 0;
    time_set = 0;
    window_open = false;
    sas_shown = 0;
    sas_cleared = 0;
    memset(&req, 0, sizeof(req));
}

static void after_each(void *fixture)
{
    ARG_UNUSED(fixture);
    mn_link_close(&link);
    fs_unmount(&mnt);
}

ZTEST_SUITE(dispatch, NULL, setup, before_each, after_each, NULL);

/* ---- session ------------------------------------------------------------ */

ZTEST(dispatch, test_hello_from_paired_host)
{
    memset(&req, 0, sizeof(req));
    req.body.hello.proto_version = 1;
    memcpy(req.body.hello.host_id, host_id, 8);
    send_request(micnote_v1_Frame_hello_tag);
    const micnote_v1_Frame *r = next();

    zassert_equal(r->which_body, micnote_v1_Frame_hello_ack_tag);
    zassert_true(r->body.hello_ack.has_device_info);
    zassert_str_equal(r->body.hello_ack.device_info.board, "native_sim");
    zassert_true(nothing_more());
}

ZTEST(dispatch, test_hello_rejections)
{
    memset(&req, 0, sizeof(req));
    req.body.hello.proto_version = 1;
    memset(req.body.hello.host_id, 0x99, 8);
    send_request(micnote_v1_Frame_hello_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED);

    memset(&req, 0, sizeof(req));
    req.body.hello.proto_version = 2;
    memcpy(req.body.hello.host_id, host_id, 8);
    send_request(micnote_v1_Frame_hello_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_PROTO_VERSION);
}

ZTEST(dispatch, test_nothing_before_hello)
{
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_list_recordings_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    send_request(micnote_v1_Frame_get_status_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
}

ZTEST(dispatch, test_unpaired_hello_ends_the_session)
{
    hello();
    memset(&req, 0, sizeof(req));
    memset(req.body.hello.host_id, 0x99, 8);
    req.body.hello.proto_version = 1;
    send_request(micnote_v1_Frame_hello_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED);
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_get_status_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
}

ZTEST(dispatch, test_set_time)
{
    hello();
    memset(&req, 0, sizeof(req));
    req.body.set_time.unix_ms = 1763800000000ULL;
    send_request(micnote_v1_Frame_set_time_tag);
    zassert_equal(next()->which_body, micnote_v1_Frame_ok_tag);
    zassert_equal(time_set, 1763800000000ULL);

    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_set_time_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST);
}

ZTEST(dispatch, test_unsupported_requests)
{
    hello();
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_pair_begin_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    send_request(0); /* no body */
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST);
    send_request(micnote_v1_Frame_ok_tag); /* a response type */
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST);
}

ZTEST(dispatch, test_list_hosts)
{
    hello();
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_list_hosts_tag);
    const micnote_v1_Frame *r = next();

    zassert_equal(r->which_body, micnote_v1_Frame_host_list_tag);
    zassert_equal(r->body.host_list.hosts_count, 1);
    zassert_mem_equal(r->body.host_list.hosts[0].host_id, host_id, 8);
}

/* ---- authentication ----------------------------------------------------- */

ZTEST(dispatch, test_only_status_and_auth_before_auth)
{
    const micnote_v1_HelloAck *ack = hello_as(host_id);

    zassert_equal(ack->device_proof.size, 32);
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_get_status_tag);
    zassert_equal(next()->which_body, micnote_v1_Frame_device_status_tag);
    send_request(micnote_v1_Frame_list_recordings_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    send_request(micnote_v1_Frame_ack_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    send_request(micnote_v1_Frame_unpair_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    /* Not in PAIRING: no PairBegin. */
    send_request(micnote_v1_Frame_pair_begin_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
}

ZTEST(dispatch, test_wrong_host_proof_ends_the_session)
{
    hello_as(host_id);
    memset(&req, 0, sizeof(req));
    memset(req.body.auth.host_proof, 0x11, 32);
    send_request(micnote_v1_Frame_auth_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED);
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_get_status_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
}

ZTEST(dispatch, test_proof_from_an_old_link_is_refused)
{
    uint8_t old_n_d[16];

    memcpy(old_n_d, hello_as(host_id)->device_nonce, 16);
    /* A new link (new device nonce): the old nonce's proof is a replay. */
    hello_as(host_id);
    memset(&req, 0, sizeof(req));
    zassert_ok(
        mn_pair_proof(mn_hosts_link_key(0), MN_PROOF_HOST, n_h, old_n_d, req.body.auth.host_proof));
    send_request(micnote_v1_Frame_auth_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED);
}

ZTEST(dispatch, test_auth_again_in_session)
{
    hello();
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_auth_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
}

/* ---- pairing ------------------------------------------------------------ */

/* A second host: private key 0x42.. (test vectors' host_b). */
static psa_key_id_t b_key;
static uint8_t b_pub[32];
static uint8_t b_id[8];

static void make_host_b(void)
{
    uint8_t priv[32];
    uint8_t hash[32];
    size_t len;

    memset(priv, 0x42, 32);
    zassert_ok(mn_x25519_import(priv, &b_key));
    zassert_equal(psa_export_public_key(b_key, b_pub, 32, &len), PSA_SUCCESS);
    zassert_equal(psa_hash_compute(PSA_ALG_SHA_256, b_pub, 32, hash, 32, &len), PSA_SUCCESS);
    memcpy(b_id, hash, 8);
    psa_destroy_key(b_key);
}

/* Hello + PairBegin as host_b; returns the PairChallenge's device nonce. */
static void begin_pairing_b(uint8_t n_d[16])
{
    const micnote_v1_HelloAck *ack = hello_as(b_id);

    zassert_equal(ack->device_proof.size, 0, "no proof for a host that isn't paired");
    memset(&req, 0, sizeof(req));
    memcpy(req.body.pair_begin.host_pub, b_pub, 32);
    strcpy(req.body.pair_begin.host_name, "laptop");
    send_request(micnote_v1_Frame_pair_begin_tag);
    const micnote_v1_Frame *r = next();

    zassert_equal(r->which_body, micnote_v1_Frame_pair_challenge_tag, "body %u", r->which_body);
    memcpy(n_d, r->body.pair_challenge.device_nonce, 16);
}

static void confirm(bool yes)
{
    memset(&req, 0, sizeof(req));
    req.body.pair_confirm.user_confirmed = yes;
    send_request(micnote_v1_Frame_pair_confirm_tag);
}

ZTEST(dispatch, test_unknown_host_outside_pairing)
{
    make_host_b();
    memset(&req, 0, sizeof(req));
    req.body.hello.proto_version = 1;
    memcpy(req.body.hello.host_id, b_id, 8);
    send_request(micnote_v1_Frame_hello_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED);
}

ZTEST(dispatch, test_pairing_end_to_end)
{
    uint8_t n_d[16];

    make_host_b();
    window_open = true;
    begin_pairing_b(n_d);
    zassert_equal(sas_shown, 1);
    for (int i = 0; i < MN_SAS_SYMBOLS; i++) {
        zassert_true(shown_sas[i] < MN_SAS_COLORS);
    }

    /* Confirmed in the app: the answer waits for the button. */
    confirm(true);
    zassert_true(nothing_more());
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_get_status_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BUSY);

    zassert_ok(mn_link_pair_press(&link));
    req.req_id--; /* the PairDone answers the PairConfirm before the GetStatus */
    const micnote_v1_Frame *r = next();

    zassert_equal(r->which_body, micnote_v1_Frame_pair_done_tag, "body %u", r->which_body);
    zassert_equal(r->body.pair_done.host_slot, 1);
    zassert_equal(sas_cleared, 1);
    zassert_equal(mn_link_pair_press(&link), -EALREADY);

    /* The link is a session now. */
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_list_hosts_tag);
    r = next();
    zassert_equal(r->body.host_list.hosts_count, 2);
    zassert_mem_equal(r->body.host_list.hosts[1].host_id, b_id, 8);
    zassert_str_equal(r->body.host_list.hosts[1].name, "laptop");

    /* And the next link authenticates with the stored link key. */
    window_open = false;
    const micnote_v1_HelloAck *ack = hello_as(b_id);

    zassert_equal(ack->device_proof.size, 32);
    memcpy(n_d, ack->device_nonce, 16);
    auth(1, n_d);
}

ZTEST(dispatch, test_pairing_moves_fallback_recordings)
{
    static struct mn_rec_params p;
    static struct mn_rec_secrets sec;
    static struct mn_mnr_header h;
    uint8_t n_d[16];
    const uint8_t pkt[20] = {0x48};

    /* A recording made before any pairing: wrapped to the fallback key. */
    memset(&p, 0, sizeof(p));
    p.rec_id = 0x0000000500000001ULL;
    p.frame_ms = 20;
    p.n_recipients = 1;
    memcpy(p.recipients[0].pub, fallback.pub, 32);
    memcpy(p.recipients[0].host_id, fallback.host_id, 8);
    zassert_ok(mn_rec_secrets_random(&sec));
    zassert_ok(mn_rec_open(&rec, DIR, &p, &sec));
    zassert_ok(mn_rec_append(&rec, pkt, sizeof(pkt)));
    zassert_ok(mn_rec_close(&rec));

    make_host_b();
    window_open = true;
    begin_pairing_b(n_d);
    confirm(true);
    zassert_ok(mn_link_pair_press(&link));
    zassert_equal(next()->which_body, micnote_v1_Frame_pair_done_tag);

    const size_t n = read_file(p.rec_id, ".mnr");

    zassert_true(mn_mnr_header_parse(file_buf, n, &h) > 0);
    zassert_equal(h.n_wraps, 1);
    zassert_mem_equal(h.wraps[0].host_id, b_id, 8, "now host_b's");
}

/* Fills slots 1..3 with made-up hosts, each with a real link key. */
static void fill_slots(void)
{
    for (int i = 1; i < MN_HOSTS_MAX; i++) {
        psa_key_id_t d;
        psa_key_id_t link;
        uint8_t d_pub[32];
        uint8_t sas[4];
        uint8_t id[8];

        memset(id, 0xC0 + i, 8);
        zassert_ok(mn_pair_key_generate(&d, d_pub));
        zassert_ok(mn_pair_derive(d, d_pub, host_pub, id, cfg.device_info.device_id, n_h, n_h,
                                  &link, sas));
        zassert_equal(mn_hosts_add(id, host_pub, "x", link), i);
        psa_destroy_key(link);
        psa_destroy_key(d);
    }
}

ZTEST(dispatch, test_pair_begin_checks)
{
    make_host_b();
    window_open = true;
    /* host_pub must be the key Hello's host_id names. */
    hello_as(b_id);
    memset(&req, 0, sizeof(req));
    memcpy(req.body.pair_begin.host_pub, host_pub, 32);
    send_request(micnote_v1_Frame_pair_begin_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST);

    /* All four slots taken: a fifth host is refused, but a paired one may
     * pair again. */
    fill_slots();
    memcpy(req.body.pair_begin.host_pub, b_pub, 32);
    send_request(micnote_v1_Frame_pair_begin_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_HOSTS_FULL);
    hello_as(host_id);
    memset(&req, 0, sizeof(req));
    memcpy(req.body.pair_begin.host_pub, host_pub, 32);
    send_request(micnote_v1_Frame_pair_begin_tag);
    zassert_equal(next()->which_body, micnote_v1_Frame_pair_challenge_tag);
}

ZTEST(dispatch, test_cancel_and_window_close)
{
    uint8_t n_d[16];

    make_host_b();
    window_open = true;
    begin_pairing_b(n_d);
    confirm(false);
    zassert_equal(next()->which_body, micnote_v1_Frame_ok_tag);
    zassert_equal(sas_cleared, 1);
    zassert_equal(mn_hosts_find(b_id), -ENOENT);
    /* The link starts over. */
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_get_status_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);

    /* The window closes while the confirm waits for the button. */
    begin_pairing_b(n_d);
    confirm(true);
    zassert_ok(mn_link_pair_cancel(&link));
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    zassert_equal(sas_cleared, 2);
    zassert_equal(mn_link_pair_press(&link), -EALREADY);
    zassert_equal(mn_hosts_find(b_id), -ENOENT);
    zassert_ok(mn_link_pair_cancel(&link), "nothing to cancel");
}

ZTEST(dispatch, test_pairing_state_accepts_only_confirm)
{
    uint8_t n_d[16];

    make_host_b();
    window_open = true;
    begin_pairing_b(n_d);
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_list_hosts_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    /* A Hello drops the pairing. */
    hello();
    zassert_equal(sas_cleared, 1);
}

ZTEST(dispatch, test_repair_keeps_the_slot)
{
    window_open = true;
    hello_as(host_id);
    memset(&req, 0, sizeof(req));
    memcpy(req.body.pair_begin.host_pub, host_pub, 32);
    strcpy(req.body.pair_begin.host_name, "renamed");
    send_request(micnote_v1_Frame_pair_begin_tag);
    zassert_equal(next()->which_body, micnote_v1_Frame_pair_challenge_tag);
    confirm(true);
    zassert_ok(mn_link_pair_press(&link));
    zassert_equal(next()->body.pair_done.host_slot, 0);
    zassert_str_equal(mn_hosts_get(0)->name, "renamed");
    zassert_equal(mn_hosts_count(), 1);
}

ZTEST(dispatch, test_unpair)
{
    hello();
    memset(&req, 0, sizeof(req));
    memset(req.body.unpair.host_id, 0x77, 8);
    send_request(micnote_v1_Frame_unpair_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    memcpy(req.body.unpair.host_id, host_id, 8);
    send_request(micnote_v1_Frame_unpair_tag);
    zassert_equal(next()->which_body, micnote_v1_Frame_ok_tag);
    zassert_equal(mn_hosts_count(), 0);

    /* Gone for the next link too. */
    memset(&req, 0, sizeof(req));
    req.body.hello.proto_version = 1;
    memcpy(req.body.hello.host_id, host_id, 8);
    send_request(micnote_v1_Frame_hello_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED);
}

/* ---- recordings --------------------------------------------------------- */

ZTEST(dispatch, test_status_counts_recordings)
{
    record(0x0000000100000001ULL, 50, true);
    record(0x0000000100000002ULL, 50, true);
    hello();
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_get_status_tag);
    const micnote_v1_Frame *r = next();

    zassert_equal(r->which_body, micnote_v1_Frame_device_status_tag);
    zassert_equal(r->body.device_status.recording_count, 2);
    zassert_equal(r->body.device_status.battery_pct, 77, "from fill_status");
    zassert_true(r->body.device_status.storage_used_kb >= 4);
}

ZTEST(dispatch, test_list)
{
    record(0x0000000100000002ULL, 150, true);
    record(0x0000000100000001ULL, 10, true);
    record(0x0000000200000001ULL, 150, false);
    hello();
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_list_recordings_tag);
    const micnote_v1_Frame *r = next();
    const micnote_v1_RecordingList *list = &r->body.recording_list;

    zassert_equal(r->which_body, micnote_v1_Frame_recording_list_tag);
    zassert_equal(list->entries_count, 3);
    zassert_false(list->more);
    zassert_equal(list->entries[0].id, 0x0000000100000001ULL, "ascending");
    zassert_equal(list->entries[1].id, 0x0000000100000002ULL);
    zassert_equal(list->entries[2].id, 0x0000000200000001ULL);
    zassert_true(list->entries[0].complete);
    zassert_false(list->entries[2].complete, "still a .part");
    zassert_equal(list->entries[0].last_seq, 1, "one audio chunk + the trailer");
    zassert_equal(list->entries[0].start_boot, 1);
    zassert_equal(list->entries[0].size_bytes, read_file(0x0000000100000001ULL, ".mnr"));

    memset(&req, 0, sizeof(req));
    req.body.list_recordings.after_id = 0x0000000100000001ULL;
    send_request(micnote_v1_Frame_list_recordings_tag);
    zassert_equal(next()->body.recording_list.entries_count, 2, "after_id is exclusive");
}

ZTEST(dispatch, test_list_pages)
{
    /* 18 recordings: a full page of 16, then 2. */
    for (uint64_t i = 18; i >= 1; i--) {
        record(0x0000000100000000ULL + i, 1, true);
    }
    hello();
    memset(&req, 0, sizeof(req));
    send_request(micnote_v1_Frame_list_recordings_tag);
    const micnote_v1_RecordingList *list = &next()->body.recording_list;

    zassert_equal(list->entries_count, 16);
    zassert_true(list->more);
    for (int i = 0; i < 16; i++) {
        zassert_equal(list->entries[i].id, 0x0000000100000001ULL + (uint64_t)i);
    }
    memset(&req, 0, sizeof(req));
    req.body.list_recordings.after_id = 0x0000000100000010ULL;
    send_request(micnote_v1_Frame_list_recordings_tag);
    list = &next()->body.recording_list;
    zassert_equal(list->entries_count, 2);
    zassert_false(list->more);
}

/* A fetch rebuilds the stored file byte for byte; a resumed fetch gives the
 * rest of it. */
ZTEST(dispatch, test_fetch_and_resume)
{
    static uint8_t got[64 * 1024];
    static uint8_t tail[64 * 1024];
    const uint64_t id = 0x0000000100000001ULL;
    uint32_t chunks;
    uint32_t tail_chunks;

    record(id, 300, true);
    const size_t size = read_file(id, ".mnr");

    hello();
    zassert_equal(fetch(id, 0, got, &chunks), size);
    zassert_mem_equal(got, file_buf, size);
    zassert_true(chunks >= 4);

    const size_t tail_size = fetch(id, 2, tail, &tail_chunks);

    zassert_equal(tail_chunks, chunks - 2);
    /* Header, then the records from seq 2 on: the end of the file. */
    const size_t hdr = MN_MNR_HEADER_SIZE;

    zassert_mem_equal(&tail[hdr], &file_buf[size - (tail_size - hdr)], tail_size - hdr);
}

ZTEST(dispatch, test_fetch_recording_in_progress)
{
    static uint8_t got[64 * 1024];
    const uint64_t id = 0x0000000200000001ULL;
    uint32_t chunks;

    record(id, 300, false);
    const size_t size = read_file(id, ".part");

    hello();
    zassert_equal(fetch(id, 0, got, &chunks), size);
    zassert_mem_equal(got, file_buf, size);
}

ZTEST(dispatch, test_fetch_missing)
{
    hello();
    memset(&req, 0, sizeof(req));
    req.body.fetch.recording_id = 42;
    send_request(micnote_v1_Frame_fetch_tag);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

static void ack(uint64_t id, const uint8_t sha[32])
{
    memset(&req, 0, sizeof(req));
    req.body.ack.recording_id = id;
    memcpy(req.body.ack.sha256, sha, 32);
    send_request(micnote_v1_Frame_ack_tag);
}

ZTEST(dispatch, test_ack)
{
    const uint64_t id = 0x0000000100000001ULL;
    uint8_t sha[32];
    size_t len;

    record(id, 100, true);
    const size_t size = read_file(id, ".mnr");

    zassert_equal(psa_hash_compute(PSA_ALG_SHA_256, file_buf, size, sha, sizeof(sha), &len),
                  PSA_SUCCESS);
    hello();

    uint8_t wrong[32];

    memcpy(wrong, sha, 32);
    wrong[0] ^= 1;
    ack(id, wrong);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_HASH_MISMATCH);
    zassert_true(exists(id, ".mnr"), "a bad hash deletes nothing");

    ack(id, sha);
    zassert_equal(next()->which_body, micnote_v1_Frame_ok_tag);
    zassert_false(exists(id, ".mnr"));

    ack(id, sha);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

ZTEST(dispatch, test_ack_while_recording)
{
    const uint64_t id = 0x0000000200000001ULL;
    static const uint8_t sha[32];

    record(id, 10, false);
    hello();
    ack(id, sha);
    expect_error(micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE);
    zassert_true(exists(id, ".part"));
}

/* ---- the byte stream ---------------------------------------------------- */

ZTEST(dispatch, test_request_split_across_packets)
{
    memset(&req, 0, sizeof(req));
    req.req_id = next_req_id++;
    req.which_body = micnote_v1_Frame_hello_tag;
    req.body.hello.proto_version = 1;
    memcpy(req.body.hello.host_id, host_id, 8);
    const int n = mn_frame_encode(&req, wire, sizeof(wire));

    for (int i = 0; i < n; i++) {
        zassert_ok(mn_link_rx(&link, &wire[i], 1));
    }
    zassert_equal(next()->which_body, micnote_v1_Frame_hello_ack_tag);
}

ZTEST(dispatch, test_garbage_drops_the_link)
{
    static const uint8_t garbage[] = {0xFF, 0x7F};

    hello();
    zassert_equal(mn_link_rx(&link, garbage, sizeof(garbage)), -EMSGSIZE);
    zassert_equal(link.state, MN_LINK_NONE);
}

ZTEST(dispatch, test_send_failure_is_returned)
{
    memset(&req, 0, sizeof(req));
    req.req_id = next_req_id++;
    req.which_body = micnote_v1_Frame_hello_tag;
    req.body.hello.proto_version = 1;
    memcpy(req.body.hello.host_id, host_id, 8);
    const int n = mn_frame_encode(&req, wire, sizeof(wire));

    send_result = -EIO;
    zassert_equal(mn_link_rx(&link, wire, (size_t)n), -EIO);
}
