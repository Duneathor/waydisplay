#pragma once

#include <stdint.h>

#include "waydisplay/wd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WD_PROTOCOL_VERSION 1u

/*
 * Historical value used by the prototype/server.
 * Integer value is compared directly on same-endian Linux peers.
 */
#define WD_TCP_MAGIC 0x54434457u

enum wd_message_type {
    WD_MSG_CLIENT_HELLO = 1,
    WD_MSG_SERVER_CONFIG = 2,
    WD_MSG_TILE_GENERATION_SUMMARY = 3,
    WD_MSG_RETRANSMIT_REQUEST = 4,
    WD_MSG_KEYBOARD_KEY = 5,
    WD_MSG_ERROR = 255,
};

enum wd_pixel_format {
    WD_PIXEL_FORMAT_XRGB8888 = 1,
};

enum wd_compression_mode {
    WD_COMPRESSION_ZSTD = 1,
};

#if defined(_MSC_VER)
#define WD_PACKED_BEGIN __pragma(pack(push, 1))
#define WD_PACKED_END __pragma(pack(pop))
#else
#define WD_PACKED_BEGIN _Pragma("pack(push, 1)")
#define WD_PACKED_END _Pragma("pack(pop)")
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
    uint16_t reserved;
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
    uint8_t pressed;
    uint8_t reserved;
};

struct wd_udp_tile_packet_header {
    uint16_t tile_id;
    uint16_t tile_pkt_count;
    uint16_t tile_pkt_id;
    uint16_t payload_size;
    uint64_t tile_generation;
    uint32_t compressed_tile_size;
};

WD_PACKED_END

#undef WD_PACKED_BEGIN
#undef WD_PACKED_END

#if defined(__cplusplus)
static_assert(sizeof(struct wd_tcp_header) == 12, "unexpected wd_tcp_header size");
static_assert(sizeof(struct wd_udp_tile_packet_header) == 20,
              "unexpected wd_udp_tile_packet_header size");
#else
_Static_assert(sizeof(struct wd_tcp_header) == 12, "unexpected wd_tcp_header size");
_Static_assert(sizeof(struct wd_udp_tile_packet_header) == 20,
               "unexpected wd_udp_tile_packet_header size");
#endif

#ifdef __cplusplus
}
#endif
