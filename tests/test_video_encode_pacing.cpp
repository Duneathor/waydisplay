#include "video_encode_pacing.h"
#include "video_encoder_clock.h"

#include "test_check.h"
#include <cstdint>

int main() {
    // Capture can move 60 -> 57 -> 51 -> 45 without changing FFmpeg's clock.
    WD_TEST_CHECK(wd_video_encoder_nominal_fps(60) == 60);
    WD_TEST_CHECK(wd_video_encoder_nominal_fps(0) == WD_DEFAULT_SESSION_FPS);
    WD_TEST_CHECK(wd_video_encoder_nominal_fps(30) == 30);
    WD_TEST_CHECK(wd_video_encoder_nominal_fps(UINT16_MAX) == WD_MAX_SESSION_FPS);

    WD_TEST_CHECK(wd_video_encode_pacing_cap(60, 0, 0) == 60);
    WD_TEST_CHECK(wd_video_encode_pacing_cap(60, UINT64_C(77000000), 3) == 60);
    WD_TEST_CHECK(wd_video_encode_pacing_cap(60, UINT64_C(77000000), 4) == 11);
    WD_TEST_CHECK(wd_video_encode_pacing_cap(60, UINT64_C(20000000), 4) == 42);
    WD_TEST_CHECK(wd_video_encode_pacing_cap(60, UINT64_C(10000000), 4) == 60);
    WD_TEST_CHECK(wd_video_encode_pacing_cap(15, UINT64_C(20000000), 4) == 15);
    WD_TEST_CHECK(wd_video_encode_pacing_cap(60, UINT64_C(1000000000), 4) == 1);
    WD_TEST_CHECK(wd_video_encode_pacing_cap(3, UINT64_C(1000000000), 4) == 1);
    WD_TEST_CHECK(wd_video_encode_pacing_cap(0, UINT64_C(1000000000), 4) == 0);
    WD_TEST_CHECK(wd_video_encode_pacing_ewma(0, UINT64_C(700000000)) == UINT64_C(700000000));
    WD_TEST_CHECK(wd_video_encode_pacing_ewma(UINT64_C(80000000), UINT64_C(40000000)) == UINT64_C(70000000));
    WD_TEST_CHECK(wd_video_encode_pacing_ewma(UINT64_C(40000000), UINT64_C(80000000)) == UINT64_C(50000000));
    WD_TEST_CHECK(wd_video_encode_pacing_ewma(UINT64_MAX - 1, UINT64_MAX) <= UINT64_MAX);
    WD_TEST_CHECK(wd_video_encode_pacing_ewma(1234, 0) == 1234);
    return 0;
}
