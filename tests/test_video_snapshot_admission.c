#include "video_snapshot_admission.h"
#include "test_check.h"

int main(void) {
    WD_TEST_CHECK(wd_video_snapshot_preflight_decide(false, false) == WD_VIDEO_SNAPSHOT_UNAVAILABLE);
    WD_TEST_CHECK(wd_video_snapshot_preflight_decide(true, true) == WD_VIDEO_SNAPSHOT_PENDING_SEND);
    WD_TEST_CHECK(wd_video_snapshot_preflight_decide(true, false) == WD_VIDEO_SNAPSHOT_ACCEPT);
    WD_TEST_CHECK(wd_video_snapshot_admission_percent(30, 60) == 50.0);
    WD_TEST_CHECK(wd_video_snapshot_admission_percent(0, 0) == 0.0);
    return 0;
}
