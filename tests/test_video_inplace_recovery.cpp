#include "wd_video_transition.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x); std::exit(1);} } while(0)
int main() {
    // Auto mode cannot overtake keyframe recovery while video is available.
    CHECK(wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_AUTO, true, true, true));
    CHECK(wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_FORCE, true, true, true));
    CHECK(!wd_video_auto_mode_wait_for_recovery(false, WD_VIDEO_MODE_AUTO, true, true, true));
    // User disable or a lost video dependency must remain actionable.
    CHECK(!wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_OFF, true, true, true));
    CHECK(!wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_AUTO, false, true, true));
    CHECK(!wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_AUTO, true, false, true));
    CHECK(!wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_AUTO, true, true, false));
    // In-place recovery must retain the current epoch even though it forces
    // a keyframe. Initial entry from tiles must still reserve a new epoch.
    const wd_video_entry_plan recovering = wd_video_entry_plan_make(10, false, true);
    CHECK(recovering.frame_content_epoch == 10 && !recovering.commit_on_queue);
    const wd_video_entry_plan entering = wd_video_entry_plan_make(10, true, true);
    CHECK(entering.frame_content_epoch == 11 && entering.commit_on_queue);
    // Periodic and retransmitted keyframes cannot move the acknowledgement
    // target or prevent the controller's retry/fallback timeout from expiring.
    bool queued = false;
    uint64_t frame_id = 0;
    uint32_t waited = 2;
    CHECK(wd_video_recovery_track_keyframe(&queued, &frame_id, &waited, 1823));
    CHECK(queued && frame_id == 1823 && waited == 0);
    waited = 2;
    CHECK(!wd_video_recovery_track_keyframe(&queued, &frame_id, &waited, 1824));
    CHECK(!wd_video_recovery_track_keyframe(&queued, &frame_id, &waited, 1884));
    CHECK(frame_id == 1823 && waited == 2);
    CHECK(wd_video_recovery_decide(queued, frame_id, 1822, 3, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_RETRY_KEYFRAME);
    queued = false; frame_id = 0; waited = 0;
    CHECK(wd_video_recovery_track_keyframe(&queued, &frame_id, &waited, 1944));
    CHECK(frame_id == 1944);
    CHECK(wd_video_recovery_decide(queued, frame_id, 1943, 3, 3, 2, 2) == WD_VIDEO_RECOVERY_ACTION_FALLBACK_TILES);
    CHECK(wd_video_recovery_decide(false, 0, 0, 0, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_WAIT);
    CHECK(wd_video_recovery_decide(true, 50, 49, 2, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_WAIT);
    CHECK(wd_video_recovery_decide(true, 50, 50, 1, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_PRESENTED);
    CHECK(wd_video_recovery_decide(true, 50, 49, 3, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_RETRY_KEYFRAME);
    CHECK(wd_video_recovery_decide(true, 50, 49, 3, 3, 2, 2) == WD_VIDEO_RECOVERY_ACTION_FALLBACK_TILES);
    return 0;
}
