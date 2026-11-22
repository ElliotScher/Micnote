/*
 * Storage policy (stage 1 section 4.9): the OK/WARN/FULL boundaries, the
 * 64 KB free-space floor, nonsense inputs, and when recording is allowed.
 * Sizes are multiples of 100 so "exactly N%" really is exact. KB(), MB()
 * and GB() come from <zephyr/sys/util.h>.
 */
#include <zephyr/ztest.h>

#include "policy.h"

ZTEST_SUITE(policy, NULL, NULL, NULL, NULL, NULL);

ZTEST(policy, test_empty_store_is_ok)
{
    zassert_equal(mn_policy_storage(0, MB(100ULL)), MN_STORAGE_OK);
}

ZTEST(policy, test_ok_one_byte_under_90_percent)
{
    zassert_equal(mn_policy_storage(MB(90ULL) - 1, MB(100ULL)), MN_STORAGE_OK);
}

ZTEST(policy, test_warn_at_exactly_90_percent)
{
    zassert_equal(mn_policy_storage(MB(90ULL), MB(100ULL)), MN_STORAGE_WARN);
}

ZTEST(policy, test_warn_one_byte_under_99_percent)
{
    zassert_equal(mn_policy_storage(MB(99ULL) - 1, MB(100ULL)), MN_STORAGE_WARN);
}

ZTEST(policy, test_full_at_exactly_99_percent)
{
    zassert_equal(mn_policy_storage(MB(99ULL), MB(100ULL)), MN_STORAGE_FULL);
}

ZTEST(policy, test_full_when_completely_used)
{
    zassert_equal(mn_policy_storage(MB(100ULL), MB(100ULL)), MN_STORAGE_FULL);
}

/* On a large store the 64 KB floor fires long before 99%: 60 KB free of
 * 1 GB is still under 99.99% used. */
ZTEST(policy, test_full_below_64_kb_free_on_large_store)
{
    zassert_equal(mn_policy_storage(GB(1ULL) - KB(60ULL), GB(1ULL)), MN_STORAGE_FULL);
}

/* The floor is "less than 64 KB free": exactly 64 KB free still records. A
 * 64 KB store isolates it, since any use at all drops below the floor. */
ZTEST(policy, test_floor_is_exclusive)
{
    zassert_equal(mn_policy_storage(0, KB(64ULL)), MN_STORAGE_OK);
    zassert_equal(mn_policy_storage(1, KB(64ULL)), MN_STORAGE_FULL);
}

/* On a small store the floor dominates: 100 KB total can use at most 36 KB,
 * long before the 90% warning. */
ZTEST(policy, test_floor_on_small_store)
{
    zassert_equal(mn_policy_storage(KB(36ULL), KB(100ULL)), MN_STORAGE_OK);
    zassert_equal(mn_policy_storage(KB(36ULL) + 1, KB(100ULL)), MN_STORAGE_FULL);
}

ZTEST(policy, test_zero_size_store_is_full)
{
    zassert_equal(mn_policy_storage(0, 0), MN_STORAGE_FULL);
}

/* used > total would underflow total - used if it weren't checked first. */
ZTEST(policy, test_overfull_store_is_full)
{
    zassert_equal(mn_policy_storage(MB(100ULL) + 1, MB(100ULL)), MN_STORAGE_FULL);
    zassert_equal(mn_policy_storage(UINT64_MAX, 1), MN_STORAGE_FULL);
}

ZTEST(policy, test_may_record_when_ok)
{
    zassert_true(mn_policy_may_record(MN_STORAGE_OK, 100));
}

/* WARN only flashes amber; recording carries on until FULL. */
ZTEST(policy, test_may_record_when_warn)
{
    zassert_true(mn_policy_may_record(MN_STORAGE_WARN, 50));
}

ZTEST(policy, test_may_not_record_when_full)
{
    zassert_false(mn_policy_may_record(MN_STORAGE_FULL, 100));
}

ZTEST(policy, test_battery_threshold)
{
    zassert_false(mn_policy_may_record(MN_STORAGE_OK, 0));
    zassert_false(mn_policy_may_record(MN_STORAGE_OK, 4));
    zassert_true(mn_policy_may_record(MN_STORAGE_OK, 5));
}
