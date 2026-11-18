#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

ZTEST_SUITE(smoke, NULL, NULL, NULL, NULL, NULL);

// The kernel's clock advances while a thread sleeps.
ZTEST(smoke, test_uptime_advances_across_sleep)
{
    int64_t start = k_uptime_get();

    k_msleep(10);

    zassert_true(k_uptime_get() - start >= 10, "uptime advanced less than the 10 ms slept");
}
