#pragma once
#include <cstdint>
namespace waydisplay {
enum class ClientVideoAudioSyncDecision : uint8_t { Present, Hold, Drop };
ClientVideoAudioSyncDecision client_video_audio_sync_decide(uint64_t video_pts_usec,
                                                            uint64_t audio_playhead_samples,
                                                            uint32_t sample_rate = 48000);
}
