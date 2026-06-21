#pragma once

#include "waydisplay/wd_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_PROTOCOL_VERSION 39u

/*
 * WayDisplay peers are little-endian Linux systems and transmit packed
 * integer fields directly. Protocol versions still change whenever a wire
 * structure changes.
 */
#define WD_TCP_MAGIC             0x54434457u
#define WD_UDP_TILE_ID_MTU_PROBE        0xffffu
#define WD_UDP_TILE_ID_THROUGHPUT_PROBE 0xfffeu

enum wd_message_type {
    WD_MSG_CLIENT_HELLO            = 1,
    WD_MSG_SERVER_CONFIG           = 2,
    WD_MSG_TILE_GENERATION_SUMMARY = 3,
    WD_MSG_TILE_REPAIR_REQUEST     = 4,
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
    WD_MSG_VIDEO_CHANNEL_HELLO     = 22,
    WD_MSG_VIDEO_FRAME             = 23,
    WD_MSG_CONFIG_APPLIED          = 24,
    WD_MSG_AUDIO_CHANNEL_HELLO     = 25,
    WD_MSG_AUDIO_CONFIG            = 26,
    WD_MSG_AUDIO_PACKET            = 27,
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

enum wd_client_capability {
    WD_CLIENT_CAP_VIDEO_STREAM = 1u << 0,
    WD_CLIENT_CAP_AUDIO_STREAM = 1u << 1,
};
#define WD_CLIENT_CAP_MASK (WD_CLIENT_CAP_VIDEO_STREAM | WD_CLIENT_CAP_AUDIO_STREAM)

enum wd_video_codec {
    WD_VIDEO_CODEC_H265 = 1u << 0,
    WD_VIDEO_CODEC_H264 = 1u << 1,
};
#define WD_VIDEO_CODEC_MASK (WD_VIDEO_CODEC_H265 | WD_VIDEO_CODEC_H264)

enum wd_video_transport {
    WD_VIDEO_TRANSPORT_TCP = 1,
};

enum wd_audio_codec {
    WD_AUDIO_CODEC_OPUS = 1u << 0,
};
#define WD_AUDIO_CODEC_MASK WD_AUDIO_CODEC_OPUS

enum wd_audio_transport {
    WD_AUDIO_TRANSPORT_TCP = 1,
};

#define WD_AUDIO_SAMPLE_RATE_DEFAULT       48000u
#define WD_AUDIO_CHANNELS_MAX              2u
#define WD_AUDIO_FRAME_SAMPLES_DEFAULT     960u
#define WD_AUDIO_TARGET_LATENCY_MS_DEFAULT 60u
#define WD_AUDIO_TARGET_LATENCY_MS_MIN     20u
#define WD_AUDIO_TARGET_LATENCY_MS_MAX     500u
#define WD_AUDIO_BITRATE_DEFAULT           128000u
#define WD_AUDIO_PACKET_MAX_PAYLOAD_BYTES  16384u

enum wd_audio_packet_flags {
    WD_AUDIO_PACKET_DISCONTINUITY = 1u << 0,
    WD_AUDIO_PACKET_END_OF_STREAM = 1u << 1,
};
#define WD_AUDIO_PACKET_FLAG_MASK (WD_AUDIO_PACKET_DISCONTINUITY | WD_AUDIO_PACKET_END_OF_STREAM)

enum wd_video_mode {
    WD_VIDEO_MODE_AUTO  = 0,
    WD_VIDEO_MODE_OFF   = 1,
    WD_VIDEO_MODE_FORCE = 2,
};

#define WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT 60u
#define WD_VIDEO_MIN_DIRTY_PERCENT_MAX     100u
#define WD_VIDEO_ENTER_SECONDS_DEFAULT     3u
#define WD_VIDEO_ENTER_SECONDS_MAX         60u
#define WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT 30u
#define WD_VIDEO_EXIT_DIRTY_PERCENT_MAX     100u
#define WD_VIDEO_EXIT_SECONDS_DEFAULT       30u
#define WD_VIDEO_EXIT_SECONDS_MAX           300u


enum wd_video_frame_flags {
    WD_VIDEO_FRAME_CONFIG        = 1u << 0,
    WD_VIDEO_FRAME_KEYFRAME      = 1u << 1,
    WD_VIDEO_FRAME_RESIZE        = 1u << 2,
    WD_VIDEO_FRAME_END_OF_STREAM = 1u << 3,
};
#define WD_VIDEO_FRAME_FLAG_MASK \
    (WD_VIDEO_FRAME_CONFIG | WD_VIDEO_FRAME_KEYFRAME | WD_VIDEO_FRAME_RESIZE | WD_VIDEO_FRAME_END_OF_STREAM)

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
     * Requested remote capture cadence cap. 0 means server default.
     * The client may present repaired/tile batches at the same cap locally,
     * but compositor output refresh and local presentation are separate concepts.
     */
    uint16_t requested_capture_fps;

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

    /* Bitmask from enum wd_client_capability. */
    uint32_t capabilities;

    /* Bitmask from enum wd_video_codec. 0 means no video-stream codec. */
    uint32_t video_codecs;

    /* Preferred enum wd_video_transport value for a future video stream. */
    uint16_t video_transport;

