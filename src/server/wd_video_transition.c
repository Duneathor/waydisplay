#include "wd_video_transition.h"

uint64_t wd_next_nonzero_epoch(uint64_t current_epoch) {
    current_epoch++;
    return current_epoch == 0 ? 1 : current_epoch;
}

struct wd_video_entry_plan wd_video_entry_plan_make(uint64_t current_epoch,
                                                     bool waiting_for_first_keyframe,
                                                     bool frame_is_keyframe) {
    struct wd_video_entry_plan plan = {
        .source_content_epoch = current_epoch,
        .frame_content_epoch = current_epoch,
        .commit_on_queue = false,
    };
    if (waiting_for_first_keyframe && frame_is_keyframe)
    {
        plan.frame_content_epoch = wd_next_nonzero_epoch(current_epoch);
        plan.commit_on_queue = true;
    }
    return plan;
}

bool wd_video_entry_plan_can_commit(const struct wd_video_entry_plan* plan,
                                    uint64_t current_epoch, bool queue_succeeded) {
    return plan && plan->commit_on_queue && queue_succeeded &&
           plan->source_content_epoch == current_epoch &&
           plan->frame_content_epoch == wd_next_nonzero_epoch(current_epoch);
}
