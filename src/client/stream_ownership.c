#include "stream_ownership.h"

static uint64_t wd_client_stream_ownership_pack(uint64_t epoch, enum wd_client_content_owner owner) {
    return (epoch << 1u) | (uint64_t)owner;
}

void wd_client_stream_ownership_init(struct wd_client_stream_ownership* ownership) {
    if (ownership)
    {
        __atomic_store_n(&ownership->packed_state, WD_CLIENT_STREAM_OWNERSHIP_INITIAL_STATE, __ATOMIC_RELAXED);
    }
}

struct wd_client_content_ownership_snapshot
wd_client_stream_ownership_snapshot(const struct wd_client_stream_ownership* ownership) {
    struct wd_client_content_ownership_snapshot result = {
        .epoch = 0,
        .owner = WD_CLIENT_CONTENT_OWNER_TILES,
    };
    if (!ownership)
    {
        return result;
    }

    const uint64_t packed = __atomic_load_n(&ownership->packed_state, __ATOMIC_ACQUIRE);
    result.epoch          = packed >> 1u;
    result.owner          = (enum wd_client_content_owner)(packed & 1u);
    return result;
}

static uint64_t wd_client_stream_ownership_transition_to(struct wd_client_stream_ownership* ownership,
                                                         enum wd_client_content_owner owner) {
    uint64_t current = __atomic_load_n(&ownership->packed_state, __ATOMIC_ACQUIRE);
    for (;;)
    {
        if ((enum wd_client_content_owner)(current & 1u) == owner)
        {
            return current >> 1u;
        }
        const uint64_t next_epoch = (current >> 1u) + 1u;
        const uint64_t next       = wd_client_stream_ownership_pack(next_epoch, owner);
        if (__atomic_compare_exchange_n(&ownership->packed_state, &current, next, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            return next_epoch;
        }
    }
}

static uint64_t wd_client_stream_ownership_advance(struct wd_client_stream_ownership* ownership,
                                                   enum wd_client_content_owner owner) {
    uint64_t current = __atomic_load_n(&ownership->packed_state, __ATOMIC_RELAXED);
    for (;;)
    {
        const uint64_t next_epoch = (current >> 1u) + 1u;
        const uint64_t next       = wd_client_stream_ownership_pack(next_epoch, owner);
        if (__atomic_compare_exchange_n(&ownership->packed_state, &current, next, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        {
            return next_epoch;
        }
    }
}

uint64_t wd_client_stream_ownership_begin_video_stream(struct wd_client_stream_ownership* ownership) {
    return wd_client_stream_ownership_transition_to(ownership, WD_CLIENT_CONTENT_OWNER_VIDEO);
}

uint64_t wd_client_stream_ownership_end_video_stream(struct wd_client_stream_ownership* ownership) {
    return wd_client_stream_ownership_transition_to(ownership, WD_CLIENT_CONTENT_OWNER_TILES);
}

uint64_t wd_client_stream_ownership_reset_to_video(struct wd_client_stream_ownership* ownership) {
    return wd_client_stream_ownership_advance(ownership, WD_CLIENT_CONTENT_OWNER_VIDEO);
}

uint64_t wd_client_stream_ownership_reset_to_tiles(struct wd_client_stream_ownership* ownership) {
    return wd_client_stream_ownership_advance(ownership, WD_CLIENT_CONTENT_OWNER_TILES);
}

bool wd_client_stream_ownership_is_current(const struct wd_client_stream_ownership* ownership, uint64_t epoch,
                                           enum wd_client_content_owner owner) {
    const struct wd_client_content_ownership_snapshot current = wd_client_stream_ownership_snapshot(ownership);
    return current.epoch == epoch && current.owner == owner;
}
