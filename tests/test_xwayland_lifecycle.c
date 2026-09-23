#include "wd_xwayland_lifecycle.h"

#include <assert.h>

int main(void) {
    assert(!wd_xwayland_should_show(false, false, false)); /* map_request before associate */
    assert(!wd_xwayland_should_show(true, false, false));  /* associate before buffer map */
    assert(wd_xwayland_should_show(true, true, false));   /* actual mapped surface */
    assert(!wd_xwayland_should_show(true, true, true));   /* minimized window */
    assert(!wd_xwayland_should_show(false, true, false)); /* dissociated surface */
    return 0;
}