    /* enum wd_video_mode: auto/off/force. */
    uint8_t video_mode;

    /* Dirty coverage threshold for auto video-mode entry. 0 means default. */
    uint8_t video_min_dirty_percent;

    /* Seconds the auto video criteria must remain stable. 0 means default. */
    uint16_t video_enter_seconds;

    /* Target video encoder bitrate in KiB/s. 0 means derive from link budget. */
    uint32_t video_bitrate_kib_per_second;

    /* Dirty coverage threshold for auto video-mode exit. 0 means default. */
    uint8_t video_exit_dirty_percent;

    /* Reserved for future video-policy byte flags. Must be zero. */
    uint8_t video_reserved;

    /* Seconds dirty coverage must stay below exit threshold. 0 means default. */
    uint16_t video_exit_seconds;

    /* Audio capability negotiation. All fields must be zero when disabled. */
    uint32_t audio_codecs;
    uint16_t audio_transport;
    uint8_t  audio_max_channels;
    uint8_t  audio_reserved;
    uint16_t audio_target_latency_ms;
};

enum wd_server_capability {
    WD_SERVER_CAP_INPUT_CHANNEL     = 1u << 0,
    WD_SERVER_CAP_SELECTION_CHANNEL = 1u << 1,
    WD_SERVER_CAP_VIDEO_STREAM      = 1u << 2,
    WD_SERVER_CAP_AUDIO_STREAM      = 1u << 3,
};
#define WD_SERVER_CAP_MASK \
    (WD_SERVER_CAP_INPUT_CHANNEL | WD_SERVER_CAP_SELECTION_CHANNEL | WD_SERVER_CAP_VIDEO_STREAM | \
     WD_SERVER_CAP_AUDIO_STREAM)

struct wd_server_config_payload {
    /* Stable for the lifetime of one control/UDP transport connection. */
    uint8_t session_id;
    uint64_t connection_token;

    /* Advances whenever configuration fields change. The server does not send
     * new-configuration UDP tiles until WD_MSG_CONFIG_APPLIED acknowledges it. */
    uint64_t config_epoch;

    /* Advances on framebuffer/stream ownership invalidation. UDP and video
     * payloads from older epochs must be discarded, including packets that
     * were already in flight when a resize began. */
    uint64_t content_epoch;
    uint16_t server_udp_port;
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

    /* Negotiated video-stream parameters. Valid only with WD_SERVER_CAP_VIDEO_STREAM. */
    uint32_t video_codecs;
    uint16_t video_transport;

    /* Shared zero-based media clock and negotiated audio parameters. */
    uint64_t media_clock_id;
    uint32_t audio_codec;
    uint16_t audio_transport;
    uint32_t audio_sample_rate;
    uint8_t  audio_channels;
    uint8_t  audio_reserved;
    uint16_t audio_frame_samples;
    uint16_t audio_target_latency_ms;
    uint32_t audio_bitrate;

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
    uint64_t connection_token;
    uint32_t sequence;
    uint64_t timestamp_ns;
};

struct wd_tile_generation_entry {
    uint16_t tile_id;
    uint64_t tile_generation;
};

enum wd_tile_summary_flags {
    WD_TILE_SUMMARY_DELTA = 1u << 0,
};
#define WD_TILE_SUMMARY_FLAG_MASK WD_TILE_SUMMARY_DELTA

struct wd_tile_summary_payload_header {
    uint8_t session_id;
    uint64_t connection_token;
    uint64_t content_epoch;
    uint64_t server_timestamp_ns;
    uint16_t tile_count;

    /* Bitmask from enum wd_tile_summary_flags. */
    uint8_t flags;
};

struct wd_tile_repair_request_payload_header {
    uint8_t session_id;
    uint64_t connection_token;
    uint64_t content_epoch;
    uint16_t request_count;
};

struct wd_tile_repair_entry {
    uint16_t tile_id;

    /* 0 means repair using the latest server generation for this tile. */
    uint64_t requested_generation;
};


enum wd_client_stats_flags {
    WD_CLIENT_STATS_RENDER_VISIBLE = 1u << 0,
};
#define WD_CLIENT_STATS_FLAG_MASK WD_CLIENT_STATS_RENDER_VISIBLE

struct wd_client_stats_payload {
    uint8_t session_id;
    uint64_t connection_token;
    uint32_t flags;

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

    /* Client-side render/present telemetry for sender adaptation.
     * render_frames counts presentations containing a remote tile/video update,
     * not local-only window redraws. */
    uint64_t render_frames;
    uint64_t present_samples;
    uint64_t present_sum_ns;
    uint64_t present_max_ns;
    uint64_t input_present_samples;
    uint64_t input_present_sum_ns;

    /* Client-side video TCP/decode telemetry. */
    uint64_t video_frames_rx;
    uint64_t video_bytes_rx;
    uint64_t video_frames_decoded;
    uint64_t video_frames_presented;
    uint64_t video_decode_failed;
    uint64_t video_publish_failed;
    uint64_t video_control_frames_rx;
    uint64_t video_need_keyframe_drops;
    uint64_t video_decoder_resets;
    uint64_t video_decode_samples;
    uint64_t video_decode_sum_ns;

