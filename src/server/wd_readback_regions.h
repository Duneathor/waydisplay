#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_readback_region {
    int x;
    int y;
    int width;
    int height;
};

/* Build a small set of coalesced readback rectangles from the base-tile
 * damage grid. If the output capacity is exceeded, the planner falls back to
 * one bounding rectangle so readback cost remains bounded. */
size_t wd_readback_plan_regions(bool full_damage, const bool* damage_tiles, uint32_t damage_tile_count,
                                uint32_t total_tiles, uint16_t tiles_x, uint16_t tile_width, uint16_t tile_height,
                                int max_width, int max_height, struct wd_readback_region* regions,
                                size_t region_capacity, bool* out_full_readback);

#ifdef __cplusplus
}
#endif
