#include "video_snapshot_admission.h"
#include <assert.h>

int main(void) {
    assert(wd_video_snapshot_preflight_decide(false, false) == WD_VIDEO_SNAPSHOT_UNAVAILABLE);
    assert(wd_video_snapshot_preflight_decide(true, true) == WD_VIDEO_SNAPSHOT_PENDING_SEND);
    assert(wd_video_snapshot_preflight_decide(true, false) == WD_VIDEO_SNAPSHOT_ACCEPT);
    assert(wd_video_snapshot_admission_percent(30, 60) == 50.0);
    assert(wd_video_snapshot_admission_percent(0, 0) == 0.0);
    return 0;
}
