#include "hosts.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#define SUBTREE "mn/hosts"

/* What settings stores per slot. */
struct record {
    uint8_t host_id[8];
    uint8_t pub[32];
    char name[MN_HOST_NAME_MAX];
};

static struct mn_host table[MN_HOSTS_MAX];

static int load_one(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                    void *param)
{
    ARG_UNUSED(param);
    struct record r;
    int slot;

    /* The key is relative to the subtree: just the slot digit. */
    if (key == NULL || key[0] < '0' || key[0] >= '0' + MN_HOSTS_MAX || key[1] != '\0' ||
        len != sizeof(r)) {
        return 0; /* not a slot record: ignore it */
    }
    slot = key[0] - '0';
    if (read_cb(cb_arg, &r, sizeof(r)) != sizeof(r)) {
        return 0;
    }
    table[slot].used = true;
    memcpy(table[slot].host_id, r.host_id, 8);
    memcpy(table[slot].pub, r.pub, 32);
    memcpy(table[slot].name, r.name, sizeof(r.name));
    table[slot].name[MN_HOST_NAME_MAX - 1] = '\0';
    return 0;
}

static void slot_key(int slot, char out[sizeof(SUBTREE "/0")])
{
    snprintf(out, sizeof(SUBTREE "/0"), SUBTREE "/%d", slot);
}

int mn_hosts_init(void)
{
    memset(table, 0, sizeof(table));
    return settings_load_subtree_direct(SUBTREE, load_one, NULL);
}

const struct mn_host *mn_hosts_get(int slot)
{
    if (slot < 0 || slot >= MN_HOSTS_MAX || !table[slot].used) {
        return NULL;
    }
    return &table[slot];
}

size_t mn_hosts_count(void)
{
    size_t n = 0;

    for (int i = 0; i < MN_HOSTS_MAX; i++) {
        n += table[i].used ? 1 : 0;
    }
    return n;
}

int mn_hosts_find(const uint8_t host_id[8])
{
    for (int i = 0; i < MN_HOSTS_MAX; i++) {
        if (table[i].used && memcmp(table[i].host_id, host_id, 8) == 0) {
            return i;
        }
    }
    return -ENOENT;
}

int mn_hosts_add(const uint8_t host_id[8], const uint8_t pub[32], const char *name,
                 psa_key_id_t link_key)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    char key[sizeof(SUBTREE "/0")];
    struct record r;
    psa_key_id_t copied = PSA_KEY_ID_NULL;
    int slot = mn_hosts_find(host_id);

    for (int i = 0; slot < 0 && i < MN_HOSTS_MAX; i++) {
        if (!table[i].used) {
            slot = i;
        }
    }
    if (slot < 0) {
        return -ENOSPC;
    }

    /* The persistent copy: same type and policy, minus COPY. A key left in
     * this slot by an earlier pairing (or one interrupted before its
     * settings record was saved) goes first. */
    if (psa_get_key_attributes(link_key, &attr) != PSA_SUCCESS) {
        return -EIO;
    }
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_PERSISTENT);
    psa_set_key_id(&attr, MN_KEY_ID_LINK(slot));
    (void)psa_destroy_key(MN_KEY_ID_LINK(slot));
    const psa_status_t st = psa_copy_key(link_key, &attr, &copied);

    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        return -EIO;
    }

    memset(&r, 0, sizeof(r));
    memcpy(r.host_id, host_id, 8);
    memcpy(r.pub, pub, 32);
    strncpy(r.name, name != NULL ? name : "", sizeof(r.name) - 1);
    slot_key(slot, key);
    const int ret = settings_save_one(key, &r, sizeof(r));

    if (ret < 0) {
        (void)psa_destroy_key(copied);
        return ret;
    }
    table[slot].used = true;
    memcpy(table[slot].host_id, r.host_id, 8);
    memcpy(table[slot].pub, r.pub, 32);
    memcpy(table[slot].name, r.name, sizeof(r.name));
    return slot;
}

int mn_hosts_remove(const uint8_t host_id[8])
{
    char key[sizeof(SUBTREE "/0")];
    const int slot = mn_hosts_find(host_id);

    if (slot < 0) {
        return slot;
    }
    slot_key(slot, key);
    const int ret = settings_delete(key);

    if (ret < 0) {
        return ret;
    }
    (void)psa_destroy_key(MN_KEY_ID_LINK(slot));
    memset(&table[slot], 0, sizeof(table[slot]));
    return 0;
}

size_t mn_hosts_recipients(struct mn_rec_recipient out[MN_HOSTS_MAX])
{
    size_t n = 0;

    for (int i = 0; i < MN_HOSTS_MAX; i++) {
        if (table[i].used) {
            memcpy(out[n].pub, table[i].pub, 32);
            memcpy(out[n].host_id, table[i].host_id, 8);
            n++;
        }
    }
    return n;
}
