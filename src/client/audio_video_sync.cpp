#include "audio_video_sync.hpp"
#include "waydisplay/wd_media_clock.h"
namespace waydisplay {
ClientVideoAudioSyncDecision client_video_audio_sync_decide(uint64_t video_pts_usec,
                                                            uint64_t audio_playhead_samples,
                                                            uint32_t sample_rate) {
    if (sample_rate == 0) return ClientVideoAudioSyncDecision::Present;
    const uint64_t video_samples = wd_media_usec_to_samples(video_pts_usec, sample_rate);
    const uint64_t early = static_cast<uint64_t>(sample_rate) * 40u / 1000u;
    const uint64_t late = static_cast<uint64_t>(sample_rate) * 80u / 1000u;
    if (video_samples > audio_playhead_samples + early) return ClientVideoAudioSyncDecision::Hold;
    if (video_samples + late < audio_playhead_samples) return ClientVideoAudioSyncDecision::Drop;
    return ClientVideoAudioSyncDecision::Present;
}
}
