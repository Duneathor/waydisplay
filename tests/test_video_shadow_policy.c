#include "video_shadow_policy.h"

#include <assert.h>

int main(void) {
    assert(wd_video_shadow_skip_diff(true, false, false, false));
    assert(!wd_video_shadow_skip_diff(false, false, false, false)); /* video-ready is tile-owned */
    assert(!wd_video_shadow_skip_diff(true, true, false, false));
    assert(!wd_video_shadow_skip_diff(true, false, true, false));
    assert(!wd_video_shadow_skip_diff(true, false, false, true));
    assert(!wd_video_shadow_skip_diff(false, true, true, true));
    return 0;
}
