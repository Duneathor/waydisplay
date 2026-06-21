#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_video_entry_plan {
    uint64_t source_content_epoch;
    uint64_t frame_content_epoch;
    bool     commit_on_queue;
};

uint64_t wd_next_nonzero_epoch(uint64_t current_epoch);
struct wd_video_entry_plan wd_video_entry_plan_make(uint64_t current_epoch,
                                                     bool waiting_for_first_keyframe,
                                                     bool frame_is_keyframe);
bool wd_video_entry_plan_can_commit(const struct wd_video_entry_plan* plan,
                                    uint64_t current_epoch, bool queue_succeeded);

#ifdef __cplusplus
}
#endif
