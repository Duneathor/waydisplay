#pragma once

#include "waydisplay/wd_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_client_audio_video_sync_decision {
    WD_CLIENT_AUDIO_VIDEO_SYNC_PRESENT = 0,
    WD_CLIENT_AUDIO_VIDEO_SYNC_HOLD,
    WD_CLIENT_AUDIO_VIDEO_SYNC_DROP,
};

struct wd_client_audio_video_sync_plan {
    enum wd_client_audio_video_sync_decision decision;
    int64_t                                  delta_samples;
    uint32_t                                 retry_after_ms;
};

struct wd_client_audio_video_sync_plan wd_client_audio_video_sync_plan_compute(uint64_t video_pts_usec,
                                                                               uint64_t audio_playhead_samples,
                                                                               uint32_t sample_rate);

enum wd_client_audio_video_sync_decision wd_client_audio_video_sync_decide(uint64_t video_pts_usec,
                                                                            uint64_t audio_playhead_samples,
                                                                            uint32_t sample_rate);

#ifdef __cplusplus
}
#endif
