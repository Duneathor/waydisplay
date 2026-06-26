#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_client_video_phase {
    WD_CLIENT_VIDEO_PHASE_TILES = 0,
    WD_CLIENT_VIDEO_PHASE_AWAITING_KEYFRAME,
    WD_CLIENT_VIDEO_PHASE_VIDEO,
};

struct wd_client_video_transition_decision {
    enum wd_client_video_phase next_phase;
    bool                       reset_decoder;
    bool                       accept_payload;
};

struct wd_client_video_transition_decision wd_client_video_transition_decide(enum wd_client_video_phase phase,
                                                                             bool content_epoch_advanced,
                                                                             bool end_of_stream, bool resize,
                                                                             bool keyframe, bool has_payload);

#ifdef __cplusplus
}
#endif
