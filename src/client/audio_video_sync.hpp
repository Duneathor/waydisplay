#pragma once

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"

#include <cstdint>

namespace waydisplay {

enum class ClientVideoAudioSyncDecision : uint8_t { Present, Hold, Drop };

struct ClientVideoAudioSyncPlan {
    ClientVideoAudioSyncDecision decision       = ClientVideoAudioSyncDecision::Present;
    int64_t                      delta_samples  = 0;
    uint32_t                     retry_after_ms = 0;
};

ClientVideoAudioSyncPlan     client_video_audio_sync_plan(uint64_t video_pts_usec, uint64_t audio_playhead_samples,
                                                          uint32_t sample_rate = WD_AUDIO_SAMPLE_RATE_DEFAULT);
ClientVideoAudioSyncDecision client_video_audio_sync_decide(uint64_t video_pts_usec, uint64_t audio_playhead_samples,
                                                            uint32_t sample_rate = WD_AUDIO_SAMPLE_RATE_DEFAULT);

} // namespace waydisplay
