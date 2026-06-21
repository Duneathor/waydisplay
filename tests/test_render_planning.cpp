#include "content_order.hpp"
#include "present_telemetry.hpp"
#include "render_planning.hpp"
#include "wd_client.hpp"
#include "render_wakeup.hpp"
#include "stream_ownership.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

void test_coalesces_horizontal_and_vertical_runs() {
    std::vector<ClientDirtyRect> rects{
        {16, 0, 16, 16},
        {0, 0, 16, 16},
        {0, 16, 32, 16},
    };

    coalesce_dirty_texture_rects(rects, 64, 64);
    require(rects.size() == 1, "adjacent rectangles should coalesce");
    require(rects[0].x == 0 && rects[0].y == 0 && rects[0].w == 32 && rects[0].h == 32,
            "coalesced rectangle geometry");
}

void test_clamps_to_frame() {
    std::vector<ClientDirtyRect> rects{{48, 48, 32, 32}};
    coalesce_dirty_texture_rects(rects, 64, 64);
    require(rects.size() == 1, "partially visible rectangle should remain");
    require(rects[0].w == 16 && rects[0].h == 16, "rectangle should be clipped to frame");
}

void test_upload_planner_modes() {
    std::vector<ClientDirtyRect> sparse{{0, 0, 16, 16}, {512, 512, 16, 16}};
    DirtyTextureUploadPlan sparse_plan = plan_dirty_texture_upload(sparse, 1024, 1024);
    require(sparse_plan.mode == DirtyTextureUploadMode::Rects, "sparse rectangles should remain partial");

    std::vector<ClientDirtyRect> dense{{0, 0, 800, 800}};
    DirtyTextureUploadPlan dense_plan = plan_dirty_texture_upload(dense, 1024, 1024);
    require(dense_plan.mode == DirtyTextureUploadMode::Full, "dense update should use a full upload");
}

void test_dirty_tile_grid_deduplicates_and_coalesces() {
    ClientDirtyTileGrid grid;
    require(grid.reset(50, 34, 16, 16), "dirty grid should configure");

    require(grid.mark_rect({0, 0, 32, 32}), "large tile rectangle should mark base tiles");
    require(grid.mark_rect({0, 0, 16, 16}), "duplicate tile rectangle should be accepted");
    require(grid.dirty_tile_count() == 4, "duplicate dirty tiles should be counted once");

    std::vector<ClientDirtyRect> rects;
    grid.take_rects(rects);
    require(rects.size() == 1, "rectangular dirty tile block should become one rectangle");
    require(rects[0].x == 0 && rects[0].y == 0 && rects[0].w == 32 && rects[0].h == 32,
            "dirty grid rectangle geometry");
    require(grid.dirty_tile_count() == 0, "taking dirty rectangles should clear the grid");

    require(grid.mark_rect({48, 32, 2, 2}), "edge tile should be markable");
    grid.take_rects(rects);
    require(rects.size() == 1 && rects[0].x == 48 && rects[0].y == 32 && rects[0].w == 2 && rects[0].h == 2,
            "edge dirty tile should be clipped to framebuffer dimensions");
}

void test_upload_planner_prices_sparse_updates_separately_from_locks() {
    std::vector<ClientDirtyRect> sparse;
    for (uint16_t offset = 0; offset < 1000; offset += 100)
    {
        sparse.push_back({offset, offset, 1, 1});
    }
    const DirtyTextureUploadPlan plan = plan_dirty_texture_upload(sparse, 1024, 1024);
    require(plan.mode == DirtyTextureUploadMode::Rects,
            "multiple sparse SDL_UpdateTexture calls should not be priced as texture locks");
}

