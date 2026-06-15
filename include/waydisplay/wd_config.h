#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Display/tile defaults. */
#define WD_DISPLAY_WIDTH  1664u
#define WD_DISPLAY_HEIGHT 1024u

#define WD_TILE_WIDTH  16u
#define WD_TILE_HEIGHT 16u

#define WD_BASE_TILE_WIDTH  16u
#define WD_BASE_TILE_HEIGHT 16u

#define WD_TILES_X     (((WD_DISPLAY_WIDTH) + (WD_TILE_WIDTH) - 1u) / (WD_TILE_WIDTH))
#define WD_TILES_Y     (((WD_DISPLAY_HEIGHT) + (WD_TILE_HEIGHT) - 1u) / (WD_TILE_HEIGHT))
#define WD_TOTAL_TILES (WD_TILES_X * WD_TILES_Y)

#define WD_BYTES_PER_PIXEL 4u

#define WD_FRAMEBUFFER_PIXELS ((uint32_t)(WD_DISPLAY_WIDTH * WD_DISPLAY_HEIGHT))
#define WD_FRAMEBUFFER_BYTES  ((uint32_t)(WD_FRAMEBUFFER_PIXELS * WD_BYTES_PER_PIXEL))

#define WD_UNCOMPRESSED_TILE_BYTES ((uint32_t)(WD_TILE_WIDTH * WD_TILE_HEIGHT * WD_BYTES_PER_PIXEL))

/* Network defaults and limits. */
#define WD_DEFAULT_TCP_PORT              5000u
#define WD_TCP_HANDSHAKE_TIMEOUT_MS      3000L
#define WD_TCP_CONNECTED_SEND_TIMEOUT_MS 3000L
#define WD_TCP_MAX_PAYLOAD_SIZE          (2u * 1024u * 1024u)
#define WD_UDP_PAYLOAD_TARGET            1200u
#define WD_UDP_MAX_PAYLOAD               65487u
#define WD_UDP_SOCKET_BUFFER_BYTES       (16 * 1024 * 1024)
#define WD_MIN_PROBED_UDP_PAYLOAD        512u
#define WD_THROUGHPUT_PROBE_TARGET_BYTES (8u * 1024u * 1024u)
#define WD_THROUGHPUT_PROBE_DURATION_MS  750u

/* Tile generation summary cadence.
 *
 * Full summaries are large at high resolutions, so they are sent only when
 * summary state is reset (connect/resize/reconfigure) and as a rare sanity
 * refresh. Delta summaries carry normal generation updates.
 */
#define WD_GENERATION_SUMMARY_FULL_SANITY_INTERVAL_NS 60000000000ull
#define WD_GENERATION_SUMMARY_DELTA_INTERVAL_NS       50000000ull
#define WD_GENERATION_SUMMARY_CLEAN_DELTA_INTERVAL_NS 200000000ull

/* Link-profile timing.  These values are intentionally conservative: very
 * low measured RTTs keep today's LAN-friendly floors instead of shrinking
 * repair timers too aggressively, while high-latency links are capped around
 * a practical worst-case terrestrial RTT. */
#define WD_LINK_RTT_MIN_NS       30000000ull
#define WD_LINK_RTT_DEFAULT_NS   100000000ull
#define WD_LINK_RTT_MAX_NS       800000000ull

#define WD_LINK_SUMMARY_GRACE_MIN_NS       150000000ull
#define WD_LINK_SUMMARY_GRACE_DEFAULT_NS   150000000ull
#define WD_LINK_SUMMARY_GRACE_MAX_NS       1200000000ull

#define WD_LINK_RETRANSMIT_REREQUEST_MIN_NS       250000000ull
#define WD_LINK_RETRANSMIT_REREQUEST_DEFAULT_NS   250000000ull
#define WD_LINK_RETRANSMIT_REREQUEST_MAX_NS       1200000000ull

#define WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS       250000000ull
#define WD_LINK_RETRANSMIT_INFLIGHT_DEFAULT_NS   250000000ull
#define WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS       1200000000ull

#define WD_LINK_TILE_REASSEMBLY_MIN_NS       150000000ull
#define WD_LINK_TILE_REASSEMBLY_DEFAULT_NS   250000000ull
#define WD_LINK_TILE_REASSEMBLY_MAX_NS       900000000ull

