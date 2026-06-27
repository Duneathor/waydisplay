#include "audio_playback_clock.hpp"
#include "audio_video_sync.h"

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
    require(wd_client_audio_video_sync_decide(1000000, playhead, WD_AUDIO_SAMPLE_RATE_DEFAULT) == WD_CLIENT_AUDIO_VIDEO_SYNC_PRESENT,
            "a frame at the playhead should be presented");
    require(wd_client_audio_video_sync_decide(1050000, playhead, WD_AUDIO_SAMPLE_RATE_DEFAULT) ==
                WD_CLIENT_AUDIO_VIDEO_SYNC_HOLD,
            "an early frame should be held");
    require(wd_client_audio_video_sync_decide(900000, playhead, WD_AUDIO_SAMPLE_RATE_DEFAULT) ==
                WD_CLIENT_AUDIO_VIDEO_SYNC_DROP,
            "a late frame should be dropped");
    require(wd_client_audio_video_sync_decide(1000000, playhead, 0) == WD_CLIENT_AUDIO_VIDEO_SYNC_PRESENT,
            "zero sample rate should present without synchronization");

    const struct wd_client_audio_video_sync_plan held =
        wd_client_audio_video_sync_plan_compute(1050000, playhead, WD_AUDIO_SAMPLE_RATE_DEFAULT);
    require(held.decision == WD_CLIENT_AUDIO_VIDEO_SYNC_HOLD, "held plan should retain the hold decision");
    require(held.delta_samples == 2400, "held plan should report the sample lead");
    require(held.retry_after_ms == 10, "held plan should cap the immediate retry delay");

    const struct wd_client_audio_video_sync_plan due =
        wd_client_audio_video_sync_plan_compute(1040000, playhead, WD_AUDIO_SAMPLE_RATE_DEFAULT);
    require(due.decision == WD_CLIENT_AUDIO_VIDEO_SYNC_PRESENT, "a frame inside the lead tolerance should be presented");
    require(due.retry_after_ms == 0, "a due frame should not request a retry delay");

    const struct wd_client_audio_video_sync_plan enormous_lead = wd_client_audio_video_sync_plan_compute(UINT64_MAX, 0, 1);
    require(enormous_lead.decision == WD_CLIENT_AUDIO_VIDEO_SYNC_HOLD, "an enormous lead should be held without overflow");
    require(enormous_lead.retry_after_ms == WD_CLIENT_VIDEO_AUDIO_MAX_RETRY_MS, "an enormous lead should clamp the retry delay");

    const struct wd_client_audio_video_sync_plan enormous_lag = wd_client_audio_video_sync_plan_compute(0, UINT64_MAX, 1);
    require(enormous_lag.decision == WD_CLIENT_AUDIO_VIDEO_SYNC_DROP, "an enormous lag should be dropped without overflow");

    const uint64_t mixed = client_audio_frames_to_samples_fp(2880, 48000, 48000);
    require(client_audio_device_playhead(48000, 52800, mixed, 480) == 50400,
            "queued device audio should move the playhead behind mixed audio");
    require(client_audio_device_playhead(48000, 50000, mixed, 480) == 50000, "device playhead should not exceed played samples");
    require(client_audio_device_playhead(48000, 52800, 0, 480) == 48000, "an empty mix should preserve the base sample");
    require(!client_audio_device_consumed(48000, 52800, mixed, 480), "queued mixed audio should remain unconsumed");
    require(client_audio_device_consumed(48000, 50400, mixed, 480), "played mixed audio should be reported consumed");

    require(wd_client_audio_startup_gate_decide(false, false, true, 0, 1000) == WD_CLIENT_AUDIO_STARTUP_READY,
            "disabled audio must not hold video");
    require(wd_client_audio_startup_gate_decide(true, true, true, 0, 1000) == WD_CLIENT_AUDIO_STARTUP_READY,
            "playing audio must not hold video startup");
    require(wd_client_audio_startup_gate_decide(true, false, false, 0, 1000) == WD_CLIENT_AUDIO_STARTUP_READY,
            "an audio epoch that relinquished clock ownership must not hold video");
    require(wd_client_audio_startup_gate_decide(true, false, true, 999, 1000) == WD_CLIENT_AUDIO_STARTUP_HOLD,
            "audio may hold video while its bounded startup window remains open");
    require(wd_client_audio_startup_gate_decide(true, false, true, 1000, 1000) == WD_CLIENT_AUDIO_STARTUP_TIMEOUT,
            "audio startup must release video at the configured bound");
    return EXIT_SUCCESS;
}
