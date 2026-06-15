#pragma once

#include "waydisplay/wd_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_PROTOCOL_VERSION 15u

/*
 * Wire structs are intentionally host-endian for now. WayDisplay targets
 * same-endian Linux peers and compares integer values directly. Bump the
 * protocol version before changing struct layout or byte order.
 */
#define WD_TCP_MAGIC             0x54434457u
#define WD_UDP_TILE_ID_MTU_PROBE        0xffffu
#define WD_UDP_TILE_ID_THROUGHPUT_PROBE 0xfffeu

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
    WD_MSG_THROUGHPUT_PROBE_START  = 15,
    WD_MSG_THROUGHPUT_PROBE_RESULT = 16,
    WD_MSG_INPUT_CHANNEL_HELLO     = 17,
    WD_MSG_SELECTION_CHANNEL_HELLO = 18,
    WD_MSG_CLIENT_STATS            = 19,
    WD_MSG_LINK_PROBE_PING         = 20,
    WD_MSG_LINK_PROBE_PONG         = 21,
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
    WD_CURSOR_SHAPE_HIDDEN      = 10,

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
     * Target dirty-discovery/render cadence. 0 means server default.
     */
    uint16_t target_fps;

    /*
     * Requested remote display size.
     * 0x0 means server default/current size.
     */
    uint16_t desired_width;
    uint16_t desired_height;

    /*
     * Optional client-selected maximum UDP tile budget in KiB/s.
     * 0 means use the server throughput probe. Nonzero values act as a cap
     * and do not raise the server-selected throughput-probe ceiling.
     */
    uint32_t limited_udp_kib_per_second;
};

enum wd_server_capability {
    WD_SERVER_CAP_INPUT_CHANNEL     = 1u << 0,
    WD_SERVER_CAP_SELECTION_CHANNEL = 1u << 1,
};

struct wd_server_config_payload {
    uint8_t session_id;
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
    uint32_t capabilities;

    /* Conservative link-profile timers derived by the server's connect-time
     * probe. Values are milliseconds; 0 means use the compiled default. */
    uint16_t link_rtt_ms;
    uint16_t summary_retransmit_grace_ms;
    uint16_t retransmit_rerequest_ms;
    uint16_t retransmit_inflight_grace_ms;
    uint16_t tile_reassembly_timeout_ms;
    uint16_t active_summary_interval_ms;
    uint16_t clean_summary_interval_ms;
};

struct wd_link_probe_payload {
    uint8_t session_id;
    uint32_t sequence;
    uint64_t timestamp_ns;
};

struct wd_tile_generation_entry {
    uint16_t tile_id;
    uint64_t tile_generation;
    uint64_t tile_timestamp_ns;
};

enum wd_tile_summary_flags {
    WD_TILE_SUMMARY_DELTA = 1u << 0,
};

struct wd_tile_summary_payload_header {
    uint8_t session_id;
    uint64_t server_timestamp_ns;
    uint16_t tile_count;

    /* Bitmask from enum wd_tile_summary_flags. */
    uint8_t flags;
};

struct wd_retransmit_request_payload_header {
    uint8_t session_id;
    uint16_t request_count;
};

struct wd_retransmit_entry {
    uint16_t tile_id;

    /* 0 means retransmit the latest known generation for this tile. */
    uint64_t requested_generation;
};


struct wd_client_stats_payload {
    uint8_t session_id;

    uint64_t udp_packets_rx;
    uint64_t udp_bytes_rx;
    uint64_t udp_tiles_completed;
    uint64_t udp_completed_packets;
    uint64_t partial_tiles_timed_out;
    uint64_t udp_ignored_old_generation;
    uint64_t retx_requests_tx;

    /* Client-side UDP packet interarrival/jitter telemetry. */
    uint64_t udp_interarrival_samples;
    uint64_t udp_interarrival_sum_ns;
    uint64_t udp_interarrival_jitter_samples;
    uint64_t udp_interarrival_jitter_sum_ns;
    uint64_t udp_interarrival_max_ns;
};

struct wd_input_channel_hello_payload {
    uint8_t session_id;
};

struct wd_selection_channel_hello_payload {
    uint8_t session_id;
};

struct wd_keyboard_event_payload {
    uint8_t session_id;
    uint64_t client_timestamp_ns;
    uint64_t input_sequence;
    uint16_t evdev_key_code;
    uint8_t  pressed;
};

