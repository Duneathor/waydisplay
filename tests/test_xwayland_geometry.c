#include "wd_xwayland_policy.h"

#include "test_check.h"

int main(void) {
    WD_TEST_CHECK(wd_xwayland_valid_dimension(1, 800) == 1);
    WD_TEST_CHECK(wd_xwayland_valid_dimension(48, 600) == 48);
    WD_TEST_CHECK(wd_xwayland_valid_dimension(0, 800) == 800);
    WD_TEST_CHECK(wd_xwayland_needs_decoration(true, true, false));
    WD_TEST_CHECK(!wd_xwayland_needs_decoration(true, true, true));
    WD_TEST_CHECK(!wd_xwayland_needs_decoration(false, true, false));
    WD_TEST_CHECK(!wd_xwayland_needs_decoration(true, false, false));
    WD_TEST_CHECK(wd_xwayland_content_height(773, true, 28) == 745);
    WD_TEST_CHECK(wd_xwayland_content_height(773, false, 28) == 773);
    WD_TEST_CHECK(wd_xwayland_content_height(12, true, 28) == 12);
    WD_TEST_CHECK(wd_xwayland_decoration_layout_changed(false, 800, true, 800, true));
    WD_TEST_CHECK(!wd_xwayland_decoration_layout_changed(true, 800, true, 800, true));
    WD_TEST_CHECK(wd_xwayland_decoration_layout_changed(true, 800, true, 800, false));
    WD_TEST_CHECK(wd_xwayland_decoration_layout_changed(true, 800, true, 801, true));
    return 0;
}
