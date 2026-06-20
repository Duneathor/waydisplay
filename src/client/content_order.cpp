#include "content_order.hpp"

#include "wd_client.hpp"

#include <algorithm>
#include <mutex>

namespace waydisplay {
namespace {

void reset_pending_content_locked(ClientState& state, ClientContentOwner owner) {
    state.pending_dirty_tiles.clear();
    state.pending_dirty_rect_count.store(0, std::memory_order_release);
    std::fill(state.pending_present_generation.begin(), state.pending_present_generation.end(), 0);

    state.video_framebuffer.clear();
    state.video_frame_width = 0;
    state.video_frame_height = 0;
    state.video_frame_id = 0;
    state.video_frame_pts_usec = 0;
    state.video_frame_epoch = 0;
    state.pending_video_frame_dirty.store(false, std::memory_order_release);

    const uint64_t local_epoch = owner == ClientContentOwner::Video
                                     ? state.stream_ownership.begin_video_frame()
                                     : state.stream_ownership.reset_to_tiles();
    state.pending_dirty_epoch = local_epoch;
}

void reset_present_telemetry(ClientState& state) {
    std::lock_guard<std::mutex> present_lock(state.present_mutex);
    std::fill(state.pending_tile_telemetry.begin(), state.pending_tile_telemetry.end(),
              ClientPendingTileTelemetry{});
}

} // namespace

ClientContentEpochDecision client_accept_content_epoch(ClientState& state, uint64_t content_epoch,
                                                       ClientContentOwner owner) {
    if (content_epoch == 0)
    {
        return ClientContentEpochDecision::Stale;
    }

    std::lock_guard<std::mutex> transition_lock(state.remote_content_mutex);
    if (content_epoch < state.remote_content_epoch ||
        (content_epoch == state.remote_content_epoch && owner != state.remote_content_owner))
    {
        return ClientContentEpochDecision::Stale;
    }
    if (content_epoch == state.remote_content_epoch)
    {
        return ClientContentEpochDecision::Current;
    }

    {
        std::lock_guard<std::mutex> dirty_lock(state.dirty_rect_mutex);
        std::lock_guard<std::mutex> generation_lock(state.generation_mutex);
        std::lock_guard<std::mutex> video_lock(state.video_frame_mutex);
        reset_pending_content_locked(state, owner);
    }
    reset_present_telemetry(state);
    state.remote_content_epoch = content_epoch;
    state.remote_content_owner = owner;
    state.render_wake.signal();
    return ClientContentEpochDecision::Advanced;
}

void client_reset_content_epoch(ClientState& state, uint64_t content_epoch, ClientContentOwner owner) {
    std::lock_guard<std::mutex> transition_lock(state.remote_content_mutex);
    {
        std::lock_guard<std::mutex> dirty_lock(state.dirty_rect_mutex);
        std::lock_guard<std::mutex> generation_lock(state.generation_mutex);
        std::lock_guard<std::mutex> video_lock(state.video_frame_mutex);
        reset_pending_content_locked(state, owner);
    }
    reset_present_telemetry(state);
    state.remote_content_epoch = content_epoch;
    state.remote_content_owner = owner;
    state.render_wake.signal();
}

} // namespace waydisplay