void test_dirty_tile_grid_randomized_exact_coverage() {
    constexpr uint32_t frame_width = 50;
    constexpr uint32_t frame_height = 34;
    constexpr uint16_t tile_width = 16;
    constexpr uint16_t tile_height = 16;

    ClientDirtyTileGrid grid;
    require(grid.reset(frame_width, frame_height, tile_width, tile_height),
            "randomized dirty grid should configure");

    uint32_t random_state = 0x6d2b79f5u;
    const auto next_random = [&]() {
        random_state = random_state * 1664525u + 1013904223u;
        return random_state;
    };

    for (size_t iteration = 0; iteration < 1000; ++iteration)
    {
        grid.clear();
        std::vector<uint8_t> expected(static_cast<size_t>(frame_width) * frame_height, 0);
        const uint32_t rect_count = 1u + next_random() % 24u;
        for (uint32_t i = 0; i < rect_count; ++i)
        {
            const ClientDirtyRect input{
                static_cast<uint16_t>(next_random() % 70u),
                static_cast<uint16_t>(next_random() % 50u),
                static_cast<uint16_t>(next_random() % 36u),
                static_cast<uint16_t>(next_random() % 36u),
            };
            ClientDirtyRect visible{};
            const bool should_mark = clamp_dirty_rect(input, frame_width, frame_height, visible);
            require(grid.mark_rect(input) == should_mark, "dirty-grid mark result should match clipping");
            if (!should_mark)
            {
                continue;
            }

            const uint32_t tile_x0 = visible.x / tile_width;
            const uint32_t tile_y0 = visible.y / tile_height;
            const uint32_t tile_x1 = (static_cast<uint32_t>(visible.x) + visible.w - 1u) / tile_width;
            const uint32_t tile_y1 = (static_cast<uint32_t>(visible.y) + visible.h - 1u) / tile_height;
            for (uint32_t tile_y = tile_y0; tile_y <= tile_y1; ++tile_y)
            {
                for (uint32_t tile_x = tile_x0; tile_x <= tile_x1; ++tile_x)
                {
                    const uint32_t x0 = tile_x * tile_width;
                    const uint32_t y0 = tile_y * tile_height;
                    const uint32_t x1 = std::min<uint32_t>(frame_width, x0 + tile_width);
                    const uint32_t y1 = std::min<uint32_t>(frame_height, y0 + tile_height);
                    for (uint32_t y = y0; y < y1; ++y)
                    {
                        for (uint32_t x = x0; x < x1; ++x)
                        {
                            expected[static_cast<size_t>(y) * frame_width + x] = 1;
                        }
                    }
                }
            }
        }

        std::vector<ClientDirtyRect> rects;
        grid.take_rects(rects);
        std::vector<uint8_t> actual(expected.size(), 0);
        for (const ClientDirtyRect& rect : rects)
        {
            require(rect.w != 0 && rect.h != 0, "dirty-grid output rectangles must be nonempty");
            require(static_cast<uint32_t>(rect.x) + rect.w <= frame_width &&
                        static_cast<uint32_t>(rect.y) + rect.h <= frame_height,
                    "dirty-grid output rectangles must be clipped");
            for (uint32_t y = rect.y; y < static_cast<uint32_t>(rect.y) + rect.h; ++y)
            {
                for (uint32_t x = rect.x; x < static_cast<uint32_t>(rect.x) + rect.w; ++x)
                {
                    uint8_t& count = actual[static_cast<size_t>(y) * frame_width + x];
                    ++count;
                    require(count == 1, "dirty-grid output rectangles must not overlap");
                }
            }
        }
        require(actual == expected, "dirty-grid rectangles must exactly cover marked base tiles");
        require(grid.dirty_tile_count() == 0, "taking randomized dirty work should empty the grid");
    }
}

void test_video_upload_preserves_pending_tile_work() {
    require(!should_collect_pending_tile_dirty(false, true),
            "video upload should not consume pending tile dirty regions");
    require(!should_clear_pending_tile_dirty(false, true),
            "video upload should not discard pending tile dirty regions");
    require(should_clear_pending_tile_dirty(true, false),
            "a full framebuffer upload may consume pending tile dirty regions");
}

void test_render_wakeup_sequence_and_wait() {
    ClientRenderWake wake;
    const uint64_t initial = wake.sequence();
    require(!wake.wait_for_change(initial, 1), "render wake should time out without a signal");

    std::thread signal_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        wake.signal();
    });
    require(wake.wait_for_change(initial, 500), "render wake should observe a signal");
    signal_thread.join();
    require(wake.sequence() != initial, "render wake signal should advance the sequence");

    const uint64_t observed = wake.sequence();
    wake.signal();
    require(wake.wait_for_change(observed, 500), "a signal before waiting should not be lost");
}

void test_stream_ownership_epochs() {
    ClientStreamOwnership ownership;
    const ClientContentOwnershipSnapshot initial = ownership.snapshot();
    require(initial.owner == ClientContentOwner::Tiles, "initial owner should be tiles");

    const uint64_t video_epoch = ownership.begin_video_frame();
    require(video_epoch > initial.epoch, "video frame should advance ownership epoch");
    require(ownership.is_current(video_epoch, ClientContentOwner::Video), "video epoch should be current");

    const uint64_t next_video_epoch = ownership.begin_video_frame();
    require(next_video_epoch > video_epoch, "newer video frame should supersede older frame");
    require(!ownership.is_current(video_epoch, ClientContentOwner::Video), "older video frame should become stale");

    const uint64_t tile_epoch = ownership.end_video_stream();
    require(tile_epoch > next_video_epoch, "end-of-stream should advance ownership epoch");
    require(ownership.is_current(tile_epoch, ClientContentOwner::Tiles), "tiles should own content after end-of-stream");
    require(!ownership.is_current(next_video_epoch, ClientContentOwner::Video), "video should be stale after end-of-stream");
}

