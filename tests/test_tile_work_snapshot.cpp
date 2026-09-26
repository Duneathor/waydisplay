#include "wd_tile_work_snapshot.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_lookup_and_selection() {
    wd_tile_work_snapshot snapshot{};
    require(wd_tile_work_snapshot_add(&snapshot, 17, false, 41), "add clean entry");
    require(wd_tile_work_snapshot_add(&snapshot, 18, true, 42), "add selected entry");

    bool     selected = false;
    uint64_t epoch    = 0;
    require(wd_tile_work_snapshot_lookup(&snapshot, 18, &selected, &epoch), "lookup selected entry");
    require(selected && epoch == 42, "selected entry retains state");
    require(!wd_tile_work_snapshot_lookup(&snapshot, 19, &selected, &epoch), "missing entry stays absent");

    const uint16_t child[] = {17, 18};
    require(wd_tile_work_snapshot_any_selected(&snapshot, child, 2), "child region sees selected tile");
    const uint16_t clean[] = {17};
    require(!wd_tile_work_snapshot_any_selected(&snapshot, clean, 1), "clean region stays clean");
}

void test_duplicate_updates_in_place() {
    wd_tile_work_snapshot snapshot{};
    require(wd_tile_work_snapshot_add(&snapshot, 9, false, 2), "add initial entry");
    require(wd_tile_work_snapshot_add(&snapshot, 9, true, 3), "update duplicate entry");
    require(snapshot.count == 1, "duplicate does not grow compact snapshot");

    bool     selected = false;
    uint64_t epoch    = 0;
    require(wd_tile_work_snapshot_lookup(&snapshot, 9, &selected, &epoch), "lookup updated entry");
    require(selected && epoch == 3, "duplicate updates selected flag and epoch");
}

void test_capacity_is_bounded() {
    wd_tile_work_snapshot snapshot{};
    for (uint16_t i = 0; i < WD_WIRE_TILE_MAX_BASE_TILES; ++i)
    {
        require(wd_tile_work_snapshot_add(&snapshot, i, (i % 3u) == 0u, (uint64_t)i + 1u), "fill compact snapshot");
    }
    require(snapshot.count == WD_WIRE_TILE_MAX_BASE_TILES, "snapshot reaches fixed wire-region capacity");
    require(!wd_tile_work_snapshot_add(&snapshot, WD_WIRE_TILE_MAX_BASE_TILES, true, 99), "snapshot rejects overflow");

    wd_tile_work_snapshot_reset(&snapshot);
    require(snapshot.count == 0, "reset makes snapshot reusable");
}

} // namespace

int main() {
    test_lookup_and_selection();
    test_duplicate_updates_in_place();
    test_capacity_is_bounded();
    return 0;
}
