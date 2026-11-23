/*
 * The stranger role: a central that turns up after the host has paired
 * (the pairing window is shut). It must not get a bond, nor read anything,
 * nor open the L2CAP channel.
 */
#include <errno.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "babblekit/testcase.h"
#include "roles.h"

void stranger_main(void)
{
    bool pending;
    uint8_t v[8];

    /* Bonds and the identity address live in settings (CONFIG_BT_SETTINGS). */
    TEST_ASSERT(settings_subsys_init() == 0, "settings");
    TEST_ASSERT(bt_enable(NULL) == 0, "bt");
    TEST_ASSERT(settings_load() == 0, "settings load");
    /* Let the host pair and sync first. */
    k_sleep(K_SECONDS(40));

    struct bt_conn *conn = hl_connect(&pending);
    const int r = hl_read(conn, 0x03, v, sizeof(v));

    TEST_ASSERT(r == -BT_ATT_ERR_AUTHENTICATION || r == -BT_ATT_ERR_INSUFFICIENT_ENCRYPTION,
                "psm readable without encryption: %d", r);
    const int sec = hl_encrypt(conn);

    TEST_ASSERT(sec != 0, "a stranger got a bond outside the pairing window");
    TEST_PRINT("stranger: bonding refused (%d), reads refused (%d)", sec, r);
    hl_disconnect(conn);
    TEST_PASS("stranger: kept out");
}
