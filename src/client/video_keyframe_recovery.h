#ifndef WAYDISPLAY_CLIENT_VIDEO_KEYFRAME_RECOVERY_H
#define WAYDISPLAY_CLIENT_VIDEO_KEYFRAME_RECOVERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_client_video_keyframe_result {
    WD_CLIENT_VIDEO_KEYFRAME_VALID = 0,
    WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM,
    WD_CLIENT_VIDEO_KEYFRAME_MISSING_PARAMETER_SETS,
    WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS,
};

/* The decoder is reset on overload. A recovery keyframe must therefore be a
 * self-contained Annex-B access unit, not merely a packet flagged KEYFRAME.
 * It must include parameter sets and an independently decodable picture.
 * This intentionally does not inspect every steady-state inter-frame packet. */
enum wd_client_video_keyframe_result wd_client_video_keyframe_validate(uint32_t codec, const uint8_t* data, uint32_t size);
const char* wd_client_video_keyframe_result_name(enum wd_client_video_keyframe_result result);

#ifdef __cplusplus
}
#endif

#endif
