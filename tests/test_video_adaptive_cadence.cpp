#include "wd_video_transition.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x); std::exit(1);} } while(0)
int main() {
    CHECK(wd_video_safe_decode_fps(0, 30, 85) == 30);
    CHECK(wd_video_safe_decode_fps(34000000ull, 30, 85) == 25);
    CHECK(wd_video_safe_decode_fps(50000000ull, 30, 85) == 17);
    CHECK(wd_video_safe_decode_fps(10000000ull, 30, 85) == 30);
    wd_client_video_health_metrics m{};
    m.server_frames_tx=1; m.client_reports=1; m.client_decode_queue_drops=1;
    CHECK(wd_client_video_health_classify(&m)==WD_CLIENT_VIDEO_HEALTH_DECODER_OVERLOADED);
    m.client_decode_queue_drops=0; m.client_need_keyframe_drops=1;
    CHECK(wd_client_video_health_classify(&m)==WD_CLIENT_VIDEO_HEALTH_AWAITING_KEYFRAME);
    m.client_need_keyframe_drops=0; m.client_decode_failures=1;
    CHECK(wd_client_video_health_classify(&m)==WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE);
    return 0;
}
