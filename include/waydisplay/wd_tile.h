#pragma once

#include "waydisplay/wd_config.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t wd_tiles_for_width_with_tile(uint32_t width, uint16_t tile_width);
uint16_t wd_tiles_for_height_with_tile(uint32_t height, uint16_t tile_height);
uint16_t wd_total_tiles_for_size_with_tile(uint32_t width, uint32_t height, uint16_t tile_width, uint16_t tile_height);

uint16_t wd_tiles_for_width(uint32_t width);
uint16_t wd_tiles_for_height(uint32_t height);
uint16_t wd_total_tiles_for_size(uint32_t width, uint32_t height);

bool     wd_tile_id_valid_for(uint16_t tile_id, uint16_t total_tiles);
uint16_t wd_tile_x_for(uint16_t tile_id, uint16_t tiles_x);
uint16_t wd_tile_y_for(uint16_t tile_id, uint16_t tiles_x);
uint32_t wd_tile_start_x_for_tile(uint16_t tile_id, uint16_t tiles_x, uint16_t tile_width);
uint32_t wd_tile_start_y_for_tile(uint16_t tile_id, uint16_t tiles_x, uint16_t tile_height);
uint32_t wd_tile_visible_width_for_tile(uint32_t display_width, uint16_t tile_id, uint16_t tiles_x, uint16_t tile_width);
uint32_t wd_tile_visible_height_for_tile(uint32_t display_height, uint16_t tile_id, uint16_t tiles_x, uint16_t tile_height);

uint32_t wd_tile_start_x_for(uint16_t tile_id, uint16_t tiles_x);
uint32_t wd_tile_start_y_for(uint16_t tile_id, uint16_t tiles_x);
uint32_t wd_tile_visible_width_for(uint32_t display_width, uint16_t tile_id, uint16_t tiles_x);
uint32_t wd_tile_visible_height_for(uint32_t display_height, uint16_t tile_id, uint16_t tiles_x);

uint32_t wd_fnv1a_tile_hash_xrgb8888_for_tile(const uint32_t* framebuffer_xrgb8888, uint32_t framebuffer_width, uint32_t framebuffer_height,
                                                  uint16_t tiles_x, uint16_t total_tiles, uint16_t tile_id, uint16_t tile_width,
                                                  uint16_t tile_height);

bool wd_extract_tile_xrgb8888_for_tile(const uint32_t* framebuffer_xrgb8888, uint32_t framebuffer_width, uint32_t framebuffer_height,
                                       uint16_t tiles_x, uint16_t total_tiles, uint16_t tile_id, uint16_t tile_width, uint16_t tile_height,
                                       uint8_t* out_tile_bytes);

bool wd_blit_tile_xrgb8888_for_tile(uint32_t* framebuffer_xrgb8888, uint32_t framebuffer_width, uint32_t framebuffer_height,
                                    uint16_t tiles_x, uint16_t total_tiles, uint16_t tile_id, uint16_t tile_width, uint16_t tile_height,
                                    const uint8_t* tile_bytes);

uint32_t wd_fnv1a_tile_hash_xrgb8888_for(const uint32_t* framebuffer_xrgb8888, uint32_t framebuffer_width, uint32_t framebuffer_height,
                                         uint16_t tiles_x, uint16_t total_tiles, uint16_t tile_id);

bool wd_extract_tile_xrgb8888_for(const uint32_t* framebuffer_xrgb8888, uint32_t framebuffer_width, uint32_t framebuffer_height,
                                  uint16_t tiles_x, uint16_t total_tiles, uint16_t tile_id, uint8_t* out_tile_bytes);

bool wd_blit_tile_xrgb8888_for(uint32_t* framebuffer_xrgb8888, uint32_t framebuffer_width, uint32_t framebuffer_height, uint16_t tiles_x,
                               uint16_t total_tiles, uint16_t tile_id, const uint8_t* tile_bytes);

bool     wd_tile_id_valid(uint16_t tile_id);
uint16_t wd_tile_x(uint16_t tile_id);
uint16_t wd_tile_y(uint16_t tile_id);
uint32_t wd_tile_start_x(uint16_t tile_id);
uint32_t wd_tile_start_y(uint16_t tile_id);
uint32_t wd_fnv1a_tile_hash_xrgb8888(const uint32_t* framebuffer_xrgb8888, uint16_t tile_id);
bool     wd_extract_tile_xrgb8888(const uint32_t* framebuffer_xrgb8888, uint16_t tile_id, uint8_t* out_tile_bytes);
bool     wd_blit_tile_xrgb8888(uint32_t* framebuffer_xrgb8888, uint16_t tile_id, const uint8_t* tile_bytes);

#ifdef __cplusplus
}
#endif
