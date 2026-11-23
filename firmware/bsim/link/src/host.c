/*
 * The host role, a paired host's whole life over BLE:
 *
 * 1. Find the puck by its advertising (sync_pending set), connect, bond with
 *    LE Secure Connections (the puck's pairing window is open).
 * 2. Read the PSM, open the L2CAP channel, pair at the app layer (protocol
 *    section 8), list, fetch, decrypt and ack both memos: they were on the
 *    puck's fallback key, and pairing moved them to this host.
 * 3. Reconnect: re-encrypt with the bond, authenticate with the link key
 *    (device_proof, Auth), check the status characteristic.
 * 4. Reconnect over the GATT fallback (ctrl/data): authenticate, ListHosts.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#include <psa/crypto.h>

#include "aead.h"
#include "babblekit/testcase.h"
#include "ble.h"
#include "mnr.h"
#include "pairing.h"
#include "recstore.h"
#include "roles.h"
#include "wrap.h"

/* RFC 7748 section 6.1's Bob: test vectors' host_a. */
static const uint8_t host_priv[32] = {
    0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
    0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd, 0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb};
static psa_key_id_t host_key;
static uint8_t host_pub[32];
static uint8_t host_id[8];
static psa_key_id_t link_key = PSA_KEY_ID_NULL;

static micnote_v1_Frame req;
static micnote_v1_Frame resp;
static uint8_t n_h[16];
static uint8_t n_d[16];
static uint8_t device_id[8];

static void expect(pb_size_t tag, const char *what)
{
    TEST_ASSERT(resp.which_body == tag, "%s: got body %u (error %d)", what, resp.which_body,
                resp.which_body == micnote_v1_Frame_error_tag ? resp.body.error.code : 0);
}

static void call(pb_size_t tag, pb_size_t want, const char *what, uint32_t timeout_ms)
{
    req.which_body = tag;
    TEST_ASSERT(hl_call(&req, &resp, timeout_ms) == 0, "%s: no answer", what);
    expect(want, what);
    memset(&req, 0, sizeof(req));
}

static void hello(void)
{
    TEST_ASSERT(psa_generate_random(n_h, sizeof(n_h)) == PSA_SUCCESS, "nonce");
    req.body.hello.proto_version = 1;
    memcpy(req.body.hello.host_id, host_id, 8);
    memcpy(req.body.hello.host_nonce, n_h, 16);
    call(micnote_v1_Frame_hello_tag, micnote_v1_Frame_hello_ack_tag, "Hello", 5000);
    memcpy(n_d, resp.body.hello_ack.device_nonce, 16);
    memcpy(device_id, resp.body.hello_ack.device_info.device_id, 8);
}

/* Hello + device_proof + Auth with the link key from pairing. */
static void authenticate(void)
{
    hello();
    TEST_ASSERT(resp.body.hello_ack.device_proof.size == 32, "no device_proof");
    TEST_ASSERT(mn_pair_proof_verify(link_key, MN_PROOF_PUCK, n_h, n_d,
                                     resp.body.hello_ack.device_proof.bytes) == 0,
                "device_proof doesn't verify: not our puck");
    TEST_ASSERT(mn_pair_proof(link_key, MN_PROOF_HOST, n_h, n_d, req.body.auth.host_proof) == 0,
                "proof");
    call(micnote_v1_Frame_auth_tag, micnote_v1_Frame_ok_tag, "Auth", 5000);
}

static void pair(void)
{
    uint8_t sas[MN_SAS_SYMBOLS];

    hello();
    TEST_ASSERT(resp.body.hello_ack.device_proof.size == 0, "proof from an unpaired puck");
    memcpy(req.body.pair_begin.host_pub, host_pub, 32);
    strcpy(req.body.pair_begin.host_name, "bsim host");
    call(micnote_v1_Frame_pair_begin_tag, micnote_v1_Frame_pair_challenge_tag, "PairBegin", 5000);
    memcpy(n_d, resp.body.pair_challenge.device_nonce, 16);

    /* The host's side of protocol section 8. The link key's inputs are
     * symmetric (ss, host_id, device_id); the SAS's order of public keys
     * isn't, so this test doesn't compare SAS colours (tests/pairing and
     * core's tests check the SAS against known answers). */
    TEST_ASSERT(mn_pair_derive(host_key, host_pub, resp.body.pair_challenge.device_pub, host_id,
                               device_id, n_h, n_d, &link_key, sas) == 0,
                "derive");
    req.body.pair_confirm.user_confirmed = true;
    call(micnote_v1_Frame_pair_confirm_tag, micnote_v1_Frame_pair_done_tag, "PairConfirm", 10000);
    TEST_PRINT("host: paired, slot %u", resp.body.pair_done.host_slot);
}

