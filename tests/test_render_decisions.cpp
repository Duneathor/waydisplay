#include "present_telemetry.hpp"
#include "render_planning.hpp"
#include "render_wakeup.hpp"

#include "waydisplay/wd_config.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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

void test_dirty_grid_invalid_and_empty_paths() {
    ClientDirtyTileGrid grid;
    std::vector<ClientDirtyRect> rects{{1, 1, 1, 1}};
    grid.take_rects(rects);
    require(rects.empty(), "taking an unconfigured dirty grid should produce no rectangles");
    require(!grid.mark_rect({0, 0, 1, 1}), "unconfigured dirty grid should reject marks");

    require(grid.reset(32, 32, 16, 16), "valid dirty grid configuration");
    require(grid.mark_rect({0, 0, 16, 16}), "configured grid should accept marks");
    require(!grid.reset(0, 32, 16, 16), "zero frame width should invalidate the grid");
    require(grid.dirty_tile_count() == 0 && !grid.mark_rect({0, 0, 1, 1}),
            "invalid reset should clear previous dirty work");

    ClientDirtyRect out{};
    require(!clamp_dirty_rect({0, 0, 0, 1}, 10, 10, out), "zero-width rectangle is invalid");
    require(!clamp_dirty_rect({10, 0, 1, 1}, 10, 10, out), "off-frame rectangle is invalid");
    require(clamp_dirty_rect({8, 9, 5, 5}, 10, 10, out) && out.w == 2 && out.h == 1,
            "partially visible rectangle should clip to the framebuffer");
}

void test_coalescing_and_bounds_edge_paths() {
    std::vector<ClientDirtyRect> empty;
    coalesce_dirty_texture_rects(empty, 100, 100);
    require(empty.empty(), "coalescing an empty set should be a no-op");
    require(bounding_dirty_rect(empty).w == 0, "empty bounds should be empty");

    std::vector<ClientDirtyRect> invalid{{0, 0, 0, 4}, {100, 100, 1, 1}};
    coalesce_dirty_texture_rects(invalid, 10, 10);
    require(invalid.empty(), "all-invalid rectangles should be removed");

    std::vector<ClientDirtyRect> overlap{{8, 0, 8, 8}, {0, 0, 12, 8}, {0, 8, 16, 8}};
    coalesce_dirty_texture_rects(overlap, 32, 32);
    require(overlap.size() == 1 && overlap[0].x == 0 && overlap[0].y == 0 &&
                overlap[0].w == 16 && overlap[0].h == 16,
            "overlapping horizontal spans and matching vertical spans should merge");

    const ClientDirtyRect bounds = bounding_dirty_rect({{8, 4, 2, 3}, {1, 2, 4, 8}});
    require(bounds.x == 1 && bounds.y == 2 && bounds.w == 9 && bounds.h == 8,
            "bounds should cover the union of all rectangles");
}

void test_upload_plan_branch_matrix() {
    require(plan_dirty_texture_upload({}, 100, 100).mode == DirtyTextureUploadMode::Full,
            "empty dirty work should request a full upload");
    require(plan_dirty_texture_upload({{0, 0, 1, 1}}, 0, 100).mode == DirtyTextureUploadMode::Full,
            "zero-area framebuffer should request a full upload");

    std::vector<ClientDirtyRect> many(WD_CLIENT_DIRTY_RECT_FULL_UPLOAD_THRESHOLD, {0, 0, 1, 1});
    require(plan_dirty_texture_upload(many, 1000, 1000).mode == DirtyTextureUploadMode::Full,
            "too many update calls should force a full upload");
    require(plan_dirty_texture_upload({{0, 0, 90, 90}}, 100, 100).mode == DirtyTextureUploadMode::Full,
            "large dirty coverage should force a full upload");

    ClientTextureUploadCostModel costs{};
    costs.update_samples = 4;
    costs.lock_samples = 4;
    costs.pixel_samples = 4;
    costs.snapshot_samples = 4;
    costs.update_call_cost_ns = 1000000;
    costs.lock_call_cost_ns = 1;
    costs.pixel_cost_q16 = 1ull << 16u;
    costs.snapshot_pixel_cost_q16 = 1ull << 16u;
    const DirtyTextureUploadPlan bounds_plan =
        plan_dirty_texture_upload({{10, 10, 10, 10}, {30, 10, 10, 10}}, 1000, 1000, costs);
    require(bounds_plan.mode == DirtyTextureUploadMode::Bounds && bounds_plan.bounds.x == 10 &&
                bounds_plan.bounds.w == 30,
            "high per-update overhead should select one bounding lock");
}

void test_cost_observation_limits_and_sample_paths() {
    ClientTextureUploadCostModel model{};
    observe_framebuffer_snapshot(model, 100, 0);
    require(model.snapshot_samples == 0, "zero-pixel snapshots should not affect calibration");

    observe_texture_upload_call(model, false, 50000, 1);
    require(model.update_samples == 1 && model.pixel_samples == 0,
            "small updates should calibrate fixed cost only");
    observe_texture_upload_call(model, true, 200000, 65536);
    require(model.lock_samples == 1 && model.pixel_samples == 1,
            "large lock uploads should calibrate fixed and variable cost");
    observe_framebuffer_snapshot(model, 65536, 65536);
    require(model.snapshot_samples == 1, "nonempty snapshots should calibrate snapshot pixel cost");

    model.update_samples = UINT32_MAX;
    model.pixel_samples = UINT32_MAX;
    model.snapshot_samples = UINT32_MAX;
    observe_texture_upload_call(model, false, 100000, 65536);
    observe_framebuffer_snapshot(model, 100000, 65536);
    require(model.update_samples == UINT32_MAX && model.pixel_samples == UINT32_MAX &&
                model.snapshot_samples == UINT32_MAX,
            "saturated observation counters must not wrap");
}

