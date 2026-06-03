#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_DISPLAY_WIDTH  1664u
#define WD_DISPLAY_HEIGHT 1024u

#define WD_TILE_WIDTH  128u
#define WD_TILE_HEIGHT 64u

#define WD_TILES_X     (((WD_DISPLAY_WIDTH) + (WD_TILE_WIDTH) - 1u) / (WD_TILE_WIDTH))
#define WD_TILES_Y     (((WD_DISPLAY_HEIGHT) + (WD_TILE_HEIGHT) - 1u) / (WD_TILE_HEIGHT))
#define WD_TOTAL_TILES (WD_TILES_X * WD_TILES_Y)

#define WD_BYTES_PER_PIXEL 4u

#define WD_FRAMEBUFFER_PIXELS ((uint32_t)(WD_DISPLAY_WIDTH * WD_DISPLAY_HEIGHT))
#define WD_FRAMEBUFFER_BYTES  ((uint32_t)(WD_FRAMEBUFFER_PIXELS * WD_BYTES_PER_PIXEL))

#define WD_UNCOMPRESSED_TILE_BYTES ((uint32_t)(WD_TILE_WIDTH * WD_TILE_HEIGHT * WD_BYTES_PER_PIXEL))

#define WD_UDP_PAYLOAD_TARGET 1200u

#define WD_ZSTD_LEVEL 1

#ifdef __cplusplus
}
#endif
