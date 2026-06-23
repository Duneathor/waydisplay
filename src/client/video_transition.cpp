#include "video_transition.hpp"

namespace waydisplay {

ClientVideoTransitionDecision client_video_transition(ClientVideoPhase phase, bool content_epoch_advanced, bool end_of_stream, bool resize,
                                                      bool keyframe, bool has_payload) {
    ClientVideoTransitionDecision decision{};

    if (resize)
    {
        decision.next_phase    = ClientVideoPhase::AwaitingKeyframe;
        decision.reset_decoder = phase == ClientVideoPhase::Video;
        return decision;
    }
    if (end_of_stream)
    {
        decision.next_phase    = ClientVideoPhase::Tiles;
        decision.reset_decoder = phase != ClientVideoPhase::Tiles;
        return decision;
    }

    if (content_epoch_advanced)
    {
        decision.reset_decoder = phase != ClientVideoPhase::AwaitingKeyframe;
        phase                  = ClientVideoPhase::AwaitingKeyframe;
    }

    if (!has_payload)
    {
        decision.next_phase = phase;
        return decision;
    }

    if (phase != ClientVideoPhase::Video && !keyframe)
    {
        decision.next_phase = ClientVideoPhase::AwaitingKeyframe;
        return decision;
    }

    decision.next_phase     = ClientVideoPhase::Video;
    decision.accept_payload = true;
    return decision;
}

} // namespace waydisplay
