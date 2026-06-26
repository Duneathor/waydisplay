#include "audio_video_sync.h"
#include "render_planning.hpp"
#include "video_present_queue.hpp"
#include "waydisplay/wd_protocol.h"
#include "wd_tile_policy.h"
#include "wd_video_transition.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

namespace {

using namespace waydisplay;

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

ClientVideoFrameBuffer make_frame(uint8_t fill) {
    ClientVideoFrameBuffer frame{};
    frame.format   = ClientVideoPixelFormat::IYUV;
    frame.width    = 4;
    frame.height   = 4;
    frame.y_pitch  = 4;
    frame.uv_pitch = 2;
    frame.u_offset = 16;
    frame.v_offset = 20;
    frame.bytes.assign(24, fill);
    return frame;
}

void test_stable_geometry_reconnect_accepts_cached_tiles() {
    ClientDirtyTileGrid grid;
    require(configure_client_dirty_tile_grid(grid, 1422, 773, 16, 16), "initial reconnect configuration should allocate the dirty grid");
    require(grid.mark_rect({1408, 768, 14, 5}), "the final partial tile from a cached refresh should be accepted");
    require(grid.dirty_tile_count() == 1, "a cached edge tile should produce one pending base tile");

    std::vector<ClientDirtyRect> rects;
    grid.take_rects(rects);
    require(rects.size() == 1, "the cached edge tile should be renderable");
    require(rects.front().x == 1408 && rects.front().y == 768 && rects.front().w == 14 && rects.front().h == 5,
            "the cached edge tile should remain clipped to the stable framebuffer");
}

void test_audio_held_video_survives_decode_and_recovery_cycle() {
    ClientVideoPresentQueue queue(3);
    require(queue.push_decoded(make_frame(1), 4, 4, 1, 1050000, 9), "first decoded frame should enter the presentation queue");
    require(queue.push_decoded(make_frame(2), 4, 4, 2, 1070000, 9), "second decoded frame should enter the presentation queue");
    require(queue.push_decoded(make_frame(3), 4, 4, 3, 1090000, 9), "third decoded frame should enter the presentation queue");

    const ClientQueuedVideoFrame* head = queue.front();
    require(head && head->frame_id == 1, "the oldest frame should be the presentation head");
    const struct wd_client_audio_video_sync_plan held = wd_client_audio_video_sync_plan_compute(head->pts_usec, 48000, 48000);
    require(held.decision == WD_CLIENT_AUDIO_VIDEO_SYNC_HOLD, "a frame 50 ms ahead of audio should wait");
    require(held.retry_after_ms == 10, "the renderer should wake when the frame crosses the 40 ms early threshold");

    bool                   dropped_tail  = false;
    ClientVideoFrameBuffer decode_buffer = queue.take_decode_buffer(dropped_tail);
    require(dropped_tail, "a full presentation queue should recycle its newest tail");
    require(queue.front() && queue.front()->frame_id == 1, "continued decoding must not overwrite the audio-held head");
    decode_buffer.bytes.assign(decode_buffer.bytes.size(), 4);
    require(queue.push_decoded(std::move(decode_buffer), 4, 4, 4, 1110000, 9),
            "the newest decoded frame should replace only the discarded tail");

    wd_client_video_health_metrics health{};
    health.server_frames_tx        = 4;
    health.client_reports          = 1;
    health.client_frames_seen      = 4;
    health.client_frames_decoded   = 4;
    health.client_audio_video_sync_holds = 1;
    health.client_queue_depth      = static_cast<uint32_t>(queue.size());
    require(wd_client_video_health_classify(&health) == WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT,
            "queued frames waiting for audio must not be classified as decoder failure");

    require(wd_tile_recovery_decide(false, 0, 1, 5) == WD_TILE_RECOVERY_WAIT, "tile recovery should wait until its refresh has drained");
    require(!wd_video_entry_allowed(true, 0), "forced video entry must remain blocked during tile recovery");
    require(wd_tile_recovery_decide(true, 0, 4, 5) == WD_TILE_RECOVERY_WAIT,
            "tile recovery should remain sticky while awaiting client presentation");
    require(wd_tile_recovery_decide(true, 1, 4, 5) == WD_TILE_RECOVERY_COMPLETE_PRESENTED,
            "a client tile presentation should complete recovery");
    require(!wd_video_entry_allowed(false, 1), "video should remain blocked during its post-recovery cooldown");
    require(wd_video_entry_allowed(false, 0), "video may resume after recovery and cooldown complete");

    const struct wd_client_audio_video_sync_plan due = wd_client_audio_video_sync_plan_compute(queue.front()->pts_usec, 48480, 48000);
    require(due.decision == WD_CLIENT_AUDIO_VIDEO_SYNC_PRESENT,
            "the preserved head should become presentable when audio reaches its deadline");
    const ClientQueuedVideoFrame presented = queue.pop_front();
    require(presented.frame_id == 1, "the frame that waited for audio should be presented rather than overwritten");
}

void test_bootstrap_refresh_does_not_trigger_video_selection() {
    wd_video_auto_entry_metrics metrics{};
    metrics.frame_samples                 = 60;
    metrics.changed_frame_samples         = 60;
    metrics.dirty_coverage_per_mille_sum  = 60000;
    metrics.dirty_coverage_per_mille_peak = 1000;
    metrics.tile_wire_bytes               = 10u * 1024u * 1024u;
    metrics.tile_budget_bytes_per_second  = 10u * 1024u * 1024u;
    metrics.requested_capture_fps         = 60;
    metrics.adaptive_capture_fps          = 60;
    metrics.minimum_dirty_percent         = 60;

    require(wd_video_auto_entry_evaluate(&metrics).candidate, "the same sustained scene cost should normally select video mode");
    metrics.selection_suppressed = true;
    require(!wd_video_auto_entry_evaluate(&metrics).candidate, "cached reconnect and recovery refresh traffic must not select video mode");
}

void test_reconnect_telemetry_survives_protocol_copy() {
    wd_client_stats_payload sent{};
    sent.video_frames_decoded      = 23;
    sent.audio_video_sync_holds    = 17;
    sent.video_queue_depth         = 3;
    sent.video_queue_depth_max     = 6;
    sent.video_oldest_pts_usec     = 1050000;
    sent.audio_video_delta_samples = 2400;
    sent.tile_frames_presented     = 1;

    wd_client_stats_payload received{};
    std::memcpy(&received, &sent, sizeof(received));
    require(received.video_frames_decoded == sent.video_frames_decoded && received.audio_video_sync_holds == sent.audio_video_sync_holds &&
                received.video_queue_depth == sent.video_queue_depth && received.video_queue_depth_max == sent.video_queue_depth_max &&
                received.video_oldest_pts_usec == sent.video_oldest_pts_usec &&
                received.audio_video_delta_samples == sent.audio_video_delta_samples &&
                received.tile_frames_presented == sent.tile_frames_presented,
            "reconnect health and recovery telemetry should survive the wire payload");
}

} // namespace

int main() {
    test_stable_geometry_reconnect_accepts_cached_tiles();
    test_audio_held_video_survives_decode_and_recovery_cycle();
    test_bootstrap_refresh_does_not_trigger_video_selection();
    test_reconnect_telemetry_survives_protocol_copy();
    return 0;
}