/* Fetches one recording, checks this host can decrypt it, and acks it. */
static void sync_one(uint64_t id)
{
    static uint8_t file[32 * 1024];
    static struct mn_mnr_header h;
    static uint8_t pt[MN_MNR_MAX_CT];
    size_t len = 0;
    uint32_t chunks = 0;
    const int64_t t0 = k_uptime_get();
    const size_t rx0 = hl_rx_bytes();

    /* Chunk records go first; the header (from FetchDone) goes in front. */
    static uint8_t records[32 * 1024];
    size_t rec_len = 0;

    req.body.fetch.recording_id = id;
    req.which_body = micnote_v1_Frame_fetch_tag;
    TEST_ASSERT(hl_call(&req, &resp, 10000) == 0, "Fetch: no answer");
    memset(&req, 0, sizeof(req));
    while (resp.which_body == micnote_v1_Frame_chunk_tag) {
        const struct mn_mnr_chunk_hdr ch = {
            .seq = resp.body.chunk.seq, .ct_len = (uint16_t)(resp.body.chunk.payload.size - 16)};

        TEST_ASSERT(ch.seq == chunks, "chunk %u out of order", ch.seq);
        mn_mnr_chunk_hdr_write(&ch, &records[rec_len]);
        memcpy(&records[rec_len + 6], resp.body.chunk.payload.bytes, resp.body.chunk.payload.size);
        rec_len += 6 + resp.body.chunk.payload.size;
        chunks++;
        TEST_ASSERT(hl_next(&resp, 10000) == 0, "Fetch: stream stopped");
    }
    expect(micnote_v1_Frame_fetch_done_tag, "FetchDone");
    const int64_t ms = MAX(k_uptime_get() - t0, 1);
    const size_t bytes = hl_rx_bytes() - rx0;

    TEST_PRINT("host: fetched %016llx: %u chunks, %u bytes in %lld ms (%u KB/s simulated)",
               (unsigned long long)id, chunks, (unsigned)bytes, ms,
               (unsigned)(bytes * 1000 / 1024 / ms));
    memcpy(file, resp.body.fetch_done.header.bytes, resp.body.fetch_done.header.size);
    len = resp.body.fetch_done.header.size;
    memcpy(&file[len], records, rec_len);
    len += rec_len;

    /* Decrypt the first chunk as this host: the wrap must be ours now. */
    uint8_t cek[MN_CEK_SIZE];
    uint8_t core_hash[32];
    psa_key_id_t cek_key;
    int slot = -1;
    const int hdr_len = mn_mnr_header_parse(file, len, &h);

    TEST_ASSERT(hdr_len > 0, "header");
    for (int i = 0; i < h.n_wraps; i++) {
        if (memcmp(h.wraps[i].host_id, host_id, 8) == 0) {
            slot = i;
        }
    }
    TEST_ASSERT(slot >= 0, "not wrapped to this host: pairing didn't move it");
    TEST_ASSERT(mn_unwrap_cek(host_key, id, &h.wraps[slot], cek) == 0, "unwrap");
    TEST_ASSERT(mn_cek_import(cek, &cek_key) == 0, "cek");
    TEST_ASSERT(mn_header_core_hash(file, core_hash) == 0, "core hash");
    struct mn_mnr_chunk_hdr ch;

    TEST_ASSERT(mn_mnr_chunk_hdr_parse(&file[hdr_len], &ch) == 0, "chunk header");
    TEST_ASSERT(mn_chunk_open(cek_key, id, ch.seq, core_hash, &file[hdr_len + 6], ch.ct_len + 16,
                              pt, sizeof(pt)) == ch.ct_len,
                "chunk doesn't authenticate");
    psa_destroy_key(cek_key);

    size_t hlen;

    TEST_ASSERT(
        psa_hash_compute(PSA_ALG_SHA_256, file, len, req.body.ack.sha256, 32, &hlen) == PSA_SUCCESS,
        "hash");
    req.body.ack.recording_id = id;
    call(micnote_v1_Frame_ack_tag, micnote_v1_Frame_ok_tag, "Ack", 5000);
}

static uint16_t read_psm(struct bt_conn *conn)
{
    uint8_t v[2];

    const int r = hl_read(conn, 0x03, v, sizeof(v));

    TEST_ASSERT(r == 2, "psm read: %d", r);
    return sys_get_le16(v);
}

