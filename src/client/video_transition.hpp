#pragma once

#include <cstdint>

namespace waydisplay {

enum class ClientVideoPhase : uint8_t {
    Tiles = 0,
    AwaitingKeyframe,
    Video,
};

struct ClientVideoTransitionDecision {
    ClientVideoPhase next_phase     = ClientVideoPhase::Tiles;
    bool             reset_decoder  = false;
    bool             accept_payload = false;
};

ClientVideoTransitionDecision client_video_transition(ClientVideoPhase phase, bool content_epoch_advanced, bool end_of_stream, bool resize,
                                                      bool keyframe, bool has_payload);

} // namespace waydisplay
