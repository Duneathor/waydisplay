#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "waydisplay/wd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

bool wd_tile_id_valid(uint16_t tile_id);

uint16_t wd_tile_x(uint16_t tile_id);
uint16_t wd_tile_y(uint16_t tile_id);

uint32_t wd_tile_start_x(uint16_t tile_id);
uint32_t wd_tile_start_y(uint16_t tile_id);

uint32_t wd_fnv1a_tile_hash_xrgb8888(const uint32_t *framebuffer_xrgb8888,
                                     uint16_t tile_id);

bool wd_extract_tile_xrgb8888(const uint32_t *framebuffer_xrgb8888,
                              uint16_t tile_id,
                              uint8_t *out_tile_bytes);

bool wd_blit_tile_xrgb8888(uint32_t *framebuffer_xrgb8888,
                           uint16_t tile_id,
                           const uint8_t *tile_bytes);

#ifdef __cplusplus
}
#endif
