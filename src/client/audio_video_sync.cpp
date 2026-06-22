#include "audio_video_sync.hpp"
#include "waydisplay/wd_media_clock.h"

#include <algorithm>
#include <limits>

namespace waydisplay {

ClientVideoAudioSyncPlan client_video_audio_sync_plan(uint64_t video_pts_usec,
                                                      uint64_t audio_playhead_samples,
                                                      uint32_t sample_rate) {
    ClientVideoAudioSyncPlan plan{};
    if (sample_rate == 0)
    {
        return plan;
    }

    const uint64_t video_samples = wd_media_usec_to_samples(video_pts_usec, sample_rate);
    const uint64_t early = static_cast<uint64_t>(sample_rate) * 40u / 1000u;
    const uint64_t late = static_cast<uint64_t>(sample_rate) * 80u / 1000u;
    const uint64_t positive_delta = video_samples >= audio_playhead_samples
                                        ? video_samples - audio_playhead_samples
                                        : audio_playhead_samples - video_samples;
    const uint64_t clamped_delta = std::min<uint64_t>(
        positive_delta, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    plan.delta_samples = video_samples >= audio_playhead_samples
                             ? static_cast<int64_t>(clamped_delta)
                             : -static_cast<int64_t>(clamped_delta);

    if (video_samples > audio_playhead_samples &&
        video_samples - audio_playhead_samples > early)
    {
        plan.decision = ClientVideoAudioSyncDecision::Hold;
        const uint64_t wait_samples =
            video_samples - audio_playhead_samples - early;
        const uint64_t whole_seconds = wait_samples / sample_rate;
        const uint64_t remainder_samples = wait_samples % sample_rate;
        const uint64_t whole_ms = whole_seconds > UINT32_MAX / 1000u
                                      ? UINT32_MAX
                                      : whole_seconds * 1000u;
        const uint64_t remainder_ms =
            (remainder_samples * 1000u + sample_rate - 1u) / sample_rate;
        const uint64_t wait_ms = std::min<uint64_t>(
            CLIENT_VIDEO_AUDIO_SYNC_MAX_RETRY_MS, whole_ms + remainder_ms);
        plan.retry_after_ms = static_cast<uint32_t>(wait_ms == 0 ? 1 : wait_ms);
    }
    else if (audio_playhead_samples > video_samples &&
             audio_playhead_samples - video_samples > late)
    {
        plan.decision = ClientVideoAudioSyncDecision::Drop;
    }
    return plan;
}

ClientVideoAudioSyncDecision client_video_audio_sync_decide(uint64_t video_pts_usec,
                                                            uint64_t audio_playhead_samples,
                                                            uint32_t sample_rate) {
    return client_video_audio_sync_plan(video_pts_usec, audio_playhead_samples,
                                        sample_rate)
        .decision;
}

} // namespace waydisplay
