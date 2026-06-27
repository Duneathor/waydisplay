#include "video_decode_queue_policy.h"
#include "wd_video_transition.h"
#include "waydisplay/wd_config.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

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

void test_scrub_overflow_requests_keyframe_without_tile_handoff() {
    constexpr uint32_t capacity = WD_CLIENT_VIDEO_DECODE_INPUT_QUEUE_CAPACITY;
    bool waiting = false;
    uint32_t depth = 0;

    for (uint32_t i = 0; i < capacity; ++i)
    {
        const auto plan = wd_client_video_decode_queue_plan_compute(depth, capacity, waiting, false, false);
        CHECK(plan.action == WD_CLIENT_VIDEO_DECODE_QUEUE_ENQUEUE);
        CHECK(!plan.clear_queue);
        ++depth;
    }

    const auto overflow = wd_client_video_decode_queue_plan_compute(depth, capacity, waiting, false, false);
    CHECK(overflow.action == WD_CLIENT_VIDEO_DECODE_QUEUE_RECOVER_OVERFLOW);
    CHECK(overflow.clear_queue);
    CHECK(overflow.wait_for_keyframe);
    depth = 0;
    waiting = overflow.wait_for_keyframe;

    const auto dependent = wd_client_video_decode_queue_plan_compute(depth, capacity, waiting, false, false);
    CHECK(dependent.action == WD_CLIENT_VIDEO_DECODE_QUEUE_DROP_UNTIL_KEYFRAME);
    CHECK(!dependent.clear_queue);

    const auto recovery_keyframe = wd_client_video_decode_queue_plan_compute(depth, capacity, waiting, true, false);
    CHECK(recovery_keyframe.action == WD_CLIENT_VIDEO_DECODE_QUEUE_ENQUEUE);
    CHECK(recovery_keyframe.clear_queue);
    CHECK(recovery_keyframe.reset_decoder_before);
    CHECK(!recovery_keyframe.wait_for_keyframe);

    CHECK(wd_video_recovery_decide(false, 0, 0, 0, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_WAIT);
    CHECK(wd_video_recovery_decide(true, 200, 199, 1, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_WAIT);
    CHECK(wd_video_recovery_decide(true, 200, 200, 1, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_PRESENTED);
}

void test_scrub_pressure_reduces_video_cadence() {
    CHECK(wd_video_safe_decode_fps(34000000ull, 30, WD_STREAM_VIDEO_DECODE_HEADROOM_PERCENT) == 25);
    CHECK(wd_video_safe_decode_fps(48000000ull, 30, WD_STREAM_VIDEO_DECODE_HEADROOM_PERCENT) == 17);
    CHECK(wd_video_safe_decode_fps(10000000ull, 30, WD_STREAM_VIDEO_DECODE_HEADROOM_PERCENT) == 30);
}

void test_failed_recovery_is_bounded() {
    CHECK(wd_video_recovery_decide(true, 300, 299, 3, 3, 1, 2) == WD_VIDEO_RECOVERY_ACTION_RETRY_KEYFRAME);
    CHECK(wd_video_recovery_decide(true, 301, 299, 3, 3, 2, 2) == WD_VIDEO_RECOVERY_ACTION_FALLBACK_TILES);
}

void test_mixed_failure_causes_do_not_share_a_streak() {
    wd_video_health_streak streak{};
    streak = wd_video_health_streak_update(streak, WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL);
    CHECK(streak.health == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL && streak.seconds == 1);
    streak = wd_video_health_streak_update(streak, WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE);
    CHECK(streak.health == WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE && streak.seconds == 1);
    streak = wd_video_health_streak_update(streak, WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL);
    CHECK(streak.health == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL && streak.seconds == 1);
    streak = wd_video_health_streak_update(streak, WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT);
    CHECK(streak.health == WD_CLIENT_VIDEO_HEALTH_IDLE && streak.seconds == 0);
}

void test_feedback_actions_keep_transient_overload_in_video() {
    CHECK(wd_video_feedback_action_classify(0) == WD_VIDEO_FEEDBACK_ACTION_NONE);
    CHECK(wd_video_feedback_action_classify(WD_VIDEO_FEEDBACK_AUDIO_SYNC_HOLD) == WD_VIDEO_FEEDBACK_ACTION_NONE);
    CHECK(wd_video_feedback_action_classify(WD_VIDEO_FEEDBACK_PRESENTATION_STALL) == WD_VIDEO_FEEDBACK_ACTION_NONE);
    CHECK(wd_video_feedback_action_classify(WD_VIDEO_FEEDBACK_NEEDS_KEYFRAME) ==
          WD_VIDEO_FEEDBACK_ACTION_RECOVER_IN_VIDEO);
    CHECK(wd_video_feedback_action_classify(WD_VIDEO_FEEDBACK_DECODE_OVERLOAD | WD_VIDEO_FEEDBACK_NEEDS_KEYFRAME) ==
          WD_VIDEO_FEEDBACK_ACTION_RECOVER_IN_VIDEO);
    CHECK(wd_video_feedback_action_classify(WD_VIDEO_FEEDBACK_DECODE_FAILURE) ==
          WD_VIDEO_FEEDBACK_ACTION_FALLBACK_TILES);
    CHECK(wd_video_feedback_action_classify(WD_VIDEO_FEEDBACK_PUBLISH_FAILURE | WD_VIDEO_FEEDBACK_NEEDS_KEYFRAME) ==
          WD_VIDEO_FEEDBACK_ACTION_FALLBACK_TILES);
}

void test_control_and_keyframe_queue_semantics() {
    const auto control = wd_client_video_decode_queue_plan_compute(3, 4, false, false, true);
    CHECK(control.action == WD_CLIENT_VIDEO_DECODE_QUEUE_ENQUEUE);
    CHECK(control.clear_queue);
    CHECK(control.wait_for_keyframe);
    CHECK(!control.reset_decoder_before);

    const auto first_keyframe = wd_client_video_decode_queue_plan_compute(0, 4, true, true, false);
    CHECK(first_keyframe.action == WD_CLIENT_VIDEO_DECODE_QUEUE_ENQUEUE);
    CHECK(first_keyframe.clear_queue);
    CHECK(!first_keyframe.wait_for_keyframe);
    CHECK(first_keyframe.reset_decoder_before);
}

} // namespace

int main() {
    test_scrub_overflow_requests_keyframe_without_tile_handoff();
    test_scrub_pressure_reduces_video_cadence();
    test_failed_recovery_is_bounded();
    test_mixed_failure_causes_do_not_share_a_streak();
    test_feedback_actions_keep_transient_overload_in_video();
    test_control_and_keyframe_queue_semantics();
    return 0;
}