struct wd_pointer_event_payload {
    uint8_t session_id;
    uint64_t client_timestamp_ns;
    uint64_t input_sequence;

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

enum wd_tile_protocol {
    WD_TILE_UNCOMPRESSED_SINGLE         = 0,
    WD_TILE_UNCOMPRESSED_SINGLE_LATENCY = 1,
    WD_TILE_COMPRESSED_SINGLE           = 2,
    WD_TILE_COMPRESSED_SINGLE_LATENCY   = 3,
    WD_TILE_UNCOMPRESSED_MULTI          = 4,
    WD_TILE_UNCOMPRESSED_MULTI_LATENCY  = 5,
    WD_TILE_COMPRESSED_MULTI            = 6,
    WD_TILE_COMPRESSED_MULTI_LATENCY    = 7,
};

enum wd_tile_size {
    WD_TILE_128x64 = 0,
    WD_TILE_64x64  = 1,
    WD_TILE_32x32  = 2,
    WD_TILE_16x16  = 3,
};

enum wd_tile_flags {
    WD_TILE_NORMAL             = 0,
    WD_TILE_SUBTILE            = 1,
    WD_TILE_RETRANSMIT         = 2,
    WD_TILE_RETRANSMIT_SUBTILE = 3,
};

struct wd_udp_tile_packet_header_uncompressed_single {
    uint8_t  session_id;
    uint8_t  tile_protocol;
    uint8_t  tile_flags;
    uint8_t  tile_size;
    uint16_t tile_id;
    uint16_t payload_size;
    uint64_t tile_generation;
    uint64_t tile_timestamp_ns;
};

struct wd_udp_tile_packet_header_uncompressed_single_latency {
    uint8_t  session_id;
    uint8_t  tile_protocol;
    uint8_t  tile_flags;
    uint8_t  tile_size;
    uint16_t tile_id;
    uint16_t payload_size;
    uint64_t tile_generation;
    uint64_t tile_timestamp_ns;
    uint64_t input_sequence;
};

struct wd_udp_tile_packet_header_compressed_single {
    uint8_t  session_id;
    uint8_t  tile_protocol;
    uint8_t  tile_flags;
    uint8_t  tile_size;
    uint16_t tile_id;
    uint16_t payload_size;
    uint64_t tile_generation;
    uint16_t compressed_tile_size;
    uint64_t tile_timestamp_ns;
};

struct wd_udp_tile_packet_header_compressed_single_latency {
    uint8_t  session_id;
    uint8_t  tile_protocol;
    uint8_t  tile_flags;
    uint8_t  tile_size;
    uint16_t tile_id;
    uint16_t payload_size;
    uint64_t tile_generation;
    uint16_t compressed_tile_size;
    uint64_t tile_timestamp_ns;
    uint64_t input_sequence;
};

struct wd_udp_tile_packet_header_uncompressed_multi {
    uint8_t  session_id;
    uint8_t  tile_protocol;
    uint8_t  tile_flags;
    uint8_t  tile_size;
    uint16_t tile_id;
    uint8_t  tile_pkt_count;
    uint8_t  tile_pkt_id;
    uint16_t payload_size;
    uint64_t tile_generation;
    uint64_t tile_timestamp_ns;
};

struct wd_udp_tile_packet_header_uncompressed_multi_latency {
    uint8_t  session_id;
    uint8_t  tile_protocol;
    uint8_t  tile_flags;
    uint8_t  tile_size;
    uint16_t tile_id;
    uint8_t  tile_pkt_count;
    uint8_t  tile_pkt_id;
    uint16_t payload_size;
    uint64_t tile_generation;
    uint64_t tile_timestamp_ns;
    uint64_t input_sequence;
};

struct wd_udp_tile_packet_header_compressed_multi {
    uint8_t  session_id;
    uint8_t  tile_protocol;
    uint8_t  tile_flags;
    uint8_t  tile_size;
    uint16_t tile_id;
    uint8_t  tile_pkt_count;
    uint8_t  tile_pkt_id;
    uint16_t payload_size;
    uint64_t tile_generation;
    uint16_t compressed_tile_size;
    uint64_t tile_timestamp_ns;
};

struct wd_udp_tile_packet_header_compressed_multi_latency {
    uint8_t  session_id;
    uint8_t  tile_protocol;
    uint8_t  tile_flags;
    uint8_t  tile_size;
    uint16_t tile_id;
    uint8_t  tile_pkt_count;
    uint8_t  tile_pkt_id;
    uint16_t payload_size;
    uint64_t tile_generation;
    uint16_t compressed_tile_size;
    uint64_t tile_timestamp_ns;
    uint64_t input_sequence;
};

struct wd_udp_tile_packet_decoded {
    uint8_t  session_id;
    uint8_t  tile_protocol;
    uint8_t  tile_flags;
    uint8_t  tile_size;
    uint16_t tile_id;
    uint8_t  tile_pkt_count;
    uint8_t  tile_pkt_id;
    uint16_t payload_size;
    uint64_t tile_generation;
    uint16_t compressed_tile_size;
    uint64_t tile_timestamp_ns;
    uint64_t input_sequence;
    uint16_t header_size;
};


#define WD_UDP_TILE_HEADER_MAX_SIZE ((uint16_t)sizeof(struct wd_udp_tile_packet_header_compressed_multi_latency))
#define WD_UDP_TILE_HEADER_MIN_SIZE ((uint16_t)sizeof(struct wd_udp_tile_packet_header_uncompressed_single))

static inline uint16_t wd_udp_tile_header_size_for_protocol(uint8_t tile_protocol) {
    switch (tile_protocol)
    {
        case WD_TILE_UNCOMPRESSED_SINGLE:
            return (uint16_t)sizeof(struct wd_udp_tile_packet_header_uncompressed_single);
        case WD_TILE_UNCOMPRESSED_SINGLE_LATENCY:
            return (uint16_t)sizeof(struct wd_udp_tile_packet_header_uncompressed_single_latency);
        case WD_TILE_COMPRESSED_SINGLE:
            return (uint16_t)sizeof(struct wd_udp_tile_packet_header_compressed_single);
        case WD_TILE_COMPRESSED_SINGLE_LATENCY:
            return (uint16_t)sizeof(struct wd_udp_tile_packet_header_compressed_single_latency);
        case WD_TILE_UNCOMPRESSED_MULTI:
            return (uint16_t)sizeof(struct wd_udp_tile_packet_header_uncompressed_multi);
        case WD_TILE_UNCOMPRESSED_MULTI_LATENCY:
            return (uint16_t)sizeof(struct wd_udp_tile_packet_header_uncompressed_multi_latency);
        case WD_TILE_COMPRESSED_MULTI:
            return (uint16_t)sizeof(struct wd_udp_tile_packet_header_compressed_multi);
        case WD_TILE_COMPRESSED_MULTI_LATENCY:
            return (uint16_t)sizeof(struct wd_udp_tile_packet_header_compressed_multi_latency);
        default:
            return 0;
    }
}

static inline bool wd_tile_protocol_is_multi(uint8_t tile_protocol) {
    return tile_protocol == WD_TILE_UNCOMPRESSED_MULTI || tile_protocol == WD_TILE_UNCOMPRESSED_MULTI_LATENCY ||
           tile_protocol == WD_TILE_COMPRESSED_MULTI || tile_protocol == WD_TILE_COMPRESSED_MULTI_LATENCY;
}

static inline bool wd_tile_protocol_is_compressed(uint8_t tile_protocol) {
    return tile_protocol == WD_TILE_COMPRESSED_SINGLE || tile_protocol == WD_TILE_COMPRESSED_SINGLE_LATENCY ||
           tile_protocol == WD_TILE_COMPRESSED_MULTI || tile_protocol == WD_TILE_COMPRESSED_MULTI_LATENCY;
}

static inline bool wd_tile_protocol_has_latency(uint8_t tile_protocol) {
    return tile_protocol == WD_TILE_UNCOMPRESSED_SINGLE_LATENCY || tile_protocol == WD_TILE_COMPRESSED_SINGLE_LATENCY ||
           tile_protocol == WD_TILE_UNCOMPRESSED_MULTI_LATENCY || tile_protocol == WD_TILE_COMPRESSED_MULTI_LATENCY;
}

static inline uint8_t wd_tile_size_code_for_dimensions(uint16_t width, uint16_t height) {
    if (width == 128 && height == 64)
    {
        return WD_TILE_128x64;
    }
    if (width == 64 && height == 64)
    {
        return WD_TILE_64x64;
    }
    if (width == 32 && height == 32)
    {
        return WD_TILE_32x32;
    }
    if (width == 16 && height == 16)
    {
        return WD_TILE_16x16;
    }
    return WD_TILE_128x64;
}

static inline bool wd_tile_dimensions_for_size_code(uint8_t tile_size, uint16_t* out_width, uint16_t* out_height) {
    if (!out_width || !out_height)
    {
        return false;
    }

    switch (tile_size)
    {
        case WD_TILE_128x64:
            *out_width = 128;
            *out_height = 64;
            return true;
        case WD_TILE_64x64:
            *out_width = 64;
            *out_height = 64;
            return true;
        case WD_TILE_32x32:
            *out_width = 32;
            *out_height = 32;
            return true;
        case WD_TILE_16x16:
            *out_width = 16;
            *out_height = 16;
            return true;
        default:
            return false;
    }
}

static inline bool wd_udp_tile_packet_decode(const void* packet, size_t packet_size, struct wd_udp_tile_packet_decoded* out) {
    if (!packet || !out || packet_size < 4)
    {
        return false;
    }

    const uint8_t* bytes = (const uint8_t*)packet;
    uint8_t tile_protocol = bytes[1];
    uint16_t header_size = wd_udp_tile_header_size_for_protocol(tile_protocol);
    if (header_size == 0 || packet_size < header_size)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->session_id = bytes[0];
    out->tile_protocol = tile_protocol;
    out->tile_flags = bytes[2];
    out->tile_size = bytes[3];
    out->header_size = header_size;

    switch (tile_protocol)
    {
        case WD_TILE_UNCOMPRESSED_SINGLE: {
            struct wd_udp_tile_packet_header_uncompressed_single h;
            memcpy(&h, packet, sizeof(h));
            out->tile_id = h.tile_id;
            out->tile_pkt_count = 1;
            out->tile_pkt_id = 0;
            out->payload_size = h.payload_size;
            out->tile_generation = h.tile_generation;
            out->compressed_tile_size = h.payload_size;
            out->tile_timestamp_ns = h.tile_timestamp_ns;
            break;
        }
        case WD_TILE_UNCOMPRESSED_SINGLE_LATENCY: {
            struct wd_udp_tile_packet_header_uncompressed_single_latency h;
            memcpy(&h, packet, sizeof(h));
            out->tile_id = h.tile_id;
            out->tile_pkt_count = 1;
            out->tile_pkt_id = 0;
            out->payload_size = h.payload_size;
            out->tile_generation = h.tile_generation;
            out->compressed_tile_size = h.payload_size;
            out->tile_timestamp_ns = h.tile_timestamp_ns;
            out->input_sequence = h.input_sequence;
            break;
        }
        case WD_TILE_COMPRESSED_SINGLE: {
            struct wd_udp_tile_packet_header_compressed_single h;
            memcpy(&h, packet, sizeof(h));
            out->tile_id = h.tile_id;
            out->tile_pkt_count = 1;
            out->tile_pkt_id = 0;
            out->payload_size = h.payload_size;
            out->tile_generation = h.tile_generation;
            out->compressed_tile_size = h.compressed_tile_size;
            out->tile_timestamp_ns = h.tile_timestamp_ns;
            break;
        }
        case WD_TILE_COMPRESSED_SINGLE_LATENCY: {
            struct wd_udp_tile_packet_header_compressed_single_latency h;
            memcpy(&h, packet, sizeof(h));
            out->tile_id = h.tile_id;
            out->tile_pkt_count = 1;
            out->tile_pkt_id = 0;
            out->payload_size = h.payload_size;
            out->tile_generation = h.tile_generation;
            out->compressed_tile_size = h.compressed_tile_size;
            out->tile_timestamp_ns = h.tile_timestamp_ns;
            out->input_sequence = h.input_sequence;
            break;
        }
        case WD_TILE_UNCOMPRESSED_MULTI: {
            struct wd_udp_tile_packet_header_uncompressed_multi h;
            memcpy(&h, packet, sizeof(h));
            out->tile_id = h.tile_id;
            out->tile_pkt_count = h.tile_pkt_count;
            out->tile_pkt_id = h.tile_pkt_id;
            out->payload_size = h.payload_size;
            out->tile_generation = h.tile_generation;
            out->compressed_tile_size = h.payload_size;
            out->tile_timestamp_ns = h.tile_timestamp_ns;
            break;
        }
        case WD_TILE_UNCOMPRESSED_MULTI_LATENCY: {
            struct wd_udp_tile_packet_header_uncompressed_multi_latency h;
            memcpy(&h, packet, sizeof(h));
            out->tile_id = h.tile_id;
            out->tile_pkt_count = h.tile_pkt_count;
            out->tile_pkt_id = h.tile_pkt_id;
            out->payload_size = h.payload_size;
            out->tile_generation = h.tile_generation;
            out->compressed_tile_size = h.payload_size;
            out->tile_timestamp_ns = h.tile_timestamp_ns;
            out->input_sequence = h.input_sequence;
            break;
        }
        case WD_TILE_COMPRESSED_MULTI: {
            struct wd_udp_tile_packet_header_compressed_multi h;
            memcpy(&h, packet, sizeof(h));
            out->tile_id = h.tile_id;
            out->tile_pkt_count = h.tile_pkt_count;
            out->tile_pkt_id = h.tile_pkt_id;
            out->payload_size = h.payload_size;
            out->tile_generation = h.tile_generation;
            out->compressed_tile_size = h.compressed_tile_size;
            out->tile_timestamp_ns = h.tile_timestamp_ns;
            break;
        }
        case WD_TILE_COMPRESSED_MULTI_LATENCY: {
            struct wd_udp_tile_packet_header_compressed_multi_latency h;
            memcpy(&h, packet, sizeof(h));
            out->tile_id = h.tile_id;
            out->tile_pkt_count = h.tile_pkt_count;
            out->tile_pkt_id = h.tile_pkt_id;
            out->payload_size = h.payload_size;
            out->tile_generation = h.tile_generation;
            out->compressed_tile_size = h.compressed_tile_size;
            out->tile_timestamp_ns = h.tile_timestamp_ns;
            out->input_sequence = h.input_sequence;
            break;
        }
        default:
            return false;
    }

    return (size_t)out->header_size + (size_t)out->payload_size <= packet_size;
}

/*
 * Maximum IPv4 UDP payload is 65507 bytes. WayDisplay stores its
 * own tile packet header inside that UDP payload, so the tile data
 * budget must subtract WD_UDP_TILE_HEADER_MAX_SIZE too.
 */
#define WD_IPV4_HEADER_BYTES    20u
#define WD_UDP_HEADER_BYTES     8u
#define WD_IPV4_UDP_PAYLOAD_MAX 65507u
#define WD_UDP_TILE_PAYLOAD_MAX ((uint16_t)(WD_IPV4_UDP_PAYLOAD_MAX - WD_UDP_TILE_HEADER_MAX_SIZE))
#define WD_IPV4_MTU_TO_TILE_PAYLOAD(mtu)                                                                                                   \
    ((uint16_t)((mtu) - WD_IPV4_HEADER_BYTES - WD_UDP_HEADER_BYTES - WD_UDP_TILE_HEADER_MAX_SIZE))

struct wd_mtu_probe_start_payload {
    uint8_t session_id;
    uint16_t probe_count;
};

struct wd_mtu_probe_result_payload {
    uint8_t session_id;

