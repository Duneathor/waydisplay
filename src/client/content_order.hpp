#pragma once

#include "stream_ownership.hpp"

#include <cstdint>

namespace waydisplay {

struct ClientState;

enum class ClientContentEpochDecision : uint8_t {
    Stale = 0,
    Current,
    Advanced,
};

ClientContentEpochDecision client_accept_content_epoch(ClientState& state, uint64_t content_epoch,
                                                       ClientContentOwner owner);
void client_reset_content_epoch(ClientState& state, uint64_t content_epoch, ClientContentOwner owner);

} // namespace waydisplay
