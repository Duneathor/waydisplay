#include "wd_dirty_region_scheduler.h"
#include "wd_video_transition.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

void test_tile_recovery_completion_matrix() {
    require(wd_tile_recovery_decide(false, 7, 7, 99, 1) == WD_TILE_RECOVERY_WAIT, "recovery cannot complete before refresh transmission");
    require(wd_tile_recovery_decide(true, 7, 7, 0, 10) == WD_TILE_RECOVERY_COMPLETE_PRESENTED,
            "first presented tile frame should complete recovery");
    require(wd_tile_recovery_decide(true, 7, 6, 9, 10) == WD_TILE_RECOVERY_WAIT, "recovery should wait below timeout without presentation");
    require(wd_tile_recovery_decide(true, 7, 6, 0, 10) == WD_TILE_RECOVERY_WAIT,
            "a presentation from the prior content epoch must not complete recovery");
    require(!wd_video_entry_allowed(true, false, 0, true, WD_VIDEO_RECOVERY_NONE),
            "forced video must wait for the initial tile bootstrap presentation");
    require(!wd_video_entry_allowed(false, false, 1, true, WD_VIDEO_RECOVERY_FAILURE),
            "forced video must respect the circuit breaker after a pipeline failure");
    require(wd_tile_recovery_decide(true, 7, 6, 10, 10) == WD_TILE_RECOVERY_COMPLETE_TIMEOUT,
            "recovery should complete at the timeout boundary");
    require(wd_tile_recovery_decide(true, 7, 6, UINT32_MAX, 0) == WD_TILE_RECOVERY_WAIT, "zero timeout should disable timeout completion");

    require(wd_video_entry_allowed(false, false, 0, false, WD_VIDEO_RECOVERY_NONE), "video entry should be allowed when recovery and cooldown are clear");
    require(!wd_video_entry_allowed(false, true, 0, true, WD_VIDEO_RECOVERY_PLANNED), "active tile recovery should block video entry");
    require(!wd_video_entry_allowed(false, false, 1, false, WD_VIDEO_RECOVERY_PLANNED), "retry cooldown should block automatic video entry");
    require(wd_video_entry_allowed(false, false, 1, true, WD_VIDEO_RECOVERY_PLANNED), "forced video should bypass the post-recovery retry cooldown");
    require(!wd_video_entry_allowed(false, true, 1, true, WD_VIDEO_RECOVERY_PLANNED), "active tile recovery should block even forced video entry");
}

void test_video_epoch_plan_commit_guards() {
    require(wd_next_nonzero_epoch(0) == 1, "zero epoch should advance to one");
    require(wd_next_nonzero_epoch(UINT64_MAX) == 1, "epoch wrap must skip zero");

    wd_video_entry_plan plan = wd_video_entry_plan_make(9, false, true);
    require(plan.source_content_epoch == 9 && plan.frame_content_epoch == 9 && !plan.commit_on_queue,
            "keyframe outside first-frame wait should not reserve a new epoch");
    require(!wd_video_entry_plan_can_commit(&plan, 9, true), "non-reserving plan cannot commit");

    plan = wd_video_entry_plan_make(9, true, false);
    require(!plan.commit_on_queue && plan.frame_content_epoch == 9, "inter-frame cannot reserve the video entry epoch");

    plan = wd_video_entry_plan_make(9, true, true);
    require(plan.commit_on_queue && plan.frame_content_epoch == 10, "first keyframe should reserve the next epoch");
    require(!wd_video_entry_plan_can_commit(nullptr, 9, true), "null plan cannot commit");
    require(!wd_video_entry_plan_can_commit(&plan, 9, false), "failed queue cannot commit");
    require(!wd_video_entry_plan_can_commit(&plan, 10, true), "stale source epoch cannot commit");
    wd_video_entry_plan corrupt = plan;
    corrupt.frame_content_epoch = 12;
    require(!wd_video_entry_plan_can_commit(&corrupt, 9, true), "noncanonical reserved epoch cannot commit");
    require(wd_video_entry_plan_can_commit(&plan, 9, true), "canonical queued entry keyframe should commit");
}

