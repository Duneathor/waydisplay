#include "waydisplay/wd_tile.h"

#include <string.h>

uint16_t wd_tiles_for_width(uint32_t width) {
    if (width == 0) {
        return 0;
    }

    return (uint16_t)((width + WD_TILE_WIDTH - 1u) / WD_TILE_WIDTH);
}

uint16_t wd_tiles_for_height(uint32_t height) {
    if (height == 0) {
        return 0;
    }

    return (uint16_t)((height + WD_TILE_HEIGHT - 1u) / WD_TILE_HEIGHT);
}

uint16_t wd_total_tiles_for_size(uint32_t width, uint32_t height) {
    return (uint16_t)(wd_tiles_for_width(width) * wd_tiles_for_height(height));
}

bool wd_tile_id_valid_for(uint16_t tile_id, uint16_t total_tiles) {
    return tile_id < total_tiles;
}

uint16_t wd_tile_x_for(uint16_t tile_id, uint16_t tiles_x) {
    if (tiles_x == 0) {
        return 0;
    }

    return (uint16_t)(tile_id % tiles_x);
}

uint16_t wd_tile_y_for(uint16_t tile_id, uint16_t tiles_x) {
    if (tiles_x == 0) {
        return 0;
    }

    return (uint16_t)(tile_id / tiles_x);
}

uint32_t wd_tile_start_x_for(uint16_t tile_id, uint16_t tiles_x) {
    return (uint32_t)wd_tile_x_for(tile_id, tiles_x) * WD_TILE_WIDTH;
}

uint32_t wd_tile_start_y_for(uint16_t tile_id, uint16_t tiles_x) {
    return (uint32_t)wd_tile_y_for(tile_id, tiles_x) * WD_TILE_HEIGHT;
}

uint32_t wd_tile_visible_width_for(uint32_t display_width,
                                   uint16_t tile_id,
                                   uint16_t tiles_x) {
    const uint32_t start_x = wd_tile_start_x_for(tile_id, tiles_x);

    if (start_x >= display_width) {
        return 0;
    }

    const uint32_t remaining = display_width - start_x;
    return remaining < WD_TILE_WIDTH ? remaining : WD_TILE_WIDTH;
}

uint32_t wd_tile_visible_height_for(uint32_t display_height,
                                    uint16_t tile_id,
                                    uint16_t tiles_x) {
    const uint32_t start_y = wd_tile_start_y_for(tile_id, tiles_x);

    if (start_y >= display_height) {
        return 0;
    }

    const uint32_t remaining = display_height - start_y;
    return remaining < WD_TILE_HEIGHT ? remaining : WD_TILE_HEIGHT;
}

uint32_t wd_fnv1a_tile_hash_xrgb8888_for(
    const uint32_t *framebuffer_xrgb8888,
    uint32_t framebuffer_width,
    uint32_t framebuffer_height,
    uint16_t tiles_x,
    uint16_t total_tiles,
    uint16_t tile_id) {
    if (!framebuffer_xrgb8888 || !wd_tile_id_valid_for(tile_id, total_tiles)) {
        return 0;
    }

    const uint32_t start_x = wd_tile_start_x_for(tile_id, tiles_x);
    const uint32_t start_y = wd_tile_start_y_for(tile_id, tiles_x);
    const uint32_t visible_width =
        wd_tile_visible_width_for(framebuffer_width, tile_id, tiles_x);
    const uint32_t visible_height =
        wd_tile_visible_height_for(framebuffer_height, tile_id, tiles_x);

    uint32_t h = 2166136261u;

    for (uint32_t y = 0; y < visible_height; ++y) {
        const uint32_t *row =
            framebuffer_xrgb8888 + (start_y + y) * framebuffer_width + start_x;

        for (uint32_t x = 0; x < visible_width; ++x) {
            h ^= row[x];
            h *= 16777619u;
        }
    }

    return h;
}

