#include "../src/server/video_vaapi_quality_policy.h"
#include "test_check.h"
int main(void) {
    WD_TEST_CHECK(wd_video_hevc_vaapi_prefer_quality(WD_VIDEO_CODEC_H265, WD_VIDEO_DERIVED_BITRATE_MAX_KIB_PER_SECOND));
    WD_TEST_CHECK(wd_video_hevc_vaapi_prefer_quality(WD_VIDEO_CODEC_H265, WD_VIDEO_DERIVED_BITRATE_MAX_KIB_PER_SECOND + 1));
    WD_TEST_CHECK(!wd_video_hevc_vaapi_prefer_quality(WD_VIDEO_CODEC_H265, WD_VIDEO_DERIVED_BITRATE_MAX_KIB_PER_SECOND - 1));
    WD_TEST_CHECK(!wd_video_hevc_vaapi_prefer_quality(WD_VIDEO_CODEC_H264, WD_VIDEO_DERIVED_BITRATE_MAX_KIB_PER_SECOND));
    WD_TEST_CHECK(!wd_video_hevc_vaapi_prefer_quality(WD_VIDEO_CODEC_AV1, WD_VIDEO_DERIVED_BITRATE_MAX_KIB_PER_SECOND));
    return 0;
}