void test_client_video_health_precedence() {
    require(wd_client_video_health_classify(nullptr) == WD_CLIENT_VIDEO_HEALTH_IDLE, "missing metrics should be idle");

    wd_client_video_health_metrics metrics{};
    metrics.server_frames_tx = 1;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_IDLE, "health requires at least one client report");
    metrics.client_reports = 1;

    metrics.client_decode_failures  = 1;
    metrics.client_frames_presented = 1;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE,
            "decoder failure should take precedence over presentation");
    metrics.client_decode_failures  = 0;
    metrics.client_publish_failures = 1;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE,
            "publish failure should classify as decoder pipeline failure");
    metrics.client_publish_failures    = 0;
    metrics.client_need_keyframe_drops = 1;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_AWAITING_KEYFRAME,
            "keyframe dependency drops should classify as awaiting-keyframe recovery");

    metrics.client_need_keyframe_drops = 0;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_NORMAL,
            "successful presentation should classify as normal");

    metrics.client_frames_presented = 0;
    metrics.client_frames_decoded   = 2;
    metrics.client_audio_video_sync_holds = 3;
    metrics.client_audio_playback_state = WD_CLIENT_AUDIO_PLAYBACK_BUFFERING;
    metrics.client_audio_video_startup_hold_ms = 500;
    metrics.client_audio_video_sync_hold_current_ms = 500;
    metrics.client_queue_depth      = 1;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT,
            "decoded queued frames held by audio should not be a pipeline failure");

    metrics.client_queue_depth     = 0;
    metrics.client_queue_depth_max = 3;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT,
            "an interval queue peak should preserve audio-wait classification after the queue drains");

    metrics.client_audio_playback_state = WD_CLIENT_AUDIO_PLAYBACK_PLAYING;
    metrics.client_audio_video_sync_hold_current_ms = 1500;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT,
            "a bounded playing-state audio hold should not be classified as a video stall");
    metrics.client_audio_video_sync_hold_current_ms = WD_CLIENT_AUDIO_VIDEO_PLAYING_HOLD_MAX_MS + 1;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL,
            "an excessive playing-state hold must stop concealing a video stall");
    metrics.client_audio_playback_state = WD_CLIENT_AUDIO_PLAYBACK_BUFFERING;

    metrics.client_audio_video_startup_timeouts = 1;
    metrics.client_audio_video_sync_hold_current_ms = 0;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL,
            "an expired startup gate with no active hold must expose a video presentation stall");

    metrics.client_queue_depth_max = 0;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL,
            "audio holds without current or interval queued frames should not explain a presentation stall");
    metrics.client_frames_decoded = 0;
    metrics.client_frames_seen    = 1;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL,
            "received but unpresented video should classify as a pipeline stall");
    metrics.client_frames_seen = 0;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_IDLE,
            "no observed client video activity should remain idle");

    metrics.client_decode_queue_capacity = 4;
    metrics.client_decode_queue_depth_max = 4;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_DECODER_OVERLOADED,
            "a saturated compressed decode queue should request recovery before a drop is reported");
    metrics.client_decode_queue_depth_max = 0;
    metrics.client_decode_queue_drops = 1;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_DECODER_OVERLOADED,
            "compressed decode queue drops should request in-place overload recovery");
    require(wd_video_safe_decode_fps(34000000ull, 30, 85) == 25,
            "34ms software decode should cap a 30fps request near 25fps with headroom");
    require(wd_video_safe_decode_fps(10000000ull, 30, 85) == 30,
            "fast decode should retain the requested ceiling");

    require(std::strcmp(wd_client_video_health_name(WD_CLIENT_VIDEO_HEALTH_IDLE), "idle") == 0 &&
                std::strcmp(wd_client_video_health_name(WD_CLIENT_VIDEO_HEALTH_NORMAL), "normal") == 0 &&
                std::strcmp(wd_client_video_health_name(WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT), "audio-wait") == 0 &&
                std::strcmp(wd_client_video_health_name(WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL), "pipeline-stall") == 0 &&
                std::strcmp(wd_client_video_health_name(WD_CLIENT_VIDEO_HEALTH_DECODER_OVERLOADED), "decoder-overloaded") == 0 &&
                std::strcmp(wd_client_video_health_name(WD_CLIENT_VIDEO_HEALTH_AWAITING_KEYFRAME), "awaiting-keyframe") == 0 &&
                std::strcmp(wd_client_video_health_name(WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE), "hard-failure") == 0 &&
                std::strcmp(wd_client_video_health_name(static_cast<wd_client_video_health_class>(99)), "unknown") == 0,
            "health names should cover every public class and unknown values");
}


void test_connection_mode_transition_matrix() {
    require(!wd_video_entry_allowed(true, false, 0, true, WD_VIDEO_RECOVERY_NONE),
            "forced video must wait for the exact bootstrap presentation");
    require(!wd_video_entry_allowed(false, true, 0, true, WD_VIDEO_RECOVERY_PLANNED),
            "forced video must not interrupt an active planned recovery");
    require(wd_video_entry_allowed(false, false, 5, true, WD_VIDEO_RECOVERY_PLANNED),
            "forced video may resume immediately after an acknowledged planned resize recovery");
    require(!wd_video_entry_allowed(false, false, 5, true, WD_VIDEO_RECOVERY_FAILURE),
            "forced video must respect the circuit breaker after a real pipeline failure");
    require(wd_video_entry_allowed(false, false, 0, true, WD_VIDEO_RECOVERY_FAILURE),
            "forced video may retry after the failure cooldown expires");
    require(wd_tile_recovery_decide(true, 42, 41, 4, 5) == WD_TILE_RECOVERY_WAIT,
            "a stale presentation cannot complete recovery");
    require(wd_tile_recovery_decide(true, 42, 42, 4, 5) == WD_TILE_RECOVERY_COMPLETE_PRESENTED,
            "the exact recovery epoch completes recovery");
}

