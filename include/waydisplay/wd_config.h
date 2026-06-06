#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Display/tile defaults. */
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

/* Network defaults and limits. */
#define WD_DEFAULT_TCP_PORT                   5000u
#define WD_TCP_HANDSHAKE_TIMEOUT_MS           3000L
#define WD_TCP_CONNECTED_SEND_TIMEOUT_MS      3000L
#define WD_TCP_MAX_PAYLOAD_SIZE               (2u * 1024u * 1024u)
#define WD_UDP_PAYLOAD_TARGET                 1200u
#define WD_UDP_MAX_PAYLOAD                    65487u
#define WD_UDP_SOCKET_BUFFER_BYTES            (16 * 1024 * 1024)
#define WD_MIN_PROBED_UDP_PAYLOAD             512u

/* Stream policy defaults. */
#define WD_DEFAULT_PARTIAL_FPS                        30u
#define WD_DEFAULT_LIMITED_TILES_PER_SECOND           120u
#define WD_MAX_REASONABLE_FPS                         120u
#define WD_MAX_REASONABLE_TILES_PER_SECOND            10000u
#define WD_DEFAULT_RETRANSMIT_TILES_PER_SECOND        32u
#define WD_FULL_MODE_RETRANSMIT_TILES_PER_SECOND      128u
#define WD_PARTIAL_MODE_RETRANSMIT_TILES_PER_SECOND   64u
#define WD_LIMITED_MODE_RETRANSMIT_DIVISOR            5u
#define WD_LIMITED_MODE_RETRANSMIT_MIN_TILES_PER_SEC  8u
#define WD_MAX_REASONABLE_RETRANSMIT_TILES_PER_SECOND 2000u

/* Client interaction/render-loop tuning. */
#define WD_CLIENT_DEFAULT_TARGET_FPS            WD_DEFAULT_PARTIAL_FPS
#define WD_CLIENT_DEFAULT_MAX_TILES_PER_SECOND  WD_DEFAULT_LIMITED_TILES_PER_SECOND
#define WD_CLIENT_STATS_INTERVAL_NS             1000000000ull
#define WD_CLIENT_RESIZE_DEBOUNCE_NS            150000000ull
#define WD_CLIENT_FRAME_DELAY_MS                8
#define WD_CLIENT_MIN_WINDOW_WIDTH              64
#define WD_CLIENT_MIN_WINDOW_HEIGHT             64
#define WD_CLIENT_MAX_DIMENSION                 16384u
#define WD_CLIENT_MAX_FRAMEBUFFER_BYTES         (512ull * 1024ull * 1024ull)
#define WD_CLIENT_WHEEL_AXIS_STEP               60
#define WD_CLIENT_CONTEXT_MENU_PADDING_X        8
#define WD_CLIENT_CONTEXT_MENU_PADDING_Y        7
#define WD_CLIENT_CONTEXT_MENU_ITEM_HEIGHT      20
#define WD_CLIENT_CONTEXT_MENU_WIDTH            184
#define WD_CLIENT_CONTEXT_MENU_TEXT_SCALE       1
#define WD_CLIENT_CONTEXT_MENU_TEXT_X           8
#define WD_CLIENT_CONTEXT_MENU_TEXT_Y           6

/* Server interaction tuning. */
#define WD_FALLBACK_MOVE_ZONE_HEIGHT 32.0
#define WD_RESIZE_EDGE_ZONE          8.0
#define WD_MIN_WINDOW_WIDTH          120u
#define WD_MIN_WINDOW_HEIGHT         80u
#define WD_XDG_ACTIVATION_TOKEN_TIMEOUT_MS 10000

/* Xwayland decoration/layout defaults. */
#define WD_XWAYLAND_DEFAULT_WIDTH      800u
#define WD_XWAYLAND_DEFAULT_HEIGHT     600u
#define WD_XWAYLAND_MIN_VISIBLE_WIDTH  64u
#define WD_XWAYLAND_MIN_VISIBLE_HEIGHT 48u
#define WD_XWAYLAND_TITLEBAR_HEIGHT    28u
#define WD_XWAYLAND_BUTTON_SIZE        18u
#define WD_XWAYLAND_BUTTON_MARGIN      5u
#define WD_XWAYLAND_BUTTON_GAP         5u

#define WD_ZSTD_LEVEL 1

#ifdef __cplusplus
}
#endif
