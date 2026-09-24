// Dependency-light regression tests for the server/client video control policy.
// These tests link the production policy implementations, not copied algorithms.
#include "wd_video_transition.h"
#include "video_decode_queue_policy.h"
#include "window_render_policy.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                            \
            std::exit(EXIT_FAILURE);                                                                                   \
        }                                                                                                              \
    } while (false)

namespace {

// A deliberately small harness: the inputs/outputs are those passed to the
// production transition helpers. It doesn't instantiate the Wayland server.
struct Recovery {
    bool keyframe_queued = false;
    uint64_t keyframe_id = 0;
    uint32_t wait_seconds = 0;
    uint32_t attempts = 1;
    uint64_t content_epoch = 10;
    uint64_t last_presented = 1800;
    bool recovering = true;

    void queue_keyframe(uint64_t id, bool first_video_entry = false) {
        const wd_video_entry_plan plan = wd_video_entry_plan_make(content_epoch, first_video_entry, true);
        // Recovery keyframes must not advance the client beyond the server's
        // committed epoch. The initial video-entry path is tested separately.
        CHECK(plan.frame_content_epoch == content_epoch);
        CHECK(!plan.commit_on_queue);
        const bool first = wd_video_recovery_track_keyframe(&keyframe_queued, &keyframe_id, &wait_seconds, id);
        if (first)
        {
            CHECK(keyframe_id == id);
        }
    }

    wd_video_recovery_action tick(uint32_t timeout = 3, uint32_t maximum_attempts = 2) {
        CHECK(recovering);
        CHECK(wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_AUTO, true, true, true));
        const wd_video_recovery_action action = wd_video_recovery_decide(
            keyframe_queued, keyframe_id, last_presented, wait_seconds, timeout, attempts, maximum_attempts);
        if (action == WD_VIDEO_RECOVERY_ACTION_WAIT)
        {
            ++wait_seconds;
        }
        else if (action == WD_VIDEO_RECOVERY_ACTION_RETRY_KEYFRAME)
        {
            ++attempts;
            wait_seconds = 0;
            keyframe_queued = false;
            keyframe_id = 0;
        }
        else if (action == WD_VIDEO_RECOVERY_ACTION_PRESENTED || action == WD_VIDEO_RECOVERY_ACTION_FALLBACK_TILES)
        {
            recovering = false;
        }
        return action;
    }
};

void test_window_focus_feedback_without_decoder_reset() {
    using namespace waydisplay;
    bool focused = true;
    focused = client_window_focus_after_event(focused, ClientWindowFocusChange::Lost);
    CHECK(!focused);
    CHECK(client_window_focus_after_event(focused, ClientWindowFocusChange::None) == focused);
    CHECK(client_window_feedback_flags(true, focused) == WD_CLIENT_STATS_RENDER_VISIBLE);
    CHECK(client_window_feedback_flags(false, focused) == 0);
    focused = client_window_focus_after_event(focused, ClientWindowFocusChange::Gained);
    CHECK(focused);
    CHECK(client_window_feedback_flags(true, focused) == WD_CLIENT_STATS_FLAG_MASK);
    CHECK(client_window_feedback_flags(false, focused) == 0);
    CHECK((WD_CLIENT_STATS_FLAG_MASK & ~((1u << 2) - 1u)) == 0);
    std::puts("PASS: focus events retain visibility and send independent stats flags");
}