void test_scheduler_argument_and_round_robin_edges() {
    require(wd_dirty_region_scheduler_create(0, 1, 0) == nullptr, "zero scheduler capacity should be rejected");
    require(wd_dirty_region_scheduler_create(1, 0, 0) == nullptr, "zero region width should be rejected");
    wd_dirty_region_scheduler_destroy(nullptr);
    wd_dirty_region_scheduler_reset(nullptr);
    require(!wd_dirty_region_scheduler_enqueue(nullptr, 0, 1), "null scheduler enqueue should fail");
    require(!wd_dirty_region_scheduler_take(nullptr, 0, 1, nullptr), "null scheduler take should fail");
    require(!wd_dirty_region_scheduler_contains(nullptr, 0) && wd_dirty_region_scheduler_count(nullptr) == 0 &&
                wd_dirty_region_scheduler_enqueued_ns(nullptr, 0) == 0,
            "null scheduler queries should be safe");

    wd_dirty_region_scheduler* scheduler = wd_dirty_region_scheduler_create(130, 10, 0);
    require(scheduler != nullptr, "round-robin scheduler should be created");
    require(!wd_dirty_region_scheduler_enqueue(scheduler, 130, 1), "out-of-range enqueue should fail");
    uint16_t selected = 0;
    require(!wd_dirty_region_scheduler_take(scheduler, 0, 1, nullptr), "null output pointer should fail");
    require(!wd_dirty_region_scheduler_take(scheduler, 0, 1, &selected), "empty scheduler should not select work");

    require(wd_dirty_region_scheduler_enqueue(scheduler, 0, 0), "zero timestamp enqueue should succeed");
    require(wd_dirty_region_scheduler_enqueued_ns(scheduler, 0) == 1, "zero enqueue timestamp should normalize to a nonzero sentinel");
    require(wd_dirty_region_scheduler_enqueue(scheduler, 0, 999) && wd_dirty_region_scheduler_count(scheduler) == 1 &&
                wd_dirty_region_scheduler_enqueued_ns(scheduler, 0) == 1,
            "duplicate enqueue should preserve count and original age");
    require(wd_dirty_region_scheduler_enqueue(scheduler, 64, 2), "enqueue second bitset word");
    require(wd_dirty_region_scheduler_enqueue(scheduler, 129, 3), "enqueue partial final bitset word");
    require(wd_dirty_region_scheduler_take(scheduler, 63, 4, &selected) && selected == 64,
            "round-robin scan should start after the cursor across bitset words");
    require(wd_dirty_region_scheduler_take(scheduler, 129, 4, &selected) && selected == 0, "round-robin scan should wrap at capacity");
    require(wd_dirty_region_scheduler_take(scheduler, 0, 4, &selected) && selected == 129,
            "remaining final-word region should be selected");

    wd_dirty_region_scheduler_forget(scheduler, 130);
    require(!wd_dirty_region_scheduler_contains(scheduler, 130) && wd_dirty_region_scheduler_enqueued_ns(scheduler, 130) == 0,
            "out-of-range forget/query should be harmless");
    wd_dirty_region_scheduler_destroy(scheduler);
}

void test_scheduler_starvation_ties_forget_and_heap_pruning() {
    wd_dirty_region_scheduler* scheduler = wd_dirty_region_scheduler_create(16, 4, 100);
    require(scheduler != nullptr, "starvation scheduler should be created");
    require(wd_dirty_region_scheduler_enqueue(scheduler, 5, 10), "enqueue tie candidate five");
    require(wd_dirty_region_scheduler_enqueue(scheduler, 3, 10), "enqueue tie candidate three");
    uint16_t selected = 0;
    require(wd_dirty_region_scheduler_take(scheduler, 4, 110, &selected) && selected == 3,
            "equal-age starvation tie should choose the lower region ID");

    require(wd_dirty_region_scheduler_contains(scheduler, 5), "unselected candidate should remain queued");
    wd_dirty_region_scheduler_forget(scheduler, 5);
    require(wd_dirty_region_scheduler_count(scheduler) == 0 && !wd_dirty_region_scheduler_contains(scheduler, 5) &&
                wd_dirty_region_scheduler_enqueued_ns(scheduler, 5) == 0,
            "forget should remove queued work and its preserved age");

    require(wd_dirty_region_scheduler_enqueue(scheduler, 7, 20), "enqueue stale heap candidate");
    wd_dirty_region_scheduler_forget(scheduler, 7);
    require(wd_dirty_region_scheduler_enqueue(scheduler, 8, 30), "enqueue current heap candidate");
    require(wd_dirty_region_scheduler_take(scheduler, 0, 1000, &selected) && selected == 8,
            "heap pruning should skip forgotten stale entries");

    wd_dirty_region_scheduler_reset(scheduler);
    require(wd_dirty_region_scheduler_count(scheduler) == 0, "reset should leave scheduler empty after stale heap activity");
    wd_dirty_region_scheduler_destroy(scheduler);
}

} // namespace

int main() {
    test_tile_recovery_completion_matrix();
    test_video_epoch_plan_commit_guards();
    test_client_video_health_precedence();
    test_connection_mode_transition_matrix();
    test_scheduler_argument_and_round_robin_edges();
    test_scheduler_starvation_ties_forget_and_heap_pruning();
    return 0;
}