void test_remote_content_epochs_reject_late_cross_transport_packets() {
    ClientState state;
    require(state.pending_dirty_tiles.reset(64, 64, 16, 16), "content test dirty grid");
    state.pending_present_generation.assign(16, 0);

    client_reset_content_epoch(state, 10, ClientContentOwner::Tiles);
    require(client_accept_content_epoch(state, 11, ClientContentOwner::Video) ==
                ClientContentEpochDecision::Advanced,
            "new video epoch should advance");
    require(client_accept_content_epoch(state, 10, ClientContentOwner::Tiles) ==
                ClientContentEpochDecision::Stale,
            "late tile epoch should be rejected after video ownership");
    require(client_accept_content_epoch(state, 11, ClientContentOwner::Tiles) ==
                ClientContentEpochDecision::Stale,
            "same epoch cannot change transport ownership");
    require(client_accept_content_epoch(state, 12, ClientContentOwner::Tiles) ==
                ClientContentEpochDecision::Advanced,
            "new tile epoch should supersede video ownership");
    require(client_accept_content_epoch(state, 11, ClientContentOwner::Video) ==
                ClientContentEpochDecision::Stale,
            "late video frame should be rejected after tile recovery");
}


void test_present_telemetry_claims_only_matching_generation_batch() {
    std::vector<ClientPendingTileTelemetry> pending(4);
    pending[0] = {1, 7, 5, 100, 11};
    pending[1] = {2, 7, 4, 120, 11};
    pending[2] = {3, 6, 9, 140, 12};
    pending[3] = {4, 7, 10, 160, 13};

    std::vector<ClientTileGenerationUpdate> updates{{0, 5}, {1, 6}, {2, 9}, {3, 9}};
    ClientPresentTelemetryBatch batch;
    claim_tile_present_telemetry(pending, updates, 7, 200, batch);

    require(batch.content_epoch == 7, "claimed telemetry should retain content epoch");
    require(batch.tile_count == 1 && batch.completion_count == 1,
            "only the exact epoch and generation should be claimed");
    require(batch.claimed_ns == 200 && batch.completion_age_sum_ns == 100 &&
                batch.oldest_completion_ns == 100 && batch.newest_completion_ns == 100,
            "claimed completion timing should be aggregated without absolute-time overflow");
    require(batch.input_sequence_count == 1 && batch.input_sequences[0] == 11,
            "input sequences should be deduplicated and bounded");
    require(pending[0].generation == 5 && pending[1].generation == 4,
            "claiming should not remove telemetry before presentation succeeds");
    commit_tile_present_telemetry(pending, updates, 7);
    require(pending[0].generation == 0 && pending[1].generation == 4,
            "successful presentation should remove only the exact matching record");
    require(pending[2].generation == 9 && pending[3].generation == 10,
            "stale-epoch and not-yet-presented records should remain pending");
}

void test_present_telemetry_input_sequence_set_is_bounded() {
    std::vector<ClientPendingTileTelemetry> pending(12);
    std::vector<ClientTileGenerationUpdate> updates;
    for (uint16_t tile_id = 0; tile_id < pending.size(); ++tile_id)
    {
        pending[tile_id] = {static_cast<uint64_t>(tile_id) + 1u, 3, 1,
                            100ull + tile_id, 1000ull + tile_id};
        updates.push_back({tile_id, 1});
    }
    ClientPresentTelemetryBatch batch;
    claim_tile_present_telemetry(pending, updates, 3, 10000, batch);
    require(batch.completion_count == pending.size(), "all matching tile completion samples should aggregate");
    require(batch.input_sequence_count == batch.input_sequences.size(),
            "input correlation set should remain bounded");
}


void test_present_telemetry_counts_large_tile_completion_once() {
    std::vector<ClientPendingTileTelemetry> pending(4);
    std::vector<ClientTileGenerationUpdate> updates;
    for (uint16_t tile_id = 0; tile_id < pending.size(); ++tile_id)
    {
        pending[tile_id] = {42, 9, 12, 500, 77};
        updates.push_back({tile_id, 12});
    }

    ClientPresentTelemetryBatch batch;
    claim_tile_present_telemetry(pending, updates, 9, 700, batch);
    require(batch.tile_count == 1 && batch.completion_count == 1,
            "one wire-tile completion covering multiple base tiles should count once");
    require(batch.completion_age_sum_ns == 200,
            "deduplicated wire-tile completion should contribute one latency sample");
    require(batch.input_sequence_count == 1 && batch.input_sequences[0] == 77,
            "large-tile input correlation should be reported once");
}