    uint64_t video_messages_rx;
    uint64_t video_data_frames_rx;
    uint64_t video_invalid_frames_rx;
    uint64_t video_stale_frames_dropped;
    uint64_t video_last_frame_id_rx;
    uint64_t video_last_frame_id_presented;
    uint64_t video_present_latency_samples;
    uint64_t video_present_latency_sum_ns;

    /* Client-side audio receive/playback and A/V scheduling telemetry. */
    uint64_t audio_messages_rx;
    uint64_t audio_packets_rx;
    uint64_t audio_bytes_rx;
    uint64_t audio_decode_failed;
    uint64_t audio_discontinuities;
    uint64_t audio_late_drops;
    uint64_t audio_underflows;
    uint64_t video_audio_sync_holds;
    uint64_t video_audio_sync_drops;
};

struct wd_input_channel_hello_payload {
    uint8_t session_id;
    uint64_t connection_token;
};

struct wd_selection_channel_hello_payload {
    uint8_t session_id;
    uint64_t connection_token;
};

struct wd_video_channel_hello_payload {
    uint8_t session_id;
    uint64_t connection_token;
    uint32_t video_codecs;
    uint16_t video_transport;
};

struct wd_audio_channel_hello_payload {
    uint8_t session_id;
    uint64_t connection_token;
    uint32_t audio_codecs;
    uint16_t audio_transport;
};

struct wd_audio_config_payload {
    uint8_t  session_id;
    uint64_t connection_token;
    uint64_t audio_epoch;
    uint64_t media_clock_id;
    uint32_t codec;
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  reserved;
    uint16_t frame_samples;
    uint16_t codec_delay_samples;
    uint32_t target_bitrate;
};

struct wd_audio_packet_payload_header {
    uint8_t  session_id;
    uint64_t connection_token;
    uint64_t audio_epoch;
    uint64_t media_clock_id;
    uint64_t sequence;
    uint64_t pts_samples;
    uint16_t duration_samples;
    uint16_t flags;
    uint32_t data_size;
};

struct wd_video_frame_payload_header {
    uint8_t session_id;
    uint64_t connection_token;
    uint64_t content_epoch;

    /* Single enum wd_video_codec bit value selected for this frame. */
    uint32_t codec;

    /* Bitmask from enum wd_video_frame_flags. */
    uint16_t flags;

    uint64_t frame_id;
    uint64_t pts_usec;
    /* Visible/display size. */
    uint16_t width;
    uint16_t height;

    /* Codec dimensions; may be padded for chroma subsampling constraints. */
    uint16_t coded_width;
    uint16_t coded_height;

    /* Bytes following this header in the same WD_MSG_VIDEO_FRAME payload. */
    uint32_t data_size;
};

#define WD_VIDEO_FRAME_MAX_PAYLOAD_BYTES (64u * 1024u * 1024u)

struct wd_keyboard_event_payload {
    uint8_t session_id;
    uint64_t connection_token;
    uint64_t client_timestamp_ns;
    uint64_t input_sequence;
    uint16_t evdev_key_code;
    uint8_t  pressed;
};