void host_main(void)
{
    bool pending;
    size_t len;
    uint8_t hash[32];
    uint8_t status[MN_BLE_STATUS_LEN];

    TEST_ASSERT(psa_crypto_init() == PSA_SUCCESS, "psa");
    TEST_ASSERT(mn_x25519_import(host_priv, &host_key) == 0, "host key");
    TEST_ASSERT(psa_export_public_key(host_key, host_pub, 32, &len) == PSA_SUCCESS, "pub");
    TEST_ASSERT(psa_hash_compute(PSA_ALG_SHA_256, host_pub, 32, hash, 32, &len) == PSA_SUCCESS,
                "id");
    memcpy(host_id, hash, 8);
    /* Bonds and the identity address live in settings (CONFIG_BT_SETTINGS). */
    TEST_ASSERT(settings_subsys_init() == 0, "settings");
    TEST_ASSERT(bt_enable(NULL) == 0, "bt");
    TEST_ASSERT(settings_load() == 0, "settings load");

    /* 1-2: bond, pair, sync over L2CAP. */
    struct bt_conn *conn = hl_connect(&pending);

    TEST_ASSERT(pending, "the puck should advertise sync_pending");
    TEST_ASSERT(hl_encrypt(conn) == 0, "bonding");
    const uint16_t psm = read_psm(conn);

    TEST_ASSERT(hl_open(conn, HL_L2CAP, psm) == 0, "L2CAP to psm 0x%04x", psm);
    TEST_PRINT("host: L2CAP open on psm 0x%04x, tx MTU %u", psm, hl_l2cap_tx_mtu());
    pair();
    req.body.set_time.unix_ms = 1763900000000ULL;
    call(micnote_v1_Frame_set_time_tag, micnote_v1_Frame_ok_tag, "SetTime", 5000);
    call(micnote_v1_Frame_list_recordings_tag, micnote_v1_Frame_recording_list_tag, "List", 5000);
    TEST_ASSERT(resp.body.recording_list.entries_count == 2, "%u recordings",
                resp.body.recording_list.entries_count);
    const uint64_t ids[2] = {resp.body.recording_list.entries[0].id,
                             resp.body.recording_list.entries[1].id};

    /* The puck asks for 15 ms (12 x 1.25 ms) once the channel is busy. */
    uint16_t interval;
    uint16_t latency;

    hl_interval(&interval, &latency);
    TEST_ASSERT(interval == 12 && latency == 0, "transfer interval %u, latency %u", interval,
                latency);
    sync_one(ids[0]);
    sync_one(ids[1]);
    call(micnote_v1_Frame_list_recordings_tag, micnote_v1_Frame_recording_list_tag, "List", 5000);
    TEST_ASSERT(resp.body.recording_list.entries_count == 0, "acked recordings still listed");
    hl_disconnect(conn);

    /* 3: back with the bond and the link key. */
    conn = hl_connect(&pending);
    TEST_ASSERT(!pending, "nothing left to sync");
    TEST_ASSERT(hl_encrypt(conn) == 0, "re-encryption with the bond");
    TEST_ASSERT(hl_read(conn, 0x04, status, sizeof(status)) == MN_BLE_STATUS_LEN, "status");
    TEST_ASSERT(status[0] == 80, "status battery %u", status[0]);
    TEST_ASSERT(hl_open(conn, HL_L2CAP, read_psm(conn)) == 0, "L2CAP again");
    authenticate();
    call(micnote_v1_Frame_get_status_tag, micnote_v1_Frame_device_status_tag, "GetStatus", 5000);
    TEST_ASSERT(resp.body.device_status.recording_count == 0, "status count");
    /* Quiet for a while: back to the idle interval (100-500 ms, latency 4). */
    k_sleep(K_MSEC(MN_BLE_IDLE_MS + 2000));
    hl_interval(&interval, &latency);
    TEST_ASSERT(interval >= 80 && interval <= 400 && latency == 4, "idle interval %u, latency %u",
                interval, latency);
    hl_disconnect(conn);

    /* 4: the GATT fallback. */
    conn = hl_connect(&pending);
    TEST_ASSERT(hl_encrypt(conn) == 0, "re-encryption");
    TEST_ASSERT(hl_open(conn, HL_GATT, 0) == 0, "GATT stream");
    authenticate();
    call(micnote_v1_Frame_list_hosts_tag, micnote_v1_Frame_host_list_tag, "ListHosts", 5000);
    TEST_ASSERT(resp.body.host_list.hosts_count == 1, "hosts");
    TEST_ASSERT(strcmp(resp.body.host_list.hosts[0].name, "bsim host") == 0, "host name");
    hl_disconnect(conn);

    TEST_PASS("host: bonded, paired, synced over L2CAP, authenticated over L2CAP and GATT");
}
