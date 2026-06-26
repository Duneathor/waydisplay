#include "video_transition.h"

struct wd_client_video_transition_decision wd_client_video_transition_decide(enum wd_client_video_phase phase,
                                                                             bool content_epoch_advanced,
                                                                             bool end_of_stream, bool resize,
                                                                             bool keyframe, bool has_payload) {
    struct wd_client_video_transition_decision decision = {
        .next_phase     = WD_CLIENT_VIDEO_PHASE_TILES,
        .reset_decoder  = false,
        .accept_payload = false,
    };

    if (resize)
    {
        decision.next_phase    = WD_CLIENT_VIDEO_PHASE_AWAITING_KEYFRAME;
        decision.reset_decoder = phase == WD_CLIENT_VIDEO_PHASE_VIDEO;
        return decision;
    }
    if (end_of_stream)
    {
        decision.next_phase    = WD_CLIENT_VIDEO_PHASE_TILES;
        decision.reset_decoder = phase != WD_CLIENT_VIDEO_PHASE_TILES;
        return decision;
    }

    if (content_epoch_advanced)
    {
        decision.reset_decoder = phase != WD_CLIENT_VIDEO_PHASE_AWAITING_KEYFRAME;
        phase                  = WD_CLIENT_VIDEO_PHASE_AWAITING_KEYFRAME;
    }

    if (!has_payload)
    {
        decision.next_phase = phase;
        return decision;
    }

    if (phase != WD_CLIENT_VIDEO_PHASE_VIDEO && !keyframe)
    {
        decision.next_phase = WD_CLIENT_VIDEO_PHASE_AWAITING_KEYFRAME;
        return decision;
    }

    decision.next_phase     = WD_CLIENT_VIDEO_PHASE_VIDEO;
    decision.accept_payload = true;
    return decision;
}