struct wd_pointer_event_payload {
    uint8_t session_id;
    uint64_t connection_token;
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

enum wd_udp_tile_packet_flags {
    WD_UDP_TILE_FLAG_COMPRESSED     = 1u << 0,
    WD_UDP_TILE_FLAG_INPUT_SEQUENCE = 1u << 1,
};

#define WD_UDP_TILE_FLAG_MASK (WD_UDP_TILE_FLAG_COMPRESSED | WD_UDP_TILE_FLAG_INPUT_SEQUENCE)

enum wd_tile_size {
    WD_TILE_128x64 = 0,
    WD_TILE_64x64  = 1,
    WD_TILE_32x32  = 2,
    WD_TILE_16x16  = 3,
};

/* Protocol v36 uses one canonical base header for every tile fragment.
 * tile_payload_size is the total compressed or uncompressed tile payload.
 * The optional input sequence extension is legal only on packet zero. */
struct wd_udp_tile_packet_header {
    uint8_t  session_id;
    uint64_t connection_token;
    uint64_t content_epoch;
    uint8_t  flags;
    uint8_t  tile_size;
    uint8_t  tile_pkt_id;
    uint16_t tile_id;
    uint8_t  tile_pkt_count;
    uint8_t  reserved;
    uint16_t payload_size;
    uint16_t tile_payload_size;
    uint64_t tile_generation;
};

struct wd_udp_tile_input_sequence_extension {
    uint64_t input_sequence;
};

struct wd_udp_tile_packet_decoded {
    uint8_t  session_id;
    uint64_t connection_token;
    uint64_t content_epoch;
    uint8_t  flags;
    uint8_t  tile_size;
    uint8_t  tile_pkt_id;
    uint16_t tile_id;
    uint8_t  tile_pkt_count;
    uint16_t payload_size;
    uint16_t tile_payload_size;
    uint64_t tile_generation;
    uint64_t input_sequence;
    uint16_t header_size;
};

#define WD_UDP_TILE_HEADER_MIN_SIZE ((uint16_t)sizeof(struct wd_udp_tile_packet_header))
#define WD_UDP_TILE_HEADER_MAX_SIZE                                                                                                         \
    ((uint16_t)(sizeof(struct wd_udp_tile_packet_header) + sizeof(struct wd_udp_tile_input_sequence_extension)))

static inline bool wd_udp_tile_packet_is_compressed(const struct wd_udp_tile_packet_decoded* header) {
    return header && (header->flags & WD_UDP_TILE_FLAG_COMPRESSED) != 0;
}

static inline bool wd_udp_tile_packet_has_input_sequence(const struct wd_udp_tile_packet_decoded* header) {
    return header && (header->flags & WD_UDP_TILE_FLAG_INPUT_SEQUENCE) != 0;
}

static inline uint16_t wd_udp_tile_header_size_for_flags(uint8_t flags) {
    if ((flags & ~WD_UDP_TILE_FLAG_MASK) != 0)
    {
        return 0;
    }
    return (uint16_t)(sizeof(struct wd_udp_tile_packet_header) +
                      ((flags & WD_UDP_TILE_FLAG_INPUT_SEQUENCE) != 0
                           ? sizeof(struct wd_udp_tile_input_sequence_extension)
                           : 0));
}

static inline bool wd_tile_size_code_for_dimensions(uint16_t width, uint16_t height, uint8_t* out_tile_size) {
    if (!out_tile_size)
    {
        return false;
    }

    if (width == 128 && height == 64)
    {
        *out_tile_size = WD_TILE_128x64;
        return true;
    }
    if (width == 64 && height == 64)
    {
        *out_tile_size = WD_TILE_64x64;
        return true;
    }
    if (width == 32 && height == 32)
    {
        *out_tile_size = WD_TILE_32x32;
        return true;
    }
    if (width == 16 && height == 16)
    {
        *out_tile_size = WD_TILE_16x16;
        return true;
    }
    return false;
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

static inline bool wd_udp_tile_packet_encode_header(void* destination, size_t destination_size,
                                                     const struct wd_udp_tile_packet_decoded* header) {
    uint16_t tile_width = 0;
    uint16_t tile_height = 0;
    if (!destination || !header || header->session_id == 0 || header->connection_token == 0 ||
        header->content_epoch == 0 || header->tile_pkt_count == 0 ||
        header->tile_pkt_id >= header->tile_pkt_count || header->payload_size == 0 ||
        header->tile_payload_size == 0 || header->payload_size > header->tile_payload_size ||
        header->tile_generation == 0 ||
        !wd_tile_dimensions_for_size_code(header->tile_size, &tile_width, &tile_height))
    {
        return false;
    }
    const uint16_t header_size = wd_udp_tile_header_size_for_flags(header->flags);
    if (header_size == 0 || destination_size < header_size ||
        ((header->flags & WD_UDP_TILE_FLAG_INPUT_SEQUENCE) != 0 &&
         (header->tile_pkt_id != 0 || header->input_sequence == 0)))
    {
        return false;
    }

    struct wd_udp_tile_packet_header wire;
    memset(&wire, 0, sizeof(wire));
    wire.session_id = header->session_id;
    wire.connection_token = header->connection_token;
    wire.content_epoch = header->content_epoch;
    wire.flags = header->flags;
    wire.tile_size = header->tile_size;
    wire.tile_pkt_id = header->tile_pkt_id;
    wire.tile_id = header->tile_id;
    wire.tile_pkt_count = header->tile_pkt_count;
    wire.payload_size = header->payload_size;
    wire.tile_payload_size = header->tile_payload_size;
    wire.tile_generation = header->tile_generation;
    memcpy(destination, &wire, sizeof(wire));

    if ((header->flags & WD_UDP_TILE_FLAG_INPUT_SEQUENCE) != 0)
    {
        struct wd_udp_tile_input_sequence_extension extension;
        extension.input_sequence = header->input_sequence;
        memcpy((uint8_t*)destination + sizeof(wire), &extension, sizeof(extension));
    }
    return true;
}

static inline bool wd_udp_tile_packet_decode(const void* packet, size_t packet_size, struct wd_udp_tile_packet_decoded* out) {
    if (!packet || !out || packet_size < sizeof(struct wd_udp_tile_packet_header))
    {
        return false;
    }

    struct wd_udp_tile_packet_header wire;
    memcpy(&wire, packet, sizeof(wire));
    const uint16_t header_size = wd_udp_tile_header_size_for_flags(wire.flags);
    uint16_t tile_width = 0;
    uint16_t tile_height = 0;
    if (header_size == 0 || packet_size < header_size || wire.session_id == 0 ||
        wire.connection_token == 0 || wire.content_epoch == 0 || wire.reserved != 0 ||
        wire.tile_pkt_count == 0 || wire.tile_pkt_id >= wire.tile_pkt_count ||
        wire.payload_size == 0 || wire.tile_payload_size == 0 ||
        wire.payload_size > wire.tile_payload_size || wire.tile_generation == 0 ||
        !wd_tile_dimensions_for_size_code(wire.tile_size, &tile_width, &tile_height))
    {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->session_id = wire.session_id;
    out->connection_token = wire.connection_token;
    out->content_epoch = wire.content_epoch;
    out->flags = wire.flags;
    out->tile_size = wire.tile_size;
    out->tile_pkt_id = wire.tile_pkt_id;
    out->tile_id = wire.tile_id;
    out->tile_pkt_count = wire.tile_pkt_count;
    out->payload_size = wire.payload_size;
    out->tile_payload_size = wire.tile_payload_size;
    out->tile_generation = wire.tile_generation;
    out->header_size = header_size;

    if ((wire.flags & WD_UDP_TILE_FLAG_INPUT_SEQUENCE) != 0)
    {
        if (wire.tile_pkt_id != 0)
        {
            return false;
        }
        struct wd_udp_tile_input_sequence_extension extension;
        memcpy(&extension, (const uint8_t*)packet + sizeof(wire), sizeof(extension));
        if (extension.input_sequence == 0)
        {
            return false;
        }
        out->input_sequence = extension.input_sequence;
    }

    return (size_t)header_size + (size_t)out->payload_size == packet_size;
}

static inline bool wd_udp_tile_fragment_layout_valid(const struct wd_udp_tile_packet_decoded* header,
                                                      size_t packet_size, uint16_t udp_payload_target,
                                                      uint32_t total_payload_size) {
    if (!header || udp_payload_target == 0 || total_payload_size == 0 ||
        total_payload_size != header->tile_payload_size)
    {
        return false;
    }

    if ((size_t)header->header_size + (size_t)header->payload_size != packet_size)
    {
        return false;
    }

    const uint32_t expected_packet_count =
        (total_payload_size + (uint32_t)udp_payload_target - 1u) / (uint32_t)udp_payload_target;
    if (expected_packet_count == 0 || expected_packet_count > UINT8_MAX ||
        header->tile_pkt_count != expected_packet_count || header->tile_pkt_id >= expected_packet_count)
    {
        return false;
    }

    const uint32_t offset = (uint32_t)header->tile_pkt_id * (uint32_t)udp_payload_target;
    const uint32_t remaining = total_payload_size - offset;
    const uint16_t expected_payload_size =
        (uint16_t)(remaining > udp_payload_target ? udp_payload_target : remaining);

    return header->payload_size == expected_payload_size;
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
    uint64_t connection_token;
    uint16_t probe_count;
};

struct wd_mtu_probe_result_payload {
    uint8_t session_id;
    uint64_t connection_token;

    /*
     * Largest tile payload size received, excluding
     * the largest WayDisplay UDP tile packet header.
     */
    uint16_t max_udp_payload_received;
};

struct wd_throughput_probe_start_payload {
    uint8_t session_id;
    uint64_t connection_token;
    uint16_t probe_count;
    uint16_t payload_size;
    uint16_t duration_ms;
};

struct wd_throughput_probe_result_payload {
    uint8_t session_id;
    uint64_t connection_token;
    uint64_t bytes_received;
    uint32_t packets_received;
    uint16_t duration_ms;
};

#define WD_SELECTION_MIME_TEXT_UTF8  1u
#define WD_SELECTION_MIME_TEXT_PLAIN 2u
#define WD_SELECTION_MAX_TEXT_BYTES  (1024u * 1024u)

struct wd_selection_payload_header {
    uint8_t session_id;
    uint64_t connection_token;
    uint16_t mime_type;
    uint32_t data_size;
};

struct wd_cursor_shape_payload {
    uint8_t session_id;
    uint64_t connection_token;
    uint16_t shape;
};

struct wd_display_resize_payload {
    uint8_t session_id;
    uint64_t connection_token;
    uint16_t width;
    uint16_t height;
};

struct wd_config_applied_payload {
    uint8_t session_id;
    uint64_t connection_token;
    uint64_t config_epoch;
};

WD_PACKED_END

#undef WD_PACKED_BEGIN
#undef WD_PACKED_END

#if defined(__cplusplus)
static_assert(sizeof(struct wd_tcp_header) == 12, "unexpected wd_tcp_header size");
static_assert(sizeof(struct wd_udp_tile_packet_header) == 36, "unexpected wd_udp_tile_packet_header size");
static_assert(WD_UDP_TILE_HEADER_MAX_SIZE == 44, "unexpected maximum wd_udp_tile_packet_header size");
static_assert(sizeof(struct wd_client_hello_payload) == 44, "unexpected wd_client_hello_payload size");
static_assert(sizeof(struct wd_server_config_payload) == 101, "unexpected wd_server_config_payload size");
static_assert(sizeof(struct wd_tile_generation_entry) == 10, "unexpected wd_tile_generation_entry size");
static_assert(sizeof(struct wd_tile_summary_payload_header) == 28, "unexpected wd_tile_summary_payload_header size");
static_assert(sizeof(struct wd_tile_repair_request_payload_header) == 19, "unexpected wd_tile_repair_request_payload_header size");
static_assert(sizeof(struct wd_keyboard_event_payload) == 28, "unexpected wd_keyboard_event_payload size");
static_assert(sizeof(struct wd_pointer_event_payload) == 41, "unexpected wd_pointer_event_payload size");
static_assert(sizeof(struct wd_mtu_probe_start_payload) == 11, "unexpected wd_mtu_probe_start_payload size");
static_assert(sizeof(struct wd_mtu_probe_result_payload) == 11, "unexpected wd_mtu_probe_result_payload size");
static_assert(sizeof(struct wd_throughput_probe_start_payload) == 15, "unexpected wd_throughput_probe_start_payload size");
static_assert(sizeof(struct wd_throughput_probe_result_payload) == 23, "unexpected wd_throughput_probe_result_payload size");
static_assert(sizeof(struct wd_tile_repair_entry) == 10, "unexpected wd_tile_repair_entry size");
static_assert(sizeof(struct wd_client_stats_payload) == 381, "unexpected wd_client_stats_payload size");
static_assert(sizeof(struct wd_input_channel_hello_payload) == 9, "unexpected wd_input_channel_hello_payload size");
static_assert(sizeof(struct wd_selection_channel_hello_payload) == 9, "unexpected wd_selection_channel_hello_payload size");
static_assert(sizeof(struct wd_video_channel_hello_payload) == 15, "unexpected wd_video_channel_hello_payload size");
static_assert(sizeof(struct wd_audio_channel_hello_payload) == 15, "unexpected wd_audio_channel_hello_payload size");
static_assert(sizeof(struct wd_audio_config_payload) == 43, "unexpected wd_audio_config_payload size");
static_assert(sizeof(struct wd_audio_packet_payload_header) == 49, "unexpected wd_audio_packet_payload_header size");
static_assert(sizeof(struct wd_video_frame_payload_header) == 51, "unexpected wd_video_frame_payload_header size");
static_assert(sizeof(struct wd_selection_payload_header) == 15, "unexpected wd_selection_payload_header size");
static_assert(sizeof(struct wd_cursor_shape_payload) == 11, "unexpected wd_cursor_shape_payload size");
static_assert(sizeof(struct wd_display_resize_payload) == 13, "unexpected wd_display_resize_payload size");
static_assert(sizeof(struct wd_config_applied_payload) == 17, "unexpected wd_config_applied_payload size");
#else
_Static_assert(sizeof(struct wd_tcp_header) == 12, "unexpected wd_tcp_header size");
_Static_assert(sizeof(struct wd_udp_tile_packet_header) == 36, "unexpected wd_udp_tile_packet_header size");
_Static_assert(WD_UDP_TILE_HEADER_MAX_SIZE == 44, "unexpected maximum wd_udp_tile_packet_header size");
_Static_assert(sizeof(struct wd_client_hello_payload) == 44, "unexpected wd_client_hello_payload size");
_Static_assert(sizeof(struct wd_server_config_payload) == 101, "unexpected wd_server_config_payload size");
_Static_assert(sizeof(struct wd_tile_generation_entry) == 10, "unexpected wd_tile_generation_entry size");
_Static_assert(sizeof(struct wd_tile_summary_payload_header) == 28, "unexpected wd_tile_summary_payload_header size");
_Static_assert(sizeof(struct wd_tile_repair_request_payload_header) == 19, "unexpected wd_tile_repair_request_payload_header size");
_Static_assert(sizeof(struct wd_keyboard_event_payload) == 28, "unexpected wd_keyboard_event_payload size");
_Static_assert(sizeof(struct wd_pointer_event_payload) == 41, "unexpected wd_pointer_event_payload size");
_Static_assert(sizeof(struct wd_mtu_probe_start_payload) == 11, "unexpected wd_mtu_probe_start_payload size");
_Static_assert(sizeof(struct wd_mtu_probe_result_payload) == 11, "unexpected wd_mtu_probe_result_payload size");
_Static_assert(sizeof(struct wd_throughput_probe_start_payload) == 15, "unexpected wd_throughput_probe_start_payload size");
_Static_assert(sizeof(struct wd_throughput_probe_result_payload) == 23, "unexpected wd_throughput_probe_result_payload size");
_Static_assert(sizeof(struct wd_tile_repair_entry) == 10, "unexpected wd_tile_repair_entry size");
_Static_assert(sizeof(struct wd_client_stats_payload) == 381, "unexpected wd_client_stats_payload size");
_Static_assert(sizeof(struct wd_input_channel_hello_payload) == 9, "unexpected wd_input_channel_hello_payload size");
_Static_assert(sizeof(struct wd_selection_channel_hello_payload) == 9, "unexpected wd_selection_channel_hello_payload size");
_Static_assert(sizeof(struct wd_video_channel_hello_payload) == 15, "unexpected wd_video_channel_hello_payload size");
_Static_assert(sizeof(struct wd_audio_channel_hello_payload) == 15, "unexpected wd_audio_channel_hello_payload size");
_Static_assert(sizeof(struct wd_audio_config_payload) == 43, "unexpected wd_audio_config_payload size");
_Static_assert(sizeof(struct wd_audio_packet_payload_header) == 49, "unexpected wd_audio_packet_payload_header size");
_Static_assert(sizeof(struct wd_video_frame_payload_header) == 51, "unexpected wd_video_frame_payload_header size");
_Static_assert(sizeof(struct wd_selection_payload_header) == 15, "unexpected wd_selection_payload_header size");
_Static_assert(sizeof(struct wd_cursor_shape_payload) == 11, "unexpected wd_cursor_shape_payload size");
_Static_assert(sizeof(struct wd_display_resize_payload) == 13, "unexpected wd_display_resize_payload size");
_Static_assert(sizeof(struct wd_config_applied_payload) == 17, "unexpected wd_config_applied_payload size");
#endif


static inline bool wd_config_applied_matches(const struct wd_config_applied_payload* applied,
                                             uint8_t session_id, uint64_t connection_token,
                                             uint64_t config_epoch) {
    return applied && session_id != 0 && connection_token != 0 && config_epoch != 0 &&
           applied->session_id == session_id && applied->connection_token == connection_token &&
           applied->config_epoch == config_epoch;
}

static inline bool wd_fixed_payload_size_is_valid(uint32_t payload_size, size_t expected_size) {
    return (size_t)payload_size == expected_size;
}

static inline bool wd_keyboard_event_payload_is_valid(const struct wd_keyboard_event_payload* key,
                                                      uint32_t payload_size) {
    return key && wd_fixed_payload_size_is_valid(payload_size, sizeof(*key)) &&
           key->session_id != 0 && key->connection_token != 0 && key->client_timestamp_ns != 0 &&
           key->input_sequence != 0 && key->evdev_key_code != 0 && key->pressed <= 1;
}

static inline bool wd_pointer_event_payload_is_valid(const struct wd_pointer_event_payload* pointer,
                                                     uint32_t payload_size) {
    if (!pointer || !wd_fixed_payload_size_is_valid(payload_size, sizeof(*pointer)) ||
        pointer->session_id == 0 || pointer->connection_token == 0 || pointer->client_timestamp_ns == 0 ||
        pointer->input_sequence == 0 || (pointer->modifiers & ~0x0fu) != 0)
    {
        return false;
    }

    switch (pointer->event_type)
    {
        case WD_POINTER_EVENT_MOTION:
            return pointer->button == 0 && pointer->button_state == 0 && pointer->axis == 0 &&
                   pointer->axis_value == 0;
        case WD_POINTER_EVENT_BUTTON:
            return pointer->button != 0 && pointer->button_state <= WD_POINTER_BUTTON_PRESSED &&
                   pointer->axis == 0 && pointer->axis_value == 0;
        case WD_POINTER_EVENT_AXIS:
            return pointer->button == 0 && pointer->button_state == 0 &&
                   pointer->axis <= WD_POINTER_AXIS_HORIZONTAL && pointer->axis_value != 0;
        default:
            return false;
    }
}

static inline bool wd_counted_payload_size_is_valid(uint32_t payload_size, size_t header_size,
                                                     uint32_t count, size_t entry_size) {
    if (entry_size != 0 && (size_t)count > (SIZE_MAX - header_size) / entry_size)
    {
        return false;
    }
    return (size_t)payload_size == header_size + (size_t)count * entry_size;
}

static inline bool wd_selection_payload_size_is_valid(const struct wd_selection_payload_header* header,
                                                       uint32_t payload_size) {
    return header && header->data_size <= WD_SELECTION_MAX_TEXT_BYTES &&
           wd_counted_payload_size_is_valid(payload_size, sizeof(*header), header->data_size, 1u);
}

static inline bool wd_tile_summary_count_is_valid(uint16_t flags, uint16_t tile_count,
                                                  uint16_t total_tiles) {
    return total_tiles != 0 && tile_count <= total_tiles &&
           ((flags & WD_TILE_SUMMARY_DELTA) != 0 || tile_count == total_tiles);
}

static inline bool wd_tile_repair_count_is_valid(uint16_t request_count, uint16_t total_tiles) {
    return total_tiles != 0 && request_count != 0 && request_count <= total_tiles;
}

static inline bool wd_client_hello_payload_is_valid(const struct wd_client_hello_payload* hello,
                                                     uint32_t payload_size) {
    if (!hello || !wd_fixed_payload_size_is_valid(payload_size, sizeof(*hello)) ||
        (hello->capabilities & ~WD_CLIENT_CAP_MASK) != 0 ||
        (hello->video_codecs & ~WD_VIDEO_CODEC_MASK) != 0 || hello->video_reserved != 0 ||
        (hello->audio_codecs & ~WD_AUDIO_CODEC_MASK) != 0 || hello->audio_reserved != 0 ||
        hello->video_mode > WD_VIDEO_MODE_FORCE ||
        hello->video_min_dirty_percent > WD_VIDEO_MIN_DIRTY_PERCENT_MAX ||
        hello->video_exit_dirty_percent > WD_VIDEO_EXIT_DIRTY_PERCENT_MAX ||
        hello->video_enter_seconds > WD_VIDEO_ENTER_SECONDS_MAX ||
        hello->video_exit_seconds > WD_VIDEO_EXIT_SECONDS_MAX ||
        ((hello->desired_width == 0) != (hello->desired_height == 0)) ||
        hello->desired_width > WD_MAX_RENDER_WIDTH || hello->desired_height > WD_MAX_RENDER_HEIGHT)
    {
        return false;
    }

    const bool video = (hello->capabilities & WD_CLIENT_CAP_VIDEO_STREAM) != 0;
    const bool audio = (hello->capabilities & WD_CLIENT_CAP_AUDIO_STREAM) != 0;
    if (video)
    {
        if (hello->video_codecs == 0 || hello->video_transport != WD_VIDEO_TRANSPORT_TCP)
        {
            return false;
        }
    }
    else if (hello->video_codecs != 0 || hello->video_transport != 0)
    {
        return false;
    }

    if (audio)
    {
        return hello->audio_codecs != 0 && hello->audio_transport == WD_AUDIO_TRANSPORT_TCP &&
               hello->audio_max_channels != 0 && hello->audio_max_channels <= WD_AUDIO_CHANNELS_MAX &&
               (hello->audio_target_latency_ms == 0 ||
                (hello->audio_target_latency_ms >= WD_AUDIO_TARGET_LATENCY_MS_MIN &&
                 hello->audio_target_latency_ms <= WD_AUDIO_TARGET_LATENCY_MS_MAX));
    }
    return hello->audio_codecs == 0 && hello->audio_transport == 0 &&
           hello->audio_max_channels == 0 && hello->audio_target_latency_ms == 0;
}

static inline bool wd_audio_frame_samples_is_valid(uint16_t frame_samples) {
    return frame_samples == 120 || frame_samples == 240 || frame_samples == 480 ||
           frame_samples == 960 || frame_samples == 1920 || frame_samples == 2880;
}

static inline bool wd_audio_config_payload_is_valid(const struct wd_audio_config_payload* config,
                                                     uint32_t payload_size) {
    return config && wd_fixed_payload_size_is_valid(payload_size, sizeof(*config)) &&
           config->session_id != 0 && config->connection_token != 0 && config->audio_epoch != 0 &&
           config->media_clock_id != 0 && config->codec == WD_AUDIO_CODEC_OPUS &&
           config->sample_rate == WD_AUDIO_SAMPLE_RATE_DEFAULT && config->channels != 0 &&
           config->channels <= WD_AUDIO_CHANNELS_MAX && config->reserved == 0 &&
           wd_audio_frame_samples_is_valid(config->frame_samples) &&
           config->codec_delay_samples <= config->frame_samples && config->target_bitrate != 0;
}

static inline bool wd_audio_packet_payload_size_is_valid(const struct wd_audio_packet_payload_header* header,
                                                          uint32_t tcp_payload_size) {
    if (!header || tcp_payload_size < sizeof(*header) || header->session_id == 0 ||
        header->connection_token == 0 || header->audio_epoch == 0 || header->media_clock_id == 0 ||
        header->sequence == 0 || (header->flags & ~WD_AUDIO_PACKET_FLAG_MASK) != 0)
    {
        return false;
    }

    const uint32_t expected_data_size = tcp_payload_size - (uint32_t)sizeof(*header);
    if (header->data_size != expected_data_size || header->data_size > WD_AUDIO_PACKET_MAX_PAYLOAD_BYTES)
    {
        return false;
    }

    if ((header->flags & WD_AUDIO_PACKET_END_OF_STREAM) != 0)
    {
        return header->flags == WD_AUDIO_PACKET_END_OF_STREAM &&
               header->data_size == 0 && header->duration_samples == 0;
    }

    return header->data_size != 0 && wd_audio_frame_samples_is_valid(header->duration_samples);
}

static inline bool wd_video_frame_payload_size_is_valid(const struct wd_video_frame_payload_header* header,
                                                        uint32_t tcp_payload_size) {
    if (!header || tcp_payload_size < sizeof(*header) || header->session_id == 0 ||
        header->connection_token == 0 || header->content_epoch == 0 ||
        (header->flags & ~WD_VIDEO_FRAME_FLAG_MASK) != 0 ||
        header->codec == 0 || (header->codec & ~WD_VIDEO_CODEC_MASK) != 0 ||
        (header->codec & (header->codec - 1u)) != 0)
    {
        return false;
    }

    const uint32_t expected_data_size = tcp_payload_size - (uint32_t)sizeof(*header);
    if (header->data_size != expected_data_size || header->data_size > WD_VIDEO_FRAME_MAX_PAYLOAD_BYTES)
    {
        return false;
    }

    const bool end_of_stream = (header->flags & WD_VIDEO_FRAME_END_OF_STREAM) != 0;
    const bool resize = (header->flags & WD_VIDEO_FRAME_RESIZE) != 0;
    const bool control = end_of_stream || resize;
    if (control)
    {
        return header->data_size == 0 && header->frame_id == 0 &&
               header->width != 0 && header->height != 0 &&
               header->coded_width == header->width && header->coded_height == header->height &&
               (!resize || end_of_stream) &&
               (header->flags & (WD_VIDEO_FRAME_CONFIG | WD_VIDEO_FRAME_KEYFRAME)) == 0;
    }

    return header->data_size != 0 && header->frame_id != 0 &&
           header->width != 0 && header->height != 0 &&
           header->coded_width >= header->width && header->coded_height >= header->height &&
           ((header->flags & WD_VIDEO_FRAME_CONFIG) == 0 ||
            (header->flags & WD_VIDEO_FRAME_KEYFRAME) != 0);
}

static inline const uint8_t* wd_video_frame_payload_data(const void* payload, uint32_t tcp_payload_size) {
    if (!payload || tcp_payload_size < sizeof(struct wd_video_frame_payload_header))
    {
        return NULL;
    }

    const struct wd_video_frame_payload_header* header = (const struct wd_video_frame_payload_header*)payload;
    if (!wd_video_frame_payload_size_is_valid(header, tcp_payload_size))
    {
        return NULL;
    }

    return (const uint8_t*)payload + sizeof(*header);
}

#ifdef __cplusplus
}
#endif
