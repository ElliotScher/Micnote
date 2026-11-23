/*
 * Advertising (stage 1 §4.7): the three modes and the payload, as pure data,
 * so they can be tested without a radio. ble.c turns them into
 * bt_le_adv_start() calls.
 *
 *   FAST  30 ms for 60 s   after a snap or a button press
 *   SLOW  1 s for 5 min    after a recording ends
 *   OFF                    the default
 *
 * The payload: flags, the complete 128-bit service UUID, and manufacturer
 * data with Bluetooth SIG's test company ID 0xFFFF and one flags byte
 * (bit 0: sync_pending). §4.7 asked for service data, but service data
 * keyed by a 128-bit UUID doesn't fit next to the UUID in 31 bytes; the
 * company ID gets replaced once Micnote has one (stage 5b).
 */
#ifndef MICNOTE_BLE_ADV_H_
#define MICNOTE_BLE_ADV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The service UUID, generated 2026-11-23, in the byte order the air uses
 * (little-endian). Characteristics differ from it in the second group:
 * 4442e606-000N-4e50-8c5e-40291fa0aab7, N = 0 for the service, 1 ctrl,
 * 2 data, 3 psm, 4 status. */
#define MN_BLE_UUID_BYTES(n) \
    0xb7, 0xaa, 0xa0, 0x1f, 0x29, 0x40, 0x5e, 0x8c, 0x50, 0x4e, (n), 0x00, 0x06, 0xe6, 0x42, 0x44

#define MN_BLE_COMPANY_ID 0xFFFF
#define MN_BLE_ADV_FLAG_SYNC_PENDING 0x01

enum mn_adv_mode {
    MN_ADV_OFF,
    MN_ADV_FAST,
    MN_ADV_SLOW,
};

struct mn_adv_timing {
    /* In units of 0.625 ms, as the controller takes them. */
    uint16_t interval_min;
    uint16_t interval_max;
    /* How long to advertise before going back to OFF; 0 for OFF itself. */
    uint32_t duration_ms;
};

struct mn_adv_timing mn_adv_timing(enum mn_adv_mode mode);

/* The manufacturer data field's value: company ID (LE) | flags. */
#define MN_ADV_MFG_LEN 3
void mn_adv_mfg_data(bool sync_pending, uint8_t out[MN_ADV_MFG_LEN]);

/* The complete advertising payload, as AD structures (len | type | data),
 * for checking it against the 31-byte limit and for scanners in tests.
 * Returns its length. */
#define MN_ADV_MAX_LEN 31
size_t mn_adv_payload(bool sync_pending, uint8_t out[MN_ADV_MAX_LEN]);

/* Parses a payload back: true if it advertises the Micnote service, with
 * *sync_pending from the manufacturer data (false if absent). */
bool mn_adv_parse(const uint8_t *ad, size_t len, bool *sync_pending);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_BLE_ADV_H_ */
