#include "audio_video_sync.h"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_media_clock.h"

#include <limits.h>

static uint64_t wd_min_u64(uint64_t left, uint64_t right) {
    return left < right ? left : right;
}

struct wd_client_audio_video_sync_plan wd_client_audio_video_sync_plan_compute(uint64_t video_pts_usec,
                                                                               uint64_t audio_playhead_samples,
                                                                               uint32_t sample_rate) {
    struct wd_client_audio_video_sync_plan plan = {
        .decision       = WD_CLIENT_AUDIO_VIDEO_SYNC_PRESENT,
        .delta_samples  = 0,
        .retry_after_ms = 0,
    };
    if (sample_rate == 0)
    {
        return plan;
    }

    const uint64_t video_samples = wd_media_usec_to_samples(video_pts_usec, sample_rate);
    const uint64_t early         = (uint64_t)sample_rate * WD_CLIENT_VIDEO_AUDIO_EARLY_MS / WD_MSEC_PER_SEC;
    const uint64_t late          = (uint64_t)sample_rate * WD_CLIENT_VIDEO_AUDIO_LATE_MS / WD_MSEC_PER_SEC;
    const uint64_t positive_delta =
        video_samples >= audio_playhead_samples ? video_samples - audio_playhead_samples : audio_playhead_samples - video_samples;
    const uint64_t clamped_delta = wd_min_u64(positive_delta, (uint64_t)INT64_MAX);
    plan.delta_samples = video_samples >= audio_playhead_samples ? (int64_t)clamped_delta : -(int64_t)clamped_delta;

    if (video_samples > audio_playhead_samples && video_samples - audio_playhead_samples > early)
    {
        plan.decision                    = WD_CLIENT_AUDIO_VIDEO_SYNC_HOLD;
        const uint64_t wait_samples      = video_samples - audio_playhead_samples - early;
        const uint64_t whole_seconds     = wait_samples / sample_rate;
        const uint64_t remainder_samples = wait_samples % sample_rate;
        const uint64_t whole_ms =
            whole_seconds > UINT32_MAX / WD_MSEC_PER_SEC ? UINT32_MAX : whole_seconds * WD_MSEC_PER_SEC;
        const uint64_t remainder_ms = (remainder_samples * WD_MSEC_PER_SEC + sample_rate - 1u) / sample_rate;
        const uint64_t wait_ms      = wd_min_u64(WD_CLIENT_VIDEO_AUDIO_MAX_RETRY_MS, whole_ms + remainder_ms);
        plan.retry_after_ms         = (uint32_t)(wait_ms == 0 ? 1 : wait_ms);
    }
    else if (audio_playhead_samples > video_samples && audio_playhead_samples - video_samples > late)
    {
        plan.decision = WD_CLIENT_AUDIO_VIDEO_SYNC_DROP;
    }
    return plan;
}

enum wd_client_audio_video_sync_decision wd_client_audio_video_sync_decide(uint64_t video_pts_usec,
                                                                            uint64_t audio_playhead_samples,
                                                                            uint32_t sample_rate) {
    return wd_client_audio_video_sync_plan_compute(video_pts_usec, audio_playhead_samples, sample_rate).decision;
}


enum wd_client_audio_startup_gate_decision wd_client_audio_startup_gate_decide(bool configured, bool playing, bool waiting,
                                                                                uint64_t wait_elapsed_ms,
                                                                                uint32_t max_wait_ms) {
    if (!configured || playing || !waiting)
    {
        return WD_CLIENT_AUDIO_STARTUP_READY;
    }
    if (max_wait_ms != 0 && wait_elapsed_ms >= max_wait_ms)
    {
        return WD_CLIENT_AUDIO_STARTUP_TIMEOUT;
    }
    return WD_CLIENT_AUDIO_STARTUP_HOLD;
}
