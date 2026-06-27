#include "content_order.hpp"

#include "client_state.hpp"
#include "waydisplay/wd_log.h"

#include <algorithm>
#include <mutex>

namespace waydisplay {
namespace {

void reset_pending_content_locked(ClientState& state, enum wd_client_content_owner owner) {
    state.pending_dirty_tiles.clear();
    state.pending_dirty_rect_count.store(0, std::memory_order_release);
    std::fill(state.pending_present_generation.begin(), state.pending_present_generation.end(), 0);

    state.video_present_queue.clear();
    state.pending_video_frame_dirty.store(false, std::memory_order_release);

    /* A remote content-epoch advance invalidates any upload already in
     * progress even when ownership remains on the same transport. Ordinary
     * video frames do not change this local ownership epoch. */
    const uint64_t local_epoch = owner == WD_CLIENT_CONTENT_OWNER_VIDEO
                                     ? wd_client_stream_ownership_reset_to_video(&state.stream_ownership)
                                     : wd_client_stream_ownership_reset_to_tiles(&state.stream_ownership);
    state.pending_dirty_epoch = local_epoch;
}

void reset_present_telemetry(ClientState& state) {
    std::lock_guard<std::mutex> present_lock(state.present_mutex);
    std::fill(state.pending_tile_telemetry.begin(), state.pending_tile_telemetry.end(), ClientPendingTileTelemetry{});
}

} // namespace

ClientContentEpochDecision client_accept_content_epoch(ClientState& state, uint64_t content_epoch, enum wd_client_content_owner owner) {
    if (content_epoch == 0) [[unlikely]]
    {
        return ClientContentEpochDecision::Stale;
    }

    std::lock_guard<std::mutex> transition_lock(state.remote_content_mutex);
    if (content_epoch < state.remote_content_epoch || (content_epoch == state.remote_content_epoch && owner != state.remote_content_owner)) [[unlikely]]
    {
        return ClientContentEpochDecision::Stale;
    }
    if (content_epoch == state.remote_content_epoch) [[likely]]
    {
        return ClientContentEpochDecision::Current;
    }

    {
        std::scoped_lock dirty_generation_video_lock(state.dirty_rect_mutex, state.generation_mutex,
                                                        state.video_frame_mutex);
        reset_pending_content_locked(state, owner);
    }
    reset_present_telemetry(state);
    const uint64_t previous_epoch = state.remote_content_epoch;
    const enum wd_client_content_owner previous_owner = state.remote_content_owner;
    state.remote_content_epoch = content_epoch;
    state.remote_content_owner = owner;
    WD_LOG_DEBUG("remote content ownership: epoch=%llu->%llu owner=%s->%s", (unsigned long long)previous_epoch,
                 (unsigned long long)content_epoch, previous_owner == WD_CLIENT_CONTENT_OWNER_VIDEO ? "video" : "tiles",
                 owner == WD_CLIENT_CONTENT_OWNER_VIDEO ? "video" : "tiles");
    state.render_wake.signal();
    return ClientContentEpochDecision::Advanced;
}

void client_reset_content_epoch(ClientState& state, uint64_t content_epoch, enum wd_client_content_owner owner) {
    std::lock_guard<std::mutex> transition_lock(state.remote_content_mutex);
    state.stats.tile_content_epoch_presented.store(0, std::memory_order_relaxed);
    state.stats.video_content_epoch_presented.store(0, std::memory_order_relaxed);
    {
        std::scoped_lock dirty_generation_video_lock(state.dirty_rect_mutex, state.generation_mutex,
                                                        state.video_frame_mutex);
        reset_pending_content_locked(state, owner);
    }
    reset_present_telemetry(state);
    state.remote_content_epoch = content_epoch;
    state.remote_content_owner = owner;
    WD_LOG_DEBUG("remote content ownership reset: epoch=%llu owner=%s", (unsigned long long)content_epoch,
                 owner == WD_CLIENT_CONTENT_OWNER_VIDEO ? "video" : "tiles");
    state.render_wake.signal();
}

} // namespace waydisplay
