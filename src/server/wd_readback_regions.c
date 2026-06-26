#include "wd_readback_regions.h"

#include <limits.h>

static void set_full_region(int max_width, int max_height, struct wd_readback_region* regions, bool* out_full_readback) {
    regions[0] = (struct wd_readback_region){.x = 0, .y = 0, .width = max_width, .height = max_height};
    *out_full_readback = true;
}

size_t wd_readback_plan_regions(bool full_damage, const bool* damage_tiles, uint32_t damage_tile_count,
                                uint32_t total_tiles, uint16_t tiles_x, uint16_t tile_width, uint16_t tile_height,
                                int max_width, int max_height, struct wd_readback_region* regions,
                                size_t region_capacity, bool* out_full_readback) {
    if (!regions || region_capacity == 0 || !out_full_readback || max_width <= 0 || max_height <= 0)
    {
        return 0;
    }

    if (full_damage || !damage_tiles || damage_tile_count == 0 || total_tiles == 0 || tiles_x == 0 || tile_width == 0 ||
        tile_height == 0)
    {
        set_full_region(max_width, max_height, regions, out_full_readback);
        return 1;
    }

    int    bound_x1     = INT_MAX;
    int    bound_y1     = INT_MAX;
    int    bound_x2     = 0;
    int    bound_y2     = 0;
    size_t region_count = 0;
    bool   overflowed   = false;

    const uint32_t row_count = (total_tiles + tiles_x - 1u) / tiles_x;
    for (uint32_t row = 0; row < row_count; ++row)
    {
        uint32_t column = 0;
        while (column < tiles_x)
        {
            const uint32_t tile_id = row * tiles_x + column;
            if (tile_id >= total_tiles || !damage_tiles[tile_id])
            {
                ++column;
                continue;
            }

            const uint32_t run_start = column;
            while (column < tiles_x)
            {
                const uint32_t run_tile_id = row * tiles_x + column;
                if (run_tile_id >= total_tiles || !damage_tiles[run_tile_id])
                {
                    break;
                }
                ++column;
            }

            int x1 = (int)(run_start * tile_width);
            int y1 = (int)(row * tile_height);
            int x2 = (int)(column * tile_width);
            int y2 = y1 + tile_height;
            if (x2 > max_width)
            {
                x2 = max_width;
            }
            if (y2 > max_height)
            {
                y2 = max_height;
            }
            if (x1 >= x2 || y1 >= y2)
            {
                continue;
            }

            if (x1 < bound_x1)
            {
                bound_x1 = x1;
            }
            if (y1 < bound_y1)
            {
                bound_y1 = y1;
            }
            if (x2 > bound_x2)
            {
                bound_x2 = x2;
            }
            if (y2 > bound_y2)
            {
                bound_y2 = y2;
            }

            bool merged = false;
            for (size_t i = 0; i < region_count; ++i)
            {
                struct wd_readback_region* region = &regions[i];
                if (region->x == x1 && region->width == x2 - x1 && region->y + region->height == y1)
                {
                    region->height += y2 - y1;
                    merged = true;
                    break;
                }
            }

            if (!merged)
            {
                if (region_count >= region_capacity)
                {
                    overflowed = true;
                    continue;
                }
                regions[region_count++] =
                    (struct wd_readback_region){.x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1};
            }
        }
    }

    if (bound_x1 == INT_MAX)
    {
        set_full_region(max_width, max_height, regions, out_full_readback);
        return 1;
    }

    if (overflowed)
    {
        regions[0] = (struct wd_readback_region){
            .x = bound_x1, .y = bound_y1, .width = bound_x2 - bound_x1, .height = bound_y2 - bound_y1};
        region_count = 1;
    }

    *out_full_readback = region_count == 1 && regions[0].x == 0 && regions[0].y == 0 &&
                         regions[0].width == max_width && regions[0].height == max_height;
    return region_count;
}
