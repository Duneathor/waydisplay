#pragma once

#include "waydisplay/wd_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_PROTOCOL_VERSION 1u

/*
 * Historical value used by the prototype/server.
 * Integer value is compared directly on same-endian Linux peers.
 */
#define WD_TCP_MAGIC             0x54434457u
#define WD_UDP_TILE_ID_MTU_PROBE 0xffffu

enum wd_message_type {
    WD_MSG_CLIENT_HELLO            = 1,
    WD_MSG_SERVER_CONFIG           = 2,
    WD_MSG_TILE_GENERATION_SUMMARY = 3,
    WD_MSG_RETRANSMIT_REQUEST      = 4,
    WD_MSG_KEYBOARD_KEY            = 5,
    WD_MSG_POINTER_EVENT           = 6,
    WD_MSG_MTU_PROBE_START         = 7,
    WD_MSG_MTU_PROBE_RESULT        = 8,
    WD_MSG_CLIPBOARD_SET           = 9,
    WD_MSG_CLIPBOARD_REQUEST       = 10,
    WD_MSG_PRIMARY_SET             = 11,
    WD_MSG_PRIMARY_REQUEST         = 12,
    WD_MSG_CURSOR_SHAPE            = 13,
    WD_MSG_DISPLAY_RESIZE          = 14,
    WD_MSG_ERROR                   = 255,
};

enum wd_pointer_event_type {
    WD_POINTER_EVENT_MOTION = 1,
    WD_POINTER_EVENT_BUTTON = 2,
    WD_POINTER_EVENT_AXIS   = 3,
};

enum wd_pointer_button_state {
    WD_POINTER_BUTTON_RELEASED = 0,
    WD_POINTER_BUTTON_PRESSED  = 1,
};

enum wd_pointer_axis {
    WD_POINTER_AXIS_VERTICAL   = 0,
    WD_POINTER_AXIS_HORIZONTAL = 1,
};

enum wd_stream_mode {
    /*
     * Send all dirty tiles whenever the compositor reports activity.
     * Best quality, worst bandwidth.
     */
    WD_STREAM_MODE_FULL = 1,

    /*
     * Send dirty tiles at a capped frame cadence.
     * Example: target_fps = 30.
     */
    WD_STREAM_MODE_PARTIAL = 2,

    /*
     * Send at most max_tiles_per_second.
     * Tile budget is the bottleneck, not frame rate.
     */
    WD_STREAM_MODE_LIMITED = 3,

    WD_STREAM_MODE_LIVE = 4,
};

enum wd_pixel_format {
    WD_PIXEL_FORMAT_XRGB8888 = 1,
};

enum wd_compression_mode {
    WD_COMPRESSION_ZSTD = 1,
};

enum wd_cursor_shape {
    WD_CURSOR_SHAPE_DEFAULT     = 0,
    WD_CURSOR_SHAPE_POINTER     = 1,
    WD_CURSOR_SHAPE_TEXT        = 2,
    WD_CURSOR_SHAPE_MOVE        = 3,
    WD_CURSOR_SHAPE_EW_RESIZE   = 4,
    WD_CURSOR_SHAPE_NS_RESIZE   = 5,
    WD_CURSOR_SHAPE_NWSE_RESIZE = 6,
    WD_CURSOR_SHAPE_NESW_RESIZE = 7,
    WD_CURSOR_SHAPE_WAIT        = 8,
    WD_CURSOR_SHAPE_NOT_ALLOWED = 9,

    WD_CURSOR_SHAPE_COUNT,
};

#if defined(_MSC_VER)
#define WD_PACKED_BEGIN __pragma(pack(push, 1))
#define WD_PACKED_END   __pragma(pack(pop))
#else
#define WD_PACKED_BEGIN _Pragma("pack(push, 1)")
#define WD_PACKED_END   _Pragma("pack(pop)")
#endif

WD_PACKED_BEGIN

