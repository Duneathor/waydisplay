#include "audio_video_sync.h"
#include "video_present_queue.hpp"
#include "waydisplay/wd_protocol.h"
#include "wd_video_transition.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                    \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

namespace {

waydisplay::ClientVideoFrameBuffer frame(uint8_t value) {
    waydisplay::ClientVideoFrameBuffer result{};
    result.format = waydisplay::ClientVideoPixelFormat::IYUV;
    result.width = 4;
    result.height = 4;
    result.y_pitch = 4;
    result.uv_pitch = 2;
    result.u_offset = 16;
    result.v_offset = 20;
    result.bytes.assign(24, value);
    return result;
}

void test_audio_never_arrives_releases_video() {
    waydisplay::ClientVideoPresentQueue queue(3);
    CHECK(queue.push_decoded(frame(1), 4, 4, 1, 100000, 7));
    CHECK(queue.push_decoded(frame(2), 4, 4, 2, 133333, 7));
    CHECK(queue.push_decoded(frame(3), 4, 4, 3, 166666, 7));
    CHECK(queue.front() && queue.front()->frame_id == 1);

    CHECK(wd_client_audio_startup_gate_decide(true, false, true, 999, 1000) == WD_CLIENT_AUDIO_STARTUP_HOLD);
    CHECK(queue.front() && queue.front()->frame_id == 1);
    CHECK(wd_client_audio_startup_gate_decide(true, false, true, 1000, 1000) == WD_CLIENT_AUDIO_STARTUP_TIMEOUT);
    CHECK(queue.pop_front().frame_id == 1);
    CHECK(queue.front() && queue.front()->frame_id == 2);
}

void test_late_audio_can_become_clock_master() {
    const auto before_audio = wd_client_audio_startup_gate_decide(true, false, true, 1000, 1000);
    CHECK(before_audio == WD_CLIENT_AUDIO_STARTUP_TIMEOUT);
    const auto after_audio = wd_client_audio_startup_gate_decide(true, true, false, 1500, 1000);
    CHECK(after_audio == WD_CLIENT_AUDIO_STARTUP_READY);

    const auto early = wd_client_audio_video_sync_plan_compute(1000000, 45000, 48000);
    CHECK(early.decision == WD_CLIENT_AUDIO_VIDEO_SYNC_HOLD);
    const auto due = wd_client_audio_video_sync_plan_compute(1000000, 48000, 48000);
    CHECK(due.decision == WD_CLIENT_AUDIO_VIDEO_SYNC_PRESENT);
    const auto late = wd_client_audio_video_sync_plan_compute(1000000, 52000, 48000);
    CHECK(late.decision == WD_CLIENT_AUDIO_VIDEO_SYNC_DROP);
}

void test_queue_pressure_preserves_audio_held_head() {
    waydisplay::ClientVideoPresentQueue queue(2);
    CHECK(queue.push_decoded(frame(1), 4, 4, 10, 1000000, 9));
    CHECK(queue.push_decoded(frame(2), 4, 4, 11, 1033333, 9));
    bool dropped_tail = false;
    auto recycled = queue.take_decode_buffer(dropped_tail);
    CHECK(dropped_tail);
    CHECK(queue.front() && queue.front()->frame_id == 10);
    recycled.bytes.assign(24, 3);
    recycled.format = waydisplay::ClientVideoPixelFormat::IYUV;
    recycled.width = 4;
    recycled.height = 4;
    recycled.y_pitch = 4;
    recycled.uv_pitch = 2;
    recycled.u_offset = 16;
    recycled.v_offset = 20;
    CHECK(queue.push_decoded(std::move(recycled), 4, 4, 12, 1066666, 9));
    CHECK(queue.front() && queue.front()->frame_id == 10);
}

void test_server_health_distinguishes_bounded_wait_from_timeout() {
    wd_client_video_health_metrics metrics{};
    metrics.server_frames_tx = 4;
    metrics.client_reports = 1;
    metrics.client_frames_seen = 4;
    metrics.client_frames_decoded = 4;
    metrics.client_audio_video_sync_holds = 4;
    metrics.client_audio_playback_state = WD_CLIENT_AUDIO_PLAYBACK_BUFFERING;
    metrics.client_audio_video_startup_hold_ms = 500;
    metrics.client_audio_video_sync_hold_current_ms = 500;
    metrics.client_queue_depth_max = 3;
    CHECK(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT);

    metrics.client_audio_video_startup_timeouts = 1;
    metrics.client_audio_video_startup_hold_ms = 1000;
    metrics.client_audio_video_sync_hold_current_ms = 0;
    CHECK(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL);

    metrics.client_audio_video_startup_timeouts = 0;
    metrics.client_decode_queue_drops = 1;
    CHECK(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_DECODER_OVERLOADED);
}

} // namespace

int main() {
    test_audio_never_arrives_releases_video();
    test_late_audio_can_become_clock_master();
    test_queue_pressure_preserves_audio_held_head();
    test_server_health_distinguishes_bounded_wait_from_timeout();
    return 0;
}