void test_background_video_control() {
    wd_client_video_health_metrics metrics{};
    metrics.server_frames_tx = 60;
    metrics.client_reports = 1;
    metrics.client_frames_seen = 60;
    metrics.client_frames_decoded = 60;
    metrics.client_decode_queue_depth_max = 4;
    metrics.client_decode_queue_capacity = 4;
    // Focus loss does not imply an invisible window or a decoder reset.
    CHECK(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL);
    CHECK(wd_video_health_for_window(WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL, true, false) == WD_CLIENT_VIDEO_HEALTH_IDLE);
    CHECK(wd_video_health_for_window(WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL, false, true) == WD_CLIENT_VIDEO_HEALTH_IDLE);
    CHECK(wd_video_health_for_window(WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL, true, true) == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL);
    CHECK(!wd_video_cadence_window_pressure(true, false, false, true, false));
    CHECK(!wd_video_cadence_window_pressure(false, false, false, true, false));
    CHECK(wd_video_cadence_window_pressure(true, true, false, true, false));
    CHECK(wd_video_cadence_window_pressure(true, false, true, false, false));
    CHECK(!wd_video_cadence_window_can_upshift(true, false));
    CHECK(!wd_video_cadence_window_can_upshift(false, true));
    CHECK(wd_video_cadence_window_can_upshift(true, true));
    // Compressed drops invalidate references regardless of focus or visibility.
    metrics.client_decode_queue_drops = 1;
    CHECK(wd_video_health_for_window(wd_client_video_health_classify(&metrics), true, false) ==
          WD_CLIENT_VIDEO_HEALTH_DECODER_OVERLOADED);
    CHECK(wd_video_cadence_window_pressure(true, false, false, false, true));
    CHECK(wd_video_cadence_window_pressure(false, false, false, false, true));
    metrics.client_decode_queue_drops = 0;
    metrics.client_decode_failures = 1;
    CHECK(wd_video_health_for_window(wd_client_video_health_classify(&metrics), false, false) ==
          WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE);
    std::puts("PASS: background pacing is not decoder failure; actual reference loss still recovers");
}

void test_normal_entry_and_inplace_recovery_epochs() {
    // A first keyframe after tiles reserves the next epoch only on a successful queue.
    const wd_video_entry_plan entry = wd_video_entry_plan_make(10, true, true);
    CHECK(entry.frame_content_epoch == 11);
    CHECK(wd_video_entry_plan_can_commit(&entry, 10, true));
    CHECK(!wd_video_entry_plan_can_commit(&entry, 10, false));
    CHECK(!wd_video_entry_plan_can_commit(&entry, 11, true));
    CHECK(wd_video_entry_plan_make(10, true, false).frame_content_epoch == 10);

    // The first AND periodic keyframes in in-video recovery must stay in 10.
    Recovery recovery;
    recovery.queue_keyframe(1823);
    const uint64_t first = recovery.keyframe_id;
    for (uint64_t id = 1824; id <= 2784; id += 60)
    {
        recovery.queue_keyframe(id);
        CHECK(recovery.content_epoch == 10);
        CHECK(recovery.keyframe_id == first);
    }
    std::puts("PASS: entry advances epoch; in-video recovery does not");
}

void test_keyframes_cannot_keep_resetting_timeout() {
    Recovery recovery;
    recovery.queue_keyframe(1823);
    CHECK(recovery.tick() == WD_VIDEO_RECOVERY_ACTION_WAIT);
    CHECK(recovery.wait_seconds == 1);
    recovery.queue_keyframe(1884);
    CHECK(recovery.wait_seconds == 1);
    CHECK(recovery.tick() == WD_VIDEO_RECOVERY_ACTION_WAIT);
    recovery.queue_keyframe(1944);
    CHECK(recovery.wait_seconds == 2);
    CHECK(recovery.tick() == WD_VIDEO_RECOVERY_ACTION_WAIT);
    recovery.queue_keyframe(2004);
    CHECK(recovery.wait_seconds == 3);
    CHECK(recovery.tick() == WD_VIDEO_RECOVERY_ACTION_RETRY_KEYFRAME);
    CHECK(recovery.attempts == 2);
    CHECK(!recovery.keyframe_queued && recovery.keyframe_id == 0);

    // Retry has a new acknowledgement target, but subsequent GOP keyframes
    // may not overwrite that target or restart its timeout.
    recovery.queue_keyframe(2064);
    CHECK(recovery.keyframe_id == 2064);
    for (uint32_t i = 0; i < 3; ++i)
    {
        recovery.queue_keyframe(2124 + (60 * i));
        CHECK(recovery.tick() == WD_VIDEO_RECOVERY_ACTION_WAIT);
    }
    recovery.queue_keyframe(2304);
    CHECK(recovery.tick() == WD_VIDEO_RECOVERY_ACTION_FALLBACK_TILES);
    CHECK(!recovery.recovering);
    std::puts("PASS: repeating keyframes cannot prevent bounded retry/fallback");
}

