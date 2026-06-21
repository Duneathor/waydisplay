#include "stream_ownership.hpp"

namespace waydisplay {

ClientContentOwnershipSnapshot ClientStreamOwnership::snapshot() const {
    const uint64_t packed = state_.load(std::memory_order_acquire);
    ClientContentOwnershipSnapshot result{};
    result.epoch = packed >> 1u;
    result.owner = static_cast<ClientContentOwner>(packed & 1u);
    return result;
}

uint64_t ClientStreamOwnership::transition_to(ClientContentOwner owner) {
    uint64_t current = state_.load(std::memory_order_acquire);
    for (;;)
    {
        if (static_cast<ClientContentOwner>(current & 1u) == owner)
        {
            return current >> 1u;
        }
        const uint64_t next_epoch = (current >> 1u) + 1u;
        const uint64_t next = pack(next_epoch, owner);
        if (state_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                         std::memory_order_acquire))
        {
            return next_epoch;
        }
    }
}

uint64_t ClientStreamOwnership::advance(ClientContentOwner owner) {
    uint64_t current = state_.load(std::memory_order_relaxed);
    for (;;)
    {
        const uint64_t next_epoch = (current >> 1u) + 1u;
        const uint64_t next = pack(next_epoch, owner);
        if (state_.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return next_epoch;
        }
    }
}

uint64_t ClientStreamOwnership::begin_video_stream() {
    return transition_to(ClientContentOwner::Video);
}

uint64_t ClientStreamOwnership::end_video_stream() {
    return transition_to(ClientContentOwner::Tiles);
}

uint64_t ClientStreamOwnership::reset_to_video() {
    return advance(ClientContentOwner::Video);
}

uint64_t ClientStreamOwnership::reset_to_tiles() {
    return advance(ClientContentOwner::Tiles);
}

bool ClientStreamOwnership::is_current(uint64_t epoch, ClientContentOwner owner) const {
    const ClientContentOwnershipSnapshot current = snapshot();
    return current.epoch == epoch && current.owner == owner;
}

} // namespace waydisplay
