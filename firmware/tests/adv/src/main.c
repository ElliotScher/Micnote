#include <string.h>

#include <zephyr/ztest.h>

#include "adv.h"

ZTEST(adv, test_payload_fits_and_parses_back)
{
    uint8_t ad[MN_ADV_MAX_LEN];
    bool pending = false;

    for (int p = 0; p < 2; p++) {
        const size_t n = mn_adv_payload(p == 1, ad);

        zassert_equal(n, 26, "flags 3 + UUID 18 + manufacturer data 5");
        zassert_true(n <= MN_ADV_MAX_LEN);
        zassert_true(mn_adv_parse(ad, n, &pending));
        zassert_equal(pending, p == 1);
    }
}

ZTEST(adv, test_payload_bytes)
{
    static const uint8_t want[26] = {
        0x02, 0x01, 0x06, /* flags */
        0x11, 0x07,       /* 128-bit UUIDs */
        0xb7, 0xaa, 0xa0, 0x1f, 0x29, 0x40, 0x5e, 0x8c,
        0x50, 0x4e, 0x00, 0x00, 0x06, 0xe6, 0x42, 0x44, /* 4442e606-0000-4e50-... */
        0x04, 0xff, 0xff, 0xff, 0x01,                   /* 0xFFFF, sync_pending */
    };
    uint8_t ad[MN_ADV_MAX_LEN];

    zassert_equal(mn_adv_payload(true, ad), sizeof(want));
    zassert_mem_equal(ad, want, sizeof(want));
}

ZTEST(adv, test_parse_rejects_others)
{
    uint8_t ad[MN_ADV_MAX_LEN];
    bool pending = true;
    const size_t n = mn_adv_payload(true, ad);

    /* Another UUID. */
    ad[5] ^= 1;
    zassert_false(mn_adv_parse(ad, n, &pending));
    ad[5] ^= 1;

    /* A field running past the end stops the parse. */
    zassert_false(mn_adv_parse(ad, 10, &pending));
    zassert_false(mn_adv_parse(ad, 0, &pending));

    /* Another company's data isn't our sync_pending. */
    ad[23] = 0x59;
    zassert_true(mn_adv_parse(ad, n, &pending));
    zassert_false(pending);
}

ZTEST(adv, test_uuid_among_others)
{
    /* Flags, then a 16-bit UUID list, then ours in an incomplete list. */
    uint8_t ad[31] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x0f, 0x18, 0x11, 0x06};
    uint8_t ours[MN_ADV_MAX_LEN];
    bool pending;

    mn_adv_payload(false, ours);
    memcpy(&ad[9], &ours[5], 16);
    zassert_true(mn_adv_parse(ad, 25, &pending));
    zassert_false(pending);
}

ZTEST(adv, test_timings)
{
    const struct mn_adv_timing fast = mn_adv_timing(MN_ADV_FAST);
    const struct mn_adv_timing slow = mn_adv_timing(MN_ADV_SLOW);
    const struct mn_adv_timing off = mn_adv_timing(MN_ADV_OFF);

    zassert_equal(fast.interval_min * 625 / 1000, 30, "30 ms");
    zassert_equal(fast.duration_ms, 60000);
    zassert_equal(slow.interval_min * 625 / 1000, 1000, "1 s");
    zassert_equal(slow.duration_ms, 300000);
    zassert_true(fast.interval_max >= fast.interval_min && slow.interval_max >= slow.interval_min);
    zassert_equal(off.duration_ms, 0);
}

ZTEST_SUITE(adv, NULL, NULL, NULL, NULL, NULL);
