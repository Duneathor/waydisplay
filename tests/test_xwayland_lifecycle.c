#include "wd_xwayland_lifecycle.h"

#include "test_check.h"

int main(void) {
    WD_TEST_CHECK(!wd_xwayland_should_show(false, false, false)); /* map_request before associate */
    WD_TEST_CHECK(!wd_xwayland_should_show(true, false, false));  /* associate before buffer map */
    WD_TEST_CHECK(wd_xwayland_should_show(true, true, false));   /* actual mapped surface */
    WD_TEST_CHECK(!wd_xwayland_should_show(true, true, true));   /* minimized window */
    WD_TEST_CHECK(!wd_xwayland_should_show(false, true, false)); /* dissociated surface */
    return 0;
}
