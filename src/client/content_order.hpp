#pragma once

#include "stream_ownership.h"

#include <cstdint>

namespace waydisplay {

struct ClientState;

enum class ClientContentEpochDecision : uint8_t {
    Stale = 0,
    Current,
    Advanced,
};

ClientContentEpochDecision client_accept_content_epoch(ClientState& state, uint64_t content_epoch, enum wd_client_content_owner owner);
void                       client_reset_content_epoch(ClientState& state, uint64_t content_epoch, enum wd_client_content_owner owner);

} // namespace waydisplay
