#include "waydisplay/wd_tile.h"

#include <string.h>

bool wd_tile_id_valid(uint16_t tile_id) {
    return tile_id < WD_TOTAL_TILES;
}

uint16_t wd_tile_x(uint16_t tile_id) {
    return (uint16_t)(tile_id % WD_TILES_X);
}

uint16_t wd_tile_y(uint16_t tile_id) {
    return (uint16_t)(tile_id / WD_TILES_X);
}

uint32_t wd_tile_start_x(uint16_t tile_id) {
    return (uint32_t)wd_tile_x(tile_id) * WD_TILE_WIDTH;
}

uint32_t wd_tile_start_y(uint16_t tile_id) {
    return (uint32_t)wd_tile_y(tile_id) * WD_TILE_HEIGHT;
}

uint32_t wd_fnv1a_tile_hash_xrgb8888(const uint32_t *framebuffer_xrgb8888,
                                     uint16_t tile_id) {
    if (!framebuffer_xrgb8888 || !wd_tile_id_valid(tile_id)) {
        return 0;
    }

    const uint32_t start_x = wd_tile_start_x(tile_id);
    const uint32_t start_y = wd_tile_start_y(tile_id);

    uint32_t h = 2166136261u;

    for (uint32_t y = 0; y < WD_TILE_HEIGHT; ++y) {
        const uint32_t *row =
            framebuffer_xrgb8888 + (start_y + y) * WD_DISPLAY_WIDTH + start_x;

        for (uint32_t x = 0; x < WD_TILE_WIDTH; ++x) {
            h ^= row[x];
            h *= 16777619u;
        }
    }

    return h;
}

bool wd_extract_tile_xrgb8888(const uint32_t *framebuffer_xrgb8888,
                              uint16_t tile_id,
                              uint8_t *out_tile_bytes) {
    if (!framebuffer_xrgb8888 || !out_tile_bytes || !wd_tile_id_valid(tile_id)) {
        return false;
    }

    const uint32_t start_x = wd_tile_start_x(tile_id);
    const uint32_t start_y = wd_tile_start_y(tile_id);

    for (uint32_t y = 0; y < WD_TILE_HEIGHT; ++y) {
        const uint8_t *src =
            (const uint8_t *)(framebuffer_xrgb8888 +
                              (start_y + y) * WD_DISPLAY_WIDTH +
                              start_x);

        uint8_t *dst =
            out_tile_bytes + y * WD_TILE_WIDTH * WD_BYTES_PER_PIXEL;

        memcpy(dst, src, WD_TILE_WIDTH * WD_BYTES_PER_PIXEL);
    }

    return true;
}

bool wd_blit_tile_xrgb8888(uint32_t *framebuffer_xrgb8888,
                           uint16_t tile_id,
                           const uint8_t *tile_bytes) {
    if (!framebuffer_xrgb8888 || !tile_bytes || !wd_tile_id_valid(tile_id)) {
        return false;
    }

    const uint32_t start_x = wd_tile_start_x(tile_id);
    const uint32_t start_y = wd_tile_start_y(tile_id);

    for (uint32_t y = 0; y < WD_TILE_HEIGHT; ++y) {
        uint8_t *dst =
            (uint8_t *)(framebuffer_xrgb8888 +
                        (start_y + y) * WD_DISPLAY_WIDTH +
                        start_x);

        const uint8_t *src =
            tile_bytes + y * WD_TILE_WIDTH * WD_BYTES_PER_PIXEL;

        memcpy(dst, src, WD_TILE_WIDTH * WD_BYTES_PER_PIXEL);
    }

    return true;
}
