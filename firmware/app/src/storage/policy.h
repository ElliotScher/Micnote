#ifndef MICNOTE_STORAGE_POLICY_H_
#define MICNOTE_STORAGE_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

enum mn_storage_state {
    MN_STORAGE_OK,
    MN_STORAGE_WARN, /* >= 90% used */
    MN_STORAGE_FULL  /* >= 99% used, < 64 KB free */
};

enum mn_storage_state mn_policy_storage(uint64_t used_bytes, uint64_t total_bytes);

/* False when storage is FULL or the battery is below 5%. */
bool mn_policy_may_record(enum mn_storage_state s, uint8_t battery_pct);

#endif  // MICNOTE_STORAGE_POLICY_H_
