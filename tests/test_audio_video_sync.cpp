#include "audio_playback_clock.hpp"
#include "audio_video_sync.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    const uint64_t playhead = 48000;
    require(client_video_audio_sync_decide(1000000, playhead) ==
                ClientVideoAudioSyncDecision::Present,
            "a frame at the playhead should be presented");
    require(client_video_audio_sync_decide(1050000, playhead) ==
                ClientVideoAudioSyncDecision::Hold,
            "an early frame should be held");
    require(client_video_audio_sync_decide(900000, playhead) ==
                ClientVideoAudioSyncDecision::Drop,
            "a late frame should be dropped");
    require(client_video_audio_sync_decide(1000000, playhead, 0) ==
                ClientVideoAudioSyncDecision::Present,
            "zero sample rate should present without synchronization");

    const ClientVideoAudioSyncPlan held = client_video_audio_sync_plan(1050000, playhead);
    require(held.decision == ClientVideoAudioSyncDecision::Hold,
            "held plan should retain the hold decision");
    require(held.delta_samples == 2400, "held plan should report the sample lead");
    require(held.retry_after_ms == 10, "held plan should cap the immediate retry delay");

    const ClientVideoAudioSyncPlan due = client_video_audio_sync_plan(1040000, playhead);
    require(due.decision == ClientVideoAudioSyncDecision::Present,
            "a frame inside the lead tolerance should be presented");
    require(due.retry_after_ms == 0, "a due frame should not request a retry delay");

    const ClientVideoAudioSyncPlan enormous_lead =
        client_video_audio_sync_plan(UINT64_MAX, 0, 1);
    require(enormous_lead.decision == ClientVideoAudioSyncDecision::Hold,
            "an enormous lead should be held without overflow");
    require(enormous_lead.retry_after_ms == WD_CLIENT_VIDEO_AUDIO_MAX_RETRY_MS,
            "an enormous lead should clamp the retry delay");

    const ClientVideoAudioSyncPlan enormous_lag =
        client_video_audio_sync_plan(0, UINT64_MAX, 1);
    require(enormous_lag.decision == ClientVideoAudioSyncDecision::Drop,
            "an enormous lag should be dropped without overflow");

    const uint64_t mixed = client_audio_frames_to_samples_fp(2880, 48000, 48000);
    require(client_audio_device_playhead(48000, 52800, mixed, 480) == 50400,
            "queued device audio should move the playhead behind mixed audio");
    require(client_audio_device_playhead(48000, 50000, mixed, 480) == 50000,
            "device playhead should not exceed played samples");
    require(client_audio_device_playhead(48000, 52800, 0, 480) == 48000,
            "an empty mix should preserve the base sample");
    require(!client_audio_device_consumed(48000, 52800, mixed, 480),
            "queued mixed audio should remain unconsumed");
    require(client_audio_device_consumed(48000, 50400, mixed, 480),
            "played mixed audio should be reported consumed");
    return EXIT_SUCCESS;
}