#define WD_LINK_ACTIVE_SUMMARY_INTERVAL_MIN_NS       50000000ull
#define WD_LINK_ACTIVE_SUMMARY_INTERVAL_DEFAULT_NS   WD_GENERATION_SUMMARY_DELTA_INTERVAL_NS
#define WD_LINK_ACTIVE_SUMMARY_INTERVAL_MAX_NS       250000000ull

#define WD_LINK_CLEAN_SUMMARY_INTERVAL_MIN_NS       200000000ull
#define WD_LINK_CLEAN_SUMMARY_INTERVAL_DEFAULT_NS   WD_GENERATION_SUMMARY_CLEAN_DELTA_INTERVAL_NS
#define WD_LINK_CLEAN_SUMMARY_INTERVAL_MAX_NS       1000000000ull

/* Stream policy defaults. */
#define WD_DEFAULT_PARTIAL_FPS                     60u
#define WD_MAX_REASONABLE_FPS                      120u
#define WD_STREAM_TOKEN_BURST_DIVISOR              4u
#define WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND (1024ull * 1024ull)
#define WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND     (25ull * 1024ull)
#define WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND     (1000ull * 1024ull * 1024ull * 1024ull)
#define WD_LIMITED_MODE_THROUGHPUT_SAFETY_PERCENT    85u

/* Stream link health policy.  Tile size is not adapted globally: each dirty
 * tile is encoded at the largest supported wire size that satisfies the
 * current packet/budget/loss rules. */
#define WD_WIRE_TILE_MAX_WIDTH                        128u
#define WD_WIRE_TILE_MAX_HEIGHT                       64u
#define WD_STREAM_LINK_LOSS_SECONDS_TO_DECREASE       1u
#define WD_STREAM_LINK_GOOD_SECONDS_TO_INCREASE       2u
#define WD_STREAM_RATE_DECREASE_PERCENT               85u
#define WD_STREAM_RATE_PRESSURE_DECREASE_PERCENT      70u
#define WD_STREAM_RATE_INCREASE_PERCENT               112u
#define WD_STREAM_RATE_INCREASE_MIN_BYTES             (64ull * 1024ull)
#define WD_STREAM_CLIENT_COMPLETION_MIN_PACKETS       4u
#define WD_STREAM_CLIENT_COMPLETION_LOSS_PERCENT      35u
#define WD_STREAM_MULTIPACKET_LOSS_COOLDOWN_SECONDS   2u

#define WD_STREAM_FPS_MIN                            5u
#define WD_STREAM_FPS_DECREASE_PERCENT               85u
#define WD_STREAM_FPS_PRESSURE_DECREASE_PERCENT      70u
#define WD_STREAM_FPS_INCREASE_PERCENT               110u
#define WD_STREAM_FPS_GOOD_SECONDS_TO_INCREASE       3u

/* Client interaction/render-loop tuning. */
#define WD_CLIENT_DEFAULT_TARGET_FPS           WD_DEFAULT_PARTIAL_FPS
#define WD_CLIENT_STATS_INTERVAL_NS            1000000000ull
#define WD_CLIENT_RESIZE_DEBOUNCE_NS           150000000ull
#define WD_CLIENT_FRAME_DELAY_MS               8
#define WD_CLIENT_MIN_WINDOW_WIDTH             64
#define WD_CLIENT_MIN_WINDOW_HEIGHT            64
#define WD_CLIENT_MAX_DIMENSION                16384u
#define WD_CLIENT_MAX_FRAMEBUFFER_BYTES        (512ull * 1024ull * 1024ull)
#define WD_CLIENT_WHEEL_AXIS_STEP              60
#define WD_CLIENT_CONTEXT_MENU_PADDING_X       8
#define WD_CLIENT_CONTEXT_MENU_PADDING_Y       7
#define WD_CLIENT_CONTEXT_MENU_ITEM_HEIGHT     20
#define WD_CLIENT_CONTEXT_MENU_WIDTH           184
#define WD_CLIENT_CONTEXT_MENU_TEXT_SCALE      1
#define WD_CLIENT_CONTEXT_MENU_TEXT_X          8
#define WD_CLIENT_CONTEXT_MENU_TEXT_Y          6

/* Server interaction tuning. */
#define WD_FALLBACK_MOVE_ZONE_HEIGHT       32.0
#define WD_RESIZE_EDGE_ZONE                8.0
#define WD_MIN_WINDOW_WIDTH                120u
#define WD_MIN_WINDOW_HEIGHT               80u
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