void test_tile_generation_claim_commit_and_requeue() {
    std::vector<uint64_t> pending{3, 4, 0, 7};
    std::vector<uint64_t> presented{1, 4, 0, 5};
    std::vector<ClientTileGenerationUpdate> updates;

    claim_pending_tile_generations(pending, presented, std::vector<uint16_t>{0, 1, 3}, updates);
    require(updates.size() == 2, "only generations newer than presented should be claimed");
    require(pending[0] == 0 && pending[3] == 0, "claimed generations should leave the pending set");

    requeue_tile_generation_updates(pending, updates);
    require(pending[0] == 3 && pending[3] == 7, "failed presentation should requeue generations");

    claim_all_pending_tile_generations(pending, presented, updates);
    commit_tile_generation_updates(presented, updates);
    require(presented[0] == 3 && presented[1] == 4 && presented[3] == 7,
            "successful presentation should advance presented generations");
}

void test_summary_pending_index_tracks_only_active_tiles() {
    std::vector<uint16_t> active;
    std::vector<uint32_t> positions(16, UINT32_MAX);
    require(summary_pending_index_add(active, positions, 3), "add first pending tile");
    require(summary_pending_index_add(active, positions, 7), "add second pending tile");
    require(summary_pending_index_add(active, positions, 3), "duplicate add should be idempotent");
    require(active.size() == 2, "pending index should deduplicate tiles");
    require(summary_pending_index_remove(active, positions, 3), "remove pending tile");
    require(active.size() == 1 && active[0] == 7, "swap removal should preserve remaining tile");
    require(positions[3] == UINT32_MAX && positions[7] == 0, "positions should remain consistent");
    require(summary_pending_index_remove(active, positions, 7), "remove final pending tile");
    require(active.empty(), "pending index should become empty");
}

void test_texture_upload_cost_calibration_changes_plan() {
    std::vector<ClientDirtyRect> rects;
    for (uint16_t i = 0; i < 6; ++i)
    {
        rects.push_back({static_cast<uint16_t>(i * 18), 0, 16, 16});
    }

    ClientTextureUploadCostModel low_call_cost{};
    low_call_cost.update_call_cost_ns = 1;
    low_call_cost.lock_call_cost_ns = 1000000;
    DirtyTextureUploadPlan sparse = plan_dirty_texture_upload(rects, 1024, 1024, low_call_cost);
    require(sparse.mode == DirtyTextureUploadMode::Rects,
            "low update-call overhead should preserve sparse rectangles");

    ClientTextureUploadCostModel high_call_cost = low_call_cost;
    high_call_cost.update_call_cost_ns = 1000000;
    high_call_cost.lock_call_cost_ns = 1;
    DirtyTextureUploadPlan bounded = plan_dirty_texture_upload(rects, 1024, 1024, high_call_cost);
    require(bounded.mode == DirtyTextureUploadMode::Bounds,
            "high update-call overhead should prefer one bounding upload");

    ClientTextureUploadCostModel observed{};
    observe_texture_upload_call(observed, false, 500000, 1024);
    require(observed.update_samples == 1, "update observation should be counted");
    require(observed.update_call_cost_ns > 100000, "slow update call should raise calibrated fixed cost");
    observe_texture_upload_call(observed, true, 700000, 1024);
    require(observed.lock_samples == 1, "lock observation should be counted");
}

void test_adaptive_framebuffer_upload_path() {
    DirtyTextureUploadPlan plan{};
    plan.mode = DirtyTextureUploadMode::Rects;
    plan.source_pixels = 4096;
    require(choose_framebuffer_upload_path(plan, 2, 0) == FramebufferUploadPath::Direct,
            "small uncontended sparse upload should remain direct");
    require(choose_framebuffer_upload_path(plan, 2, 300000) == FramebufferUploadPath::Staged,
            "contended sparse upload should stage");
    require(choose_framebuffer_upload_path(plan, 8, 0) == FramebufferUploadPath::Staged,
            "many sparse rectangles should stage");
    plan.mode = DirtyTextureUploadMode::Bounds;
    require(choose_framebuffer_upload_path(plan, 1, 0) == FramebufferUploadPath::Staged,
            "bounding upload should stage");
}

} // namespace

int main() {
    test_coalesces_horizontal_and_vertical_runs();
    test_clamps_to_frame();
    test_upload_planner_modes();
    test_upload_planner_prices_sparse_updates_separately_from_locks();
    test_dirty_tile_grid_deduplicates_and_coalesces();
    test_dirty_tile_grid_randomized_exact_coverage();
    test_video_upload_preserves_pending_tile_work();
    test_render_wakeup_sequence_and_wait();
    test_stream_ownership_epochs();
    test_remote_content_epochs_reject_late_cross_transport_packets();
    test_present_telemetry_claims_only_matching_generation_batch();
    test_present_telemetry_input_sequence_set_is_bounded();
    test_present_telemetry_counts_large_tile_completion_once();
    test_tile_generation_claim_commit_and_requeue();
    test_adaptive_framebuffer_upload_path();
    test_texture_upload_cost_calibration_changes_plan();
    test_summary_pending_index_tracks_only_active_tiles();
    return 0;
}
