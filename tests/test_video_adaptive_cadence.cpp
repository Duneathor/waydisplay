#include "wd_video_transition.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x); std::exit(1);} } while(0)
int main() {
    CHECK(wd_video_safe_decode_fps(0, 30, 85) == 30);
    CHECK(wd_video_safe_decode_fps(34000000ull, 30, 85) == 25);
    CHECK(wd_video_safe_decode_fps(50000000ull, 30, 85) == 17);
    CHECK(wd_video_safe_decode_fps(10000000ull, 30, 85) == 30);
    CHECK(wd_video_decode_ewma_update(0, 34000000ull, 1, 4) == 34000000ull);
    CHECK(wd_video_decode_ewma_update(30000000ull, 34000000ull, 1, 4) == 31000000ull);
    CHECK(wd_video_cadence_downshift_target(30, 30, 25, false, 5, 2, 75) == 25);
    CHECK(wd_video_cadence_downshift_target(26, 30, 25, false, 5, 2, 75) == 26);
    CHECK(wd_video_cadence_downshift_target(30, 30, 25, true, 5, 2, 75) == 22);
    CHECK(wd_video_cadence_upshift_target(19, 30, 25, 2, 1) == 20);
    CHECK(wd_video_cadence_upshift_target(24, 30, 25, 2, 1) == 24);
    CHECK(wd_video_cadence_upshift_target(28, 30, 30, 2, 1) == 29);
    uint16_t stable_fps = wd_video_cadence_downshift_target(30, 30, 25, false, 5, 2, 75);
    CHECK(stable_fps == 25);
    for (unsigned i = 0; i < 120; ++i)
    {
        stable_fps = wd_video_cadence_upshift_target(stable_fps, 30, 25, 2, 1);
        CHECK(stable_fps == 25);
        stable_fps = wd_video_cadence_downshift_target(stable_fps, 30, 25, false, 5, 2, 75);
        CHECK(stable_fps == 25);
    }
    uint16_t recovering_fps = 25;
    for (unsigned i = 0; i < 5; ++i)
    {
        recovering_fps = wd_video_cadence_upshift_target(recovering_fps, 30, 30, 2, 1);
    }
    CHECK(recovering_fps == 30);
    wd_client_video_health_metrics m{};
    m.server_frames_tx=1; m.client_reports=1; m.client_decode_queue_drops=1;
    CHECK(wd_client_video_health_classify(&m)==WD_CLIENT_VIDEO_HEALTH_DECODER_OVERLOADED);
    m.client_decode_queue_drops=0; m.client_need_keyframe_drops=1;
    CHECK(wd_client_video_health_classify(&m)==WD_CLIENT_VIDEO_HEALTH_AWAITING_KEYFRAME);
    m.client_need_keyframe_drops=0; m.client_decode_failures=1;
    CHECK(wd_client_video_health_classify(&m)==WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE);
    return 0;
}