struct wd_tcp_header {
    uint32_t magic;
    uint16_t protocol_version;
    uint16_t message_type;
    uint32_t payload_size;
};

struct wd_client_hello_payload {
    uint16_t client_udp_port;

    /*
     * enum wd_stream_mode
     */
    uint16_t stream_mode;

    /*
     * Used by WD_STREAM_MODE_PARTIAL.
     * 0 means server default.
     */
    uint16_t target_fps;

    /*
     * Requested remote display size.
     * 0x0 means server default/current size.
     */
    uint16_t desired_width;
    uint16_t desired_height;

    /*
     * Keep the packed hello payload 32-bit aligned and reserved for future use.
     */
    uint16_t reserved0;

    /*
     * Used by WD_STREAM_MODE_LIMITED.
     * 0 means server default.
     */
    uint32_t max_tiles_per_second;
};

struct wd_server_config_payload {
    uint32_t session_id;
    uint16_t width;
    uint16_t height;
    uint16_t tile_width;
    uint16_t tile_height;
    uint16_t tiles_x;
    uint16_t tiles_y;
    uint16_t total_tiles;
    uint16_t pixel_format;
    uint16_t compression_mode;
    uint16_t zstd_level;
    uint16_t udp_payload_target;
};

struct wd_tile_generation_entry {
    uint16_t tile_id;
    uint16_t reserved;
    uint64_t tile_generation;
    uint64_t tile_timestamp_ns;
};

struct wd_tile_summary_payload_header {
    uint32_t session_id;
    uint64_t server_timestamp_ns;
    uint16_t tile_count;
    uint16_t reserved;
};

struct wd_retransmit_request_payload_header {
    uint32_t session_id;
    uint16_t request_count;
    uint16_t reserved;
};

struct wd_retransmit_entry {
    uint16_t tile_id;
    uint16_t reserved;
    uint64_t desired_generation;
};

struct wd_keyboard_event_payload {
    uint32_t session_id;
    uint64_t client_timestamp_ns;
    uint16_t evdev_key_code;
    uint8_t  pressed;
    uint8_t  reserved;
};

struct wd_pointer_event_payload {
    uint32_t session_id;
    uint64_t client_timestamp_ns;

    /*
     * enum wd_pointer_event_type
     */
    uint16_t event_type;

    /*
     * Current pointer position in WayDisplay framebuffer coordinates.
     */
    uint16_t x;
    uint16_t y;

    /*
     * Linux input button code.
     * BTN_LEFT=0x110, BTN_RIGHT=0x111, BTN_MIDDLE=0x112, etc.
     */
    uint16_t button;

    /*
     * enum wd_pointer_button_state
     */
    uint8_t button_state;

    /*
     * enum wd_pointer_axis
     */
    uint8_t axis;

    /*
     * Wheel/scroll value.
     */
    int32_t axis_value;

    /*
     * Bitmask from client.
     * bit 0 = Alt
     * bit 1 = Shift
     * bit 2 = Ctrl
     * bit 3 = Super
     */
    uint16_t modifiers;
};

struct wd_udp_tile_packet_header {
    uint16_t tile_id;
    uint16_t tile_pkt_count;
    uint16_t tile_pkt_id;
    uint16_t payload_size;
    uint64_t tile_generation;
    uint32_t compressed_tile_size;

    /*
     * Server monotonic timestamp for when this tile generation was produced.
     * Used for best-effort same-host latency telemetry.
     */
    uint64_t tile_timestamp_ns;
};

/*
 * Maximum IPv4 UDP payload is 65507 bytes. WayDisplay stores its
 * own tile packet header inside that UDP payload, so the tile data
 * budget must subtract sizeof(struct wd_udp_tile_packet_header) too.
 */
