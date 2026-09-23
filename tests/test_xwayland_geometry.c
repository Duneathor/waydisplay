#include "wd_xwayland_policy.h"

#include <assert.h>

int main(void) {
    assert(wd_xwayland_valid_dimension(1, 800) == 1);
    assert(wd_xwayland_valid_dimension(48, 600) == 48);
    assert(wd_xwayland_valid_dimension(0, 800) == 800);
    assert(wd_xwayland_needs_decoration(true, true, false));
    assert(!wd_xwayland_needs_decoration(true, true, true));
    assert(!wd_xwayland_needs_decoration(false, true, false));
    assert(!wd_xwayland_needs_decoration(true, false, false));
    assert(wd_xwayland_content_height(773, true, 28) == 745);
    assert(wd_xwayland_content_height(773, false, 28) == 773);
    assert(wd_xwayland_content_height(12, true, 28) == 12);
    assert(wd_xwayland_decoration_layout_changed(false, 800, true, 800, true));
    assert(!wd_xwayland_decoration_layout_changed(true, 800, true, 800, true));
    assert(wd_xwayland_decoration_layout_changed(true, 800, true, 800, false));
    assert(wd_xwayland_decoration_layout_changed(true, 800, true, 801, true));
    return 0;
}