bool wd_extract_tile_xrgb8888_for(const uint32_t *framebuffer_xrgb8888,
                                  uint32_t framebuffer_width,
                                  uint32_t framebuffer_height,
                                  uint16_t tiles_x,
                                  uint16_t total_tiles,
                                  uint16_t tile_id,
                                  uint8_t *out_tile_bytes) {
    if (!framebuffer_xrgb8888 || !out_tile_bytes ||
        !wd_tile_id_valid_for(tile_id, total_tiles)) {
        return false;
    }

    const uint32_t start_x = wd_tile_start_x_for(tile_id, tiles_x);
    const uint32_t start_y = wd_tile_start_y_for(tile_id, tiles_x);
    const uint32_t visible_width =
        wd_tile_visible_width_for(framebuffer_width, tile_id, tiles_x);
    const uint32_t visible_height =
        wd_tile_visible_height_for(framebuffer_height, tile_id, tiles_x);

    memset(out_tile_bytes, 0, WD_UNCOMPRESSED_TILE_BYTES);

    for (uint32_t y = 0; y < visible_height; ++y) {
        const uint8_t *src =
            (const uint8_t *)(framebuffer_xrgb8888 +
                              (start_y + y) * framebuffer_width + start_x);
        uint8_t *dst = out_tile_bytes + y * WD_TILE_WIDTH * WD_BYTES_PER_PIXEL;

        memcpy(dst, src, visible_width * WD_BYTES_PER_PIXEL);
    }

    return true;
}

bool wd_blit_tile_xrgb8888_for(uint32_t *framebuffer_xrgb8888,
                               uint32_t framebuffer_width,
                               uint32_t framebuffer_height,
                               uint16_t tiles_x,
                               uint16_t total_tiles,
                               uint16_t tile_id,
                               const uint8_t *tile_bytes) {
    if (!framebuffer_xrgb8888 || !tile_bytes ||
        !wd_tile_id_valid_for(tile_id, total_tiles)) {
        return false;
    }

    const uint32_t start_x = wd_tile_start_x_for(tile_id, tiles_x);
    const uint32_t start_y = wd_tile_start_y_for(tile_id, tiles_x);
    const uint32_t visible_width =
        wd_tile_visible_width_for(framebuffer_width, tile_id, tiles_x);
    const uint32_t visible_height =
        wd_tile_visible_height_for(framebuffer_height, tile_id, tiles_x);

    for (uint32_t y = 0; y < visible_height; ++y) {
        uint8_t *dst =
            (uint8_t *)(framebuffer_xrgb8888 +
                        (start_y + y) * framebuffer_width + start_x);
        const uint8_t *src = tile_bytes + y * WD_TILE_WIDTH * WD_BYTES_PER_PIXEL;

        memcpy(dst, src, visible_width * WD_BYTES_PER_PIXEL);
    }

    return true;
}

bool wd_tile_id_valid(uint16_t tile_id) {
    return wd_tile_id_valid_for(tile_id, WD_TOTAL_TILES);
}

uint16_t wd_tile_x(uint16_t tile_id) {
    return wd_tile_x_for(tile_id, WD_TILES_X);
}

uint16_t wd_tile_y(uint16_t tile_id) {
    return wd_tile_y_for(tile_id, WD_TILES_X);
}

uint32_t wd_tile_start_x(uint16_t tile_id) {
    return wd_tile_start_x_for(tile_id, WD_TILES_X);
}

uint32_t wd_tile_start_y(uint16_t tile_id) {
    return wd_tile_start_y_for(tile_id, WD_TILES_X);
}

uint32_t wd_fnv1a_tile_hash_xrgb8888(const uint32_t *framebuffer_xrgb8888,
                                     uint16_t tile_id) {
    return wd_fnv1a_tile_hash_xrgb8888_for(framebuffer_xrgb8888,
                                           WD_DISPLAY_WIDTH,
                                           WD_DISPLAY_HEIGHT,
                                           WD_TILES_X,
                                           WD_TOTAL_TILES,
                                           tile_id);
}

bool wd_extract_tile_xrgb8888(const uint32_t *framebuffer_xrgb8888,
                              uint16_t tile_id,
                              uint8_t *out_tile_bytes) {
    return wd_extract_tile_xrgb8888_for(framebuffer_xrgb8888,
                                        WD_DISPLAY_WIDTH,
                                        WD_DISPLAY_HEIGHT,
                                        WD_TILES_X,
                                        WD_TOTAL_TILES,
                                        tile_id,
                                        out_tile_bytes);
}

bool wd_blit_tile_xrgb8888(uint32_t *framebuffer_xrgb8888,
                           uint16_t tile_id,
                           const uint8_t *tile_bytes) {
    return wd_blit_tile_xrgb8888_for(framebuffer_xrgb8888,
                                     WD_DISPLAY_WIDTH,
                                     WD_DISPLAY_HEIGHT,
                                     WD_TILES_X,
                                     WD_TOTAL_TILES,
                                     tile_id,
                                     tile_bytes);
}
