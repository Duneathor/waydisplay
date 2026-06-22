#pragma once

#include <cstdint>

namespace waydisplay {

enum class ClientVideoAudioSyncDecision : uint8_t { Present, Hold, Drop };

constexpr uint32_t CLIENT_VIDEO_AUDIO_SYNC_MAX_RETRY_MS = 20;

struct ClientVideoAudioSyncPlan {
    ClientVideoAudioSyncDecision decision = ClientVideoAudioSyncDecision::Present;
    int64_t delta_samples = 0;
    uint32_t retry_after_ms = 0;
};

ClientVideoAudioSyncPlan client_video_audio_sync_plan(uint64_t video_pts_usec,
                                                      uint64_t audio_playhead_samples,
                                                      uint32_t sample_rate = 48000);
ClientVideoAudioSyncDecision client_video_audio_sync_decide(uint64_t video_pts_usec,
                                                            uint64_t audio_playhead_samples,
                                                            uint32_t sample_rate = 48000);

} // namespace waydisplay