#define WD_IPV4_HEADER_BYTES    20u
#define WD_UDP_HEADER_BYTES     8u
#define WD_IPV4_UDP_PAYLOAD_MAX 65507u
#define WD_UDP_TILE_PAYLOAD_MAX \
    ((uint16_t)(WD_IPV4_UDP_PAYLOAD_MAX - sizeof(struct wd_udp_tile_packet_header)))
#define WD_IPV4_MTU_TO_TILE_PAYLOAD(mtu) \
    ((uint16_t)((mtu) - WD_IPV4_HEADER_BYTES - WD_UDP_HEADER_BYTES - sizeof(struct wd_udp_tile_packet_header)))

struct wd_mtu_probe_start_payload {
    uint32_t session_id;
    uint16_t probe_count;
    uint16_t reserved;
};

struct wd_mtu_probe_result_payload {
    uint32_t session_id;

    /*
     * Largest tile payload size received, excluding
     * struct wd_udp_tile_packet_header.
     */
    uint16_t max_udp_payload_received;

    uint16_t reserved;
};

#define WD_SELECTION_MIME_TEXT_UTF8  1u
#define WD_SELECTION_MIME_TEXT_PLAIN 2u
#define WD_SELECTION_MAX_TEXT_BYTES  (1024u * 1024u)

struct wd_selection_payload_header {
    uint32_t session_id;
    uint16_t mime_type;
    uint16_t reserved;
    uint32_t data_size;
};

struct wd_cursor_shape_payload {
    uint32_t session_id;
    uint16_t shape;
    uint16_t reserved;
};

struct wd_display_resize_payload {
    uint32_t session_id;
    uint16_t width;
    uint16_t height;
};

WD_PACKED_END

#undef WD_PACKED_BEGIN
#undef WD_PACKED_END

#if defined(__cplusplus)
static_assert(sizeof(struct wd_tcp_header) == 12, "unexpected wd_tcp_header size");
static_assert(sizeof(struct wd_udp_tile_packet_header) == 28, "unexpected wd_udp_tile_packet_header size");
static_assert(sizeof(struct wd_client_hello_payload) == 16, "unexpected wd_client_hello_payload size");
static_assert(sizeof(struct wd_pointer_event_payload) == 28, "unexpected wd_pointer_event_payload size");
static_assert(sizeof(struct wd_mtu_probe_start_payload) == 8, "unexpected wd_mtu_probe_start_payload size");
static_assert(sizeof(struct wd_mtu_probe_result_payload) == 8, "unexpected wd_mtu_probe_result_payload size");
static_assert(sizeof(struct wd_selection_payload_header) == 12, "unexpected wd_selection_payload_header size");
static_assert(sizeof(struct wd_cursor_shape_payload) == 8, "unexpected wd_cursor_shape_payload size");
static_assert(sizeof(struct wd_display_resize_payload) == 8, "unexpected wd_display_resize_payload size");
#else
_Static_assert(sizeof(struct wd_tcp_header) == 12, "unexpected wd_tcp_header size");
_Static_assert(sizeof(struct wd_udp_tile_packet_header) == 28, "unexpected wd_udp_tile_packet_header size");
_Static_assert(sizeof(struct wd_client_hello_payload) == 16, "unexpected wd_client_hello_payload size");
_Static_assert(sizeof(struct wd_pointer_event_payload) == 28, "unexpected wd_pointer_event_payload size");
_Static_assert(sizeof(struct wd_mtu_probe_start_payload) == 8, "unexpected wd_mtu_probe_start_payload size");
_Static_assert(sizeof(struct wd_mtu_probe_result_payload) == 8, "unexpected wd_mtu_probe_result_payload size");
_Static_assert(sizeof(struct wd_selection_payload_header) == 12, "unexpected wd_selection_payload_header size");
_Static_assert(sizeof(struct wd_cursor_shape_payload) == 8, "unexpected wd_cursor_shape_payload size");
_Static_assert(sizeof(struct wd_display_resize_payload) == 8, "unexpected wd_display_resize_payload size");
#endif

#ifdef __cplusplus
}
#endif