    /*
     * Largest tile payload size received, excluding
     * the largest WayDisplay UDP tile packet header.
     */
    uint16_t max_udp_payload_received;
};

struct wd_throughput_probe_start_payload {
    uint8_t session_id;
    uint16_t probe_count;
    uint16_t payload_size;
    uint16_t duration_ms;
};

struct wd_throughput_probe_result_payload {
    uint8_t session_id;
    uint64_t bytes_received;
    uint32_t packets_received;
    uint16_t duration_ms;
};

#define WD_SELECTION_MIME_TEXT_UTF8  1u
#define WD_SELECTION_MIME_TEXT_PLAIN 2u
#define WD_SELECTION_MAX_TEXT_BYTES  (1024u * 1024u)

struct wd_selection_payload_header {
    uint8_t session_id;
    uint16_t mime_type;
    uint32_t data_size;
};

struct wd_cursor_shape_payload {
    uint8_t session_id;
    uint16_t shape;
};

struct wd_display_resize_payload {
    uint8_t session_id;
    uint16_t width;
    uint16_t height;
};

WD_PACKED_END

#undef WD_PACKED_BEGIN
#undef WD_PACKED_END

#if defined(__cplusplus)
static_assert(sizeof(struct wd_tcp_header) == 12, "unexpected wd_tcp_header size");
static_assert(WD_UDP_TILE_HEADER_MAX_SIZE == 36, "unexpected wd_udp_tile_packet_header size");
static_assert(sizeof(struct wd_client_hello_payload) == 12, "unexpected wd_client_hello_payload size");
static_assert(sizeof(struct wd_tile_generation_entry) == 18, "unexpected wd_tile_generation_entry size");
static_assert(sizeof(struct wd_tile_summary_payload_header) == 12, "unexpected wd_tile_summary_payload_header size");
static_assert(sizeof(struct wd_retransmit_request_payload_header) == 3, "unexpected wd_retransmit_request_payload_header size");
static_assert(sizeof(struct wd_keyboard_event_payload) == 20, "unexpected wd_keyboard_event_payload size");
static_assert(sizeof(struct wd_pointer_event_payload) == 33, "unexpected wd_pointer_event_payload size");
static_assert(sizeof(struct wd_mtu_probe_start_payload) == 3, "unexpected wd_mtu_probe_start_payload size");
static_assert(sizeof(struct wd_mtu_probe_result_payload) == 3, "unexpected wd_mtu_probe_result_payload size");
static_assert(sizeof(struct wd_throughput_probe_start_payload) == 7, "unexpected wd_throughput_probe_start_payload size");
static_assert(sizeof(struct wd_throughput_probe_result_payload) == 15, "unexpected wd_throughput_probe_result_payload size");
static_assert(sizeof(struct wd_retransmit_entry) == 10, "unexpected wd_retransmit_entry size");
static_assert(sizeof(struct wd_client_stats_payload) == 97, "unexpected wd_client_stats_payload size");
static_assert(sizeof(struct wd_input_channel_hello_payload) == 1, "unexpected wd_input_channel_hello_payload size");
static_assert(sizeof(struct wd_selection_channel_hello_payload) == 1, "unexpected wd_selection_channel_hello_payload size");
static_assert(sizeof(struct wd_selection_payload_header) == 7, "unexpected wd_selection_payload_header size");
static_assert(sizeof(struct wd_cursor_shape_payload) == 3, "unexpected wd_cursor_shape_payload size");
static_assert(sizeof(struct wd_display_resize_payload) == 5, "unexpected wd_display_resize_payload size");
#else
_Static_assert(sizeof(struct wd_tcp_header) == 12, "unexpected wd_tcp_header size");
_Static_assert(WD_UDP_TILE_HEADER_MAX_SIZE == 36, "unexpected wd_udp_tile_packet_header size");
_Static_assert(sizeof(struct wd_client_hello_payload) == 12, "unexpected wd_client_hello_payload size");
_Static_assert(sizeof(struct wd_tile_generation_entry) == 18, "unexpected wd_tile_generation_entry size");
_Static_assert(sizeof(struct wd_tile_summary_payload_header) == 12, "unexpected wd_tile_summary_payload_header size");
_Static_assert(sizeof(struct wd_retransmit_request_payload_header) == 3, "unexpected wd_retransmit_request_payload_header size");
_Static_assert(sizeof(struct wd_keyboard_event_payload) == 20, "unexpected wd_keyboard_event_payload size");
_Static_assert(sizeof(struct wd_pointer_event_payload) == 33, "unexpected wd_pointer_event_payload size");
_Static_assert(sizeof(struct wd_mtu_probe_start_payload) == 3, "unexpected wd_mtu_probe_start_payload size");
_Static_assert(sizeof(struct wd_mtu_probe_result_payload) == 3, "unexpected wd_mtu_probe_result_payload size");
_Static_assert(sizeof(struct wd_throughput_probe_start_payload) == 7, "unexpected wd_throughput_probe_start_payload size");
_Static_assert(sizeof(struct wd_throughput_probe_result_payload) == 15, "unexpected wd_throughput_probe_result_payload size");
_Static_assert(sizeof(struct wd_retransmit_entry) == 10, "unexpected wd_retransmit_entry size");
_Static_assert(sizeof(struct wd_client_stats_payload) == 97, "unexpected wd_client_stats_payload size");
_Static_assert(sizeof(struct wd_input_channel_hello_payload) == 1, "unexpected wd_input_channel_hello_payload size");
_Static_assert(sizeof(struct wd_selection_channel_hello_payload) == 1, "unexpected wd_selection_channel_hello_payload size");
_Static_assert(sizeof(struct wd_selection_payload_header) == 7, "unexpected wd_selection_payload_header size");
_Static_assert(sizeof(struct wd_cursor_shape_payload) == 3, "unexpected wd_cursor_shape_payload size");
_Static_assert(sizeof(struct wd_display_resize_payload) == 5, "unexpected wd_display_resize_payload size");
#endif

#ifdef __cplusplus
}
#endif