void test_acknowledgement_completes_recovery() {
    Recovery recovery;
    recovery.queue_keyframe(1823);
    recovery.last_presented = 1822;
    CHECK(recovery.tick() == WD_VIDEO_RECOVERY_ACTION_WAIT);
    recovery.queue_keyframe(1884);
    CHECK(recovery.keyframe_id == 1823);
    recovery.last_presented = 1823;
    CHECK(recovery.tick() == WD_VIDEO_RECOVERY_ACTION_PRESENTED);
    CHECK(!recovery.recovering);
    std::puts("PASS: presenting tracked keyframe completes recovery");
}

void test_mode_override_and_reduced_cadence() {
    CHECK(wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_AUTO, true, true, true));
    CHECK(wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_FORCE, true, true, true));
    CHECK(!wd_video_auto_mode_wait_for_recovery(false, WD_VIDEO_MODE_AUTO, true, true, true));
    CHECK(!wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_OFF, true, true, true));
    CHECK(!wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_AUTO, false, true, true));
    CHECK(!wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_AUTO, true, false, true));
    CHECK(!wd_video_auto_mode_wait_for_recovery(true, WD_VIDEO_MODE_AUTO, true, true, false));
    CHECK(wd_video_failure_resume_fps(60, 21) == 21);
    CHECK(wd_video_failure_resume_fps(60, 0) == 60);
    CHECK(wd_video_failure_resume_fps(30, 45) == 30);
    std::puts("PASS: auto-mode cannot preempt recovery; disable is actionable; rate retained");
}

void test_queue_pressure_is_not_decoder_failure() {
    wd_client_video_health_metrics metrics{};
    metrics.server_frames_tx = 50;
    metrics.client_reports = 1;
    metrics.client_frames_presented = 50;
    metrics.client_decode_queue_depth_max = 4;
    metrics.client_decode_queue_capacity = 4;
    CHECK(wd_video_decode_queue_pressure(4, 4));
    CHECK(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_NORMAL);
    CHECK(wd_video_cadence_downshift_target(60, 60, 60, true, 5, 2, 75) == 45);

    const wd_client_video_decode_queue_plan plan = wd_client_video_decode_queue_plan_compute(4, 4, false, false, false);
    CHECK(plan.action == WD_CLIENT_VIDEO_DECODE_QUEUE_RECOVER_OVERFLOW);
    CHECK(plan.clear_queue && plan.wait_for_keyframe);
    metrics.client_decode_queue_drops = 1;
    CHECK(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_DECODER_OVERLOADED);
    const wd_client_video_decode_queue_plan after_overflow =
        wd_client_video_decode_queue_plan_compute(0, 4, true, false, false);
    CHECK(after_overflow.action == WD_CLIENT_VIDEO_DECODE_QUEUE_DROP_UNTIL_KEYFRAME);
    const wd_client_video_decode_queue_plan keyframe =
        wd_client_video_decode_queue_plan_compute(0, 4, true, true, false);
    CHECK(keyframe.action == WD_CLIENT_VIDEO_DECODE_QUEUE_ENQUEUE);
    CHECK(!keyframe.wait_for_keyframe && keyframe.reset_decoder_before);
    std::puts("PASS: high-water mark downshifts; actual overflow requests recovery");
}

void test_recovery_helper_rejects_invalid_keyframes() {
    bool queued = false;
    uint64_t frame = 0;
    uint32_t wait = 2;
    CHECK(!wd_video_recovery_track_keyframe(nullptr, &frame, &wait, 42));
    CHECK(!wd_video_recovery_track_keyframe(&queued, nullptr, &wait, 42));
    CHECK(!wd_video_recovery_track_keyframe(&queued, &frame, nullptr, 42));
    CHECK(!wd_video_recovery_track_keyframe(&queued, &frame, &wait, 0));
    CHECK(!queued && frame == 0 && wait == 2);
    std::puts("PASS: invalid keyframe tracking does not mutate state");
}

} // namespace

int main() {
    test_window_focus_feedback_without_decoder_reset();
    test_background_video_control();
    test_normal_entry_and_inplace_recovery_epochs();
    test_keyframes_cannot_keep_resetting_timeout();
    test_acknowledgement_completes_recovery();
    test_mode_override_and_reduced_cadence();
    test_queue_pressure_is_not_decoder_failure();
    test_recovery_helper_rejects_invalid_keyframes();
    std::puts("PASS: all video control policy regressions");
    return EXIT_SUCCESS;
}
