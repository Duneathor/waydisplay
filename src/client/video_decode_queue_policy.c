#include "video_decode_queue_policy.h"

struct wd_client_video_decode_queue_plan wd_client_video_decode_queue_plan_compute(
    uint32_t queued_packets, uint32_t capacity, bool waiting_for_keyframe, bool keyframe, bool control_frame) {
    struct wd_client_video_decode_queue_plan plan = {
        .action = WD_CLIENT_VIDEO_DECODE_QUEUE_ENQUEUE,
        .clear_queue = false,
        .wait_for_keyframe = waiting_for_keyframe,
        .reset_decoder_before = false,
    };

    if (control_frame)
    {
        /* End-of-stream/resize always invalidates decoder references, even
         * if an invalid mixed-flags packet also claims to be a keyframe. */
        plan.clear_queue       = true;
        plan.wait_for_keyframe = true;
        return plan;
    }
    if (keyframe)
    {
        plan.clear_queue          = true;
        plan.wait_for_keyframe    = false;
        plan.reset_decoder_before = waiting_for_keyframe;
        return plan;
    }

    if (waiting_for_keyframe)
    {
        plan.action = WD_CLIENT_VIDEO_DECODE_QUEUE_DROP_UNTIL_KEYFRAME;
        return plan;
    }

    if (capacity == 0 || queued_packets >= capacity)
    {
        plan.action            = WD_CLIENT_VIDEO_DECODE_QUEUE_RECOVER_OVERFLOW;
        plan.clear_queue       = true;
        plan.wait_for_keyframe = true;
    }
    return plan;
}