void test_generation_claim_failure_and_stale_paths() {
    std::vector<uint64_t> pending{3, 4};
    std::vector<uint64_t> mismatched_presented{1};
    std::vector<ClientTileGenerationUpdate> updates{{9, 9}};
    claim_pending_tile_generations(pending, mismatched_presented, {0}, updates);
    require(updates.empty() && pending[0] == 3, "mismatched generation vectors should not mutate state");

    std::vector<uint64_t> presented{3, 1};
    claim_pending_tile_generations(pending, presented, {9, 0, 1, 1}, updates);
    require(updates.size() == 1 && updates[0].tile_id == 1 && updates[0].generation == 4,
            "claim should skip invalid, already-presented, and duplicate work");
    require(pending[0] == 0 && pending[1] == 0, "claimed IDs should be cleared even when already presented");

    std::vector<uint64_t> all_pending{1, 0};
    claim_all_pending_tile_generations(all_pending, mismatched_presented, updates);
    require(updates.empty() && all_pending[0] == 1, "claim-all should reject mismatched vectors");

    std::vector<uint64_t> queue{5, 0};
    requeue_tile_generation_updates(queue, {{0, 4}, {1, 7}, {8, 9}});
    require(queue[0] == 5 && queue[1] == 7, "requeue should preserve newer pending generations");
    commit_tile_generation_updates(presented, {{0, 2}, {1, 6}, {8, 9}});
    require(presented[0] == 3 && presented[1] == 6, "commit should only advance valid generations");
}

void test_summary_index_consistency_checks() {
    std::vector<uint16_t> active;
    std::vector<uint32_t> positions(4, UINT32_MAX);
    require(!summary_pending_index_add(active, positions, 4), "out-of-range tile cannot be indexed");
    require(summary_pending_index_add(active, positions, 1), "first tile should be indexed");
    require(summary_pending_index_add(active, positions, 3), "second tile should be indexed");
    require(summary_pending_index_add(active, positions, 1) && active.size() == 2,
            "duplicate add should be idempotent");
    require(summary_pending_index_remove(active, positions, 1), "indexed tile should be removable");
    require(active.size() == 1 && active[0] == 3 && positions[3] == 0,
            "removal should repair the moved tile position");
    require(summary_pending_index_remove(active, positions, 1), "removing an absent tile should be idempotent");
    require(!summary_pending_index_remove(active, positions, 4), "out-of-range removal should fail");

    positions[2] = 99;
    require(!summary_pending_index_remove(active, positions, 2), "corrupt position should be detected");
}

void test_present_telemetry_filters_and_time_safety() {
    std::vector<ClientPendingTileTelemetry> pending(5);
    pending[0] = {0, 7, 1, 10, 0};
    pending[1] = {5, 7, 2, 200, 12};
    pending[2] = {5, 7, 3, 150, 12};
    pending[3] = {6, 8, 4, 100, 13};
    pending[4] = {7, 7, 5, 300, 0};
    std::vector<ClientTileGenerationUpdate> updates{{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {99, 1}};

    ClientPresentTelemetryBatch batch;
    claim_tile_present_telemetry(pending, updates, 7, 175, batch);
    require(batch.tile_count == 2 && batch.completion_count == 2,
            "zero IDs, duplicate completions, stale epochs, and invalid tiles should be filtered");
    require(batch.completion_age_sum_ns == 0,
            "future completion timestamps must not underflow age accumulation");
    require(batch.input_sequence_count == 1 && batch.input_sequences[0] == 12,
            "duplicate and zero input sequences should be ignored");

    commit_tile_present_telemetry(pending, updates, 7);
    require(pending[1].completion_id == 0 && pending[2].completion_id == 0 &&
                pending[3].completion_id == 6,
            "commit should clear exact current-epoch generations only");
}

void test_boolean_render_decisions_and_zero_timeout_wakeup() {
    require(should_collect_pending_tile_dirty(false, false), "idle tile path should collect dirty work");
    require(!should_collect_pending_tile_dirty(true, false), "full upload should not also collect partial work");
    require(!should_collect_pending_tile_dirty(false, true), "pending video should preserve tile dirty work");
    require(should_clear_pending_tile_dirty(true, false), "successful full tile upload should clear work");
    require(!should_clear_pending_tile_dirty(true, true), "video upload must not clear tile work");

    ClientRenderWake wake;
    const uint64_t observed = wake.sequence();
    require(!wake.wait_for_change(observed, 0), "zero-timeout wait should return immediately");
    wake.signal();
    require(wake.wait_for_change(observed, 0), "already-observed signal should win even with zero timeout");
}

} // namespace

int main() {
    test_dirty_grid_invalid_and_empty_paths();
    test_coalescing_and_bounds_edge_paths();
    test_upload_plan_branch_matrix();
    test_cost_observation_limits_and_sample_paths();
    test_generation_claim_failure_and_stale_paths();
    test_summary_index_consistency_checks();
    test_present_telemetry_filters_and_time_safety();
    test_boolean_render_decisions_and_zero_timeout_wakeup();
    return 0;
}
