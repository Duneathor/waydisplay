#include "wd_video_transition.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x); std::exit(1);} } while(0)
int main() {
    CHECK(wd_video_recovery_decide(false, 0, 0, 0, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_WAIT);
    CHECK(wd_video_recovery_decide(true, 50, 49, 2, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_WAIT);
    CHECK(wd_video_recovery_decide(true, 50, 50, 1, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_PRESENTED);
    CHECK(wd_video_recovery_decide(true, 50, 49, 3, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_RETRY_KEYFRAME);
    CHECK(wd_video_recovery_decide(true, 50, 49, 3, 3, 2, 2) == WD_VIDEO_RECOVERY_ACTION_FALLBACK_TILES);
    return 0;
}
