/* BabbleSim test registration: one image, three roles chosen by -testid. */
#include "bstests.h"
#include "roles.h"

static const struct bst_test_instance tests[] = {
    {.test_id = "puck", .test_descr = "The puck: BLE, dispatcher, store", .test_main_f = puck_main},
    {.test_id = "host",
     .test_descr = "A host: bond, pair, sync, authenticate",
     .test_main_f = host_main},
    {.test_id = "stranger",
     .test_descr = "An unpaired central, after the window",
     .test_main_f = stranger_main},
    BSTEST_END_MARKER,
};

static struct bst_test_list *install(struct bst_test_list *list)
{
    return bst_add_tests(list, tests);
}

bst_test_install_t test_installers[] = {install, NULL};

int main(void)
{
    bst_main();
    return 0;
}
