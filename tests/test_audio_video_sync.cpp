#include "audio_playback_clock.hpp"
#include "audio_video_sync.hpp"
#include <cassert>
using namespace waydisplay;
int main() {
    const uint64_t playhead = 48000;
    assert(client_video_audio_sync_decide(1000000, playhead) == ClientVideoAudioSyncDecision::Present);
    assert(client_video_audio_sync_decide(1050000, playhead) == ClientVideoAudioSyncDecision::Hold);
    assert(client_video_audio_sync_decide(900000, playhead) == ClientVideoAudioSyncDecision::Drop);
    assert(client_video_audio_sync_decide(1000000, playhead, 0) == ClientVideoAudioSyncDecision::Present);
    const ClientVideoAudioSyncPlan held = client_video_audio_sync_plan(1050000, playhead);
    assert(held.decision == ClientVideoAudioSyncDecision::Hold);
    assert(held.delta_samples == 2400);
    assert(held.retry_after_ms == 10);
    const ClientVideoAudioSyncPlan due = client_video_audio_sync_plan(1040000, playhead);
    assert(due.decision == ClientVideoAudioSyncDecision::Present);
    assert(due.retry_after_ms == 0);

    const ClientVideoAudioSyncPlan enormous_lead =
        client_video_audio_sync_plan(UINT64_MAX, 0, 1);
    assert(enormous_lead.decision == ClientVideoAudioSyncDecision::Hold);
    assert(enormous_lead.retry_after_ms == CLIENT_VIDEO_AUDIO_SYNC_MAX_RETRY_MS);
    const ClientVideoAudioSyncPlan enormous_lag =
        client_video_audio_sync_plan(0, UINT64_MAX, 1);
    assert(enormous_lag.decision == ClientVideoAudioSyncDecision::Drop);

    const uint64_t mixed = client_audio_frames_to_samples_fp(
        2880, 48000, 48000);
    assert(client_audio_device_playhead(48000, 52800, mixed, 480) == 50400);
    assert(client_audio_device_playhead(48000, 50000, mixed, 480) == 50000);
    assert(client_audio_device_playhead(48000, 52800, 0, 480) == 48000);
    assert(!client_audio_device_consumed(48000, 52800, mixed, 480));
    assert(client_audio_device_consumed(48000, 50400, mixed, 480));
    return 0;
}
