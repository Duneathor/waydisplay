#pragma once

#include <cstdint>

namespace waydisplay {

struct ClientRemoteTilePresentDecision {
    bool remote_frame_dirty         = false;
    bool count_empty_remote_wakeup  = false;
};

/*
 * Decide whether a renderer wake still represents remote tile work after the
 * direct-upload and legacy dirty-rectangle paths have been inspected.
 *
 * This helper intentionally mirrors the SDL render loop so the wake/present
 * decision can be covered without requiring an SDL renderer in unit tests.
 */
constexpr ClientRemoteTilePresentDecision client_remote_tile_present_decide(
    bool remote_frame_dirty, bool direct_tile_updated, bool texture_needs_full_upload,
    bool stale_video_needs_tile_restore, uint64_t source_dirty_rect_count) {
    ClientRemoteTilePresentDecision decision{};
    decision.remote_frame_dirty = remote_frame_dirty;
    decision.count_empty_remote_wakeup =
        remote_frame_dirty && !direct_tile_updated && !texture_needs_full_upload &&
        !stale_video_needs_tile_restore && source_dirty_rect_count == 0;
    if (decision.count_empty_remote_wakeup)
    {
        decision.remote_frame_dirty = false;
    }
    return decision;
}

} // namespace waydisplay
