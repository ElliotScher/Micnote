#include "adv.h"

#include <string.h>

/* AD types (Bluetooth Core Supplement, part A). */
#define AD_FLAGS 0x01
#define AD_UUID128_ALL 0x07
#define AD_MANUFACTURER 0xFF
/* LE General Discoverable, BR/EDR not supported. */
#define AD_FLAGS_VALUE 0x06

static const uint8_t service_uuid[16] = {MN_BLE_UUID_BYTES(0x00)};

struct mn_adv_timing mn_adv_timing(enum mn_adv_mode mode)
{
    switch (mode) {
        case MN_ADV_FAST:
            /* 30 ms (48 x 0.625 ms), with a little room for the controller. */
            return (struct mn_adv_timing){
                .interval_min = 48, .interval_max = 60, .duration_ms = 60 * 1000};
        case MN_ADV_SLOW:
            /* 1 s (1600 x 0.625 ms). */
            return (struct mn_adv_timing){
                .interval_min = 1600, .interval_max = 1760, .duration_ms = 5 * 60 * 1000};
        default:
            return (struct mn_adv_timing){0};
    }
}

void mn_adv_mfg_data(bool sync_pending, uint8_t out[MN_ADV_MFG_LEN])
{
    out[0] = MN_BLE_COMPANY_ID & 0xFF;
    out[1] = MN_BLE_COMPANY_ID >> 8;
    out[2] = sync_pending ? MN_BLE_ADV_FLAG_SYNC_PENDING : 0;
}

size_t mn_adv_payload(bool sync_pending, uint8_t out[MN_ADV_MAX_LEN])
{
    size_t n = 0;

    out[n++] = 2;
    out[n++] = AD_FLAGS;
    out[n++] = AD_FLAGS_VALUE;
    out[n++] = 1 + sizeof(service_uuid);
    out[n++] = AD_UUID128_ALL;
    memcpy(&out[n], service_uuid, sizeof(service_uuid));
    n += sizeof(service_uuid);
    out[n++] = 1 + MN_ADV_MFG_LEN;
    out[n++] = AD_MANUFACTURER;
    mn_adv_mfg_data(sync_pending, &out[n]);
    n += MN_ADV_MFG_LEN;
    return n; /* 3 + 18 + 5 = 26 */
}

bool mn_adv_parse(const uint8_t *ad, size_t len, bool *sync_pending)
{
    bool ours = false;

    *sync_pending = false;
    for (size_t i = 0; i < len;) {
        const size_t field = ad[i];

        if (field == 0 || i + 1 + field > len) {
            break; /* padding, or a truncated field */
        }
        const uint8_t type = ad[i + 1];
        const uint8_t *data = &ad[i + 2];
        const size_t data_len = field - 1;

        if ((type == AD_UUID128_ALL || type == AD_UUID128_ALL - 1) && data_len % 16 == 0) {
            for (size_t u = 0; u < data_len; u += 16) {
                ours = ours || memcmp(&data[u], service_uuid, 16) == 0;
            }
        } else if (type == AD_MANUFACTURER && data_len >= MN_ADV_MFG_LEN && data[0] == 0xFF &&
                   data[1] == 0xFF) {
            *sync_pending = (data[2] & MN_BLE_ADV_FLAG_SYNC_PENDING) != 0;
        }
        i += 1 + field;
    }
    return ours;
}
