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

    /* An encoder slower than the audio clock can supply one perpetually late
     * picture at a time. Always show that picture rather than dropping every
     * frame and falsely reporting a dead video pipeline. */
    require(!wd_client_audio_video_sync_should_drop(WD_CLIENT_AUDIO_VIDEO_SYNC_DROP, 0),
            "no decoded frame cannot be dropped");
    require(!wd_client_audio_video_sync_should_drop(WD_CLIENT_AUDIO_VIDEO_SYNC_DROP, 1),
            "the sole decoded frame must be presented even when audio is ahead");
    require(wd_client_audio_video_sync_should_drop(WD_CLIENT_AUDIO_VIDEO_SYNC_DROP, 2),
            "a late frame may be dropped if a fresher decoded frame is queued");
    require(wd_client_audio_video_sync_should_drop(WD_CLIENT_AUDIO_VIDEO_SYNC_DROP, 3),
            "catch-up can discard older frames while retaining a newest frame");
    require(!wd_client_audio_video_sync_should_drop(WD_CLIENT_AUDIO_VIDEO_SYNC_PRESENT, 2),
            "an on-time frame must not be dropped");
    require(!wd_client_audio_video_sync_should_drop(WD_CLIENT_AUDIO_VIDEO_SYNC_HOLD, 2),
            "a held frame must not be dropped by the late-frame policy");

    /* Audio can drift behind the sender over time even when a 20 ms startup
     * threshold was met.  A running playback FIFO must not grow unbounded. */
    const uint64_t max_audio_queue = client_audio_max_queued_samples(48000, 20);
    require(max_audio_queue == 5760, "default audio queue bound should be 120 ms");
    require(client_audio_max_queued_samples(48000, 100) == 19200,
            "explicitly larger latency targets need correspondingly larger bounds");
    require(!client_audio_output_rebase_needed(false, 100000, 960, max_audio_queue),
            "startup buffering must not be discarded before playback starts");
    require(!client_audio_output_rebase_needed(true, 4800, 960, max_audio_queue),
            "a queue exactly at the bound must remain intact");
    require(client_audio_output_rebase_needed(true, 4801, 960, max_audio_queue),
            "an accumulating playback queue must be rebased");
    require(client_audio_output_rebase_needed(true, max_audio_queue + 1, 960, max_audio_queue),
            "an already excessive queue must be rebased without unsigned underflow");
    require(!client_audio_output_rebase_needed(true, max_audio_queue, 0, max_audio_queue),
            "an empty output packet must not trigger a rebase");
    require(!client_audio_output_rebase_needed(true, 100, 20, 0),
            "zero capacity disables output rebasing");

    /* Simulate long-running audio transport with a slightly slower device.
     * The startup target alone cannot bound this drift: the queue grows by
     * ten samples each packet.  An output-only rebase discards stale PCM
     * without changing the next wire PTS or the Opus decoder's continuity. */
    uint64_t output_queue = 960;
    uint64_t packet_pts = 48000;
    uint64_t rebases = 0;
    for (int packet = 0; packet < 1400; ++packet)
    {
        if (client_audio_output_rebase_needed(true, output_queue, 960, max_audio_queue))
        {
            output_queue = 0;
            ++rebases;
        }
        output_queue += 960;
        packet_pts += 960;
        output_queue -= 950;
        require(output_queue <= max_audio_queue, "continuous output must remain within its queue latency bound");
    }
    require(rebases > 0, "a slower output device must eventually trigger an audio-only rebase");
    require(packet_pts == 48000 + 1400 * 960, "an output-only rebase must preserve the sender's packet timeline");

    /* Equal device and wire rates must not produce needless audio skips. */
    output_queue = 960;
    for (int packet = 0; packet < 1400; ++packet)
    {
        require(!client_audio_output_rebase_needed(true, output_queue, 960, max_audio_queue),
                "stable audio must not rebase");
        output_queue += 960;
        output_queue -= 960;
    }

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
