#include "video_shadow_policy.h"

#include "test_check.h"

int main(void) {
    WD_TEST_CHECK(wd_video_shadow_skip_diff(true, false, false, false));
    WD_TEST_CHECK(!wd_video_shadow_skip_diff(false, false, false, false)); /* video-ready is tile-owned */
    WD_TEST_CHECK(!wd_video_shadow_skip_diff(true, true, false, false));
    WD_TEST_CHECK(!wd_video_shadow_skip_diff(true, false, true, false));
    WD_TEST_CHECK(!wd_video_shadow_skip_diff(true, false, false, true));
    WD_TEST_CHECK(!wd_video_shadow_skip_diff(false, true, true, true));

    /* A GPU-only mailbox item is acceptable while video can skip the CPU
     * shadow, but an ownership/refresh transition requires a CPU capture. */
    WD_TEST_CHECK(!wd_video_shadow_cpu_capture_required(true, false, false, false, false));
    WD_TEST_CHECK(wd_video_shadow_cpu_capture_required(false, false, false, false, false));
    WD_TEST_CHECK(wd_video_shadow_cpu_capture_required(true, true, false, false, false));
    WD_TEST_CHECK(wd_video_shadow_cpu_capture_required(true, false, true, false, false));
    WD_TEST_CHECK(!wd_video_shadow_cpu_capture_required(false, false, false, false, true));
    return 0;
}
