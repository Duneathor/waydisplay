#ifndef WAYDISPLAY_CLIENT_VIDEO_DECODE_QUEUE_POLICY_H
#define WAYDISPLAY_CLIENT_VIDEO_DECODE_QUEUE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_client_video_decode_queue_action {
    WD_CLIENT_VIDEO_DECODE_QUEUE_ENQUEUE = 0,
    WD_CLIENT_VIDEO_DECODE_QUEUE_DROP_UNTIL_KEYFRAME = 1,
    WD_CLIENT_VIDEO_DECODE_QUEUE_RECOVER_OVERFLOW = 2,
};

struct wd_client_video_decode_queue_plan {
    enum wd_client_video_decode_queue_action action;
    bool                                     clear_queue;
    bool                                     wait_for_keyframe;
    bool                                     reset_decoder_before;
};

struct wd_client_video_decode_queue_plan wd_client_video_decode_queue_plan_compute(
    uint32_t queued_packets, uint32_t capacity, bool waiting_for_keyframe, bool keyframe, bool control_frame);

#ifdef __cplusplus
}
#endif

#endif
