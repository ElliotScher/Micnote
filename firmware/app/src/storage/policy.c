#include "policy.h"

#define MN_MIN_FREE_BYTES (64U * 1024U)

enum mn_storage_state mn_policy_storage(uint64_t used_bytes, uint64_t total_bytes)
{
    if (total_bytes == 0 || used_bytes > total_bytes) {
        return MN_STORAGE_FULL;
    }
    if ((total_bytes - used_bytes) < MN_MIN_FREE_BYTES || used_bytes * 100 >= total_bytes * 99) {
        return MN_STORAGE_FULL;
    }
    if (used_bytes * 100 >= total_bytes * 90) {
        return MN_STORAGE_WARN;
    }
    return MN_STORAGE_OK;
}

bool mn_policy_battery_low(uint8_t battery_pct)
{
    return battery_pct < 5;
}

bool mn_policy_may_record(enum mn_storage_state s, uint8_t battery_pct)
{
    return s != MN_STORAGE_FULL && !mn_policy_battery_low(battery_pct);
}
