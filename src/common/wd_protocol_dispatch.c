#include "waydisplay/wd_protocol_dispatch.h"

#include "waydisplay/wd_config.h"

#include <stddef.h>

#define WD_FIXED(type, channel, phase, direction, payload_type)                                                                              \
    {type, #type, channel, phase, direction, WD_PROTOCOL_PAYLOAD_FIXED, sizeof(struct payload_type), 0, sizeof(struct payload_type),          \
     sizeof(struct payload_type)}
#define WD_EMPTY(type, channel, phase, direction)                                                                                            \
    {type, #type, channel, phase, direction, WD_PROTOCOL_PAYLOAD_EMPTY, 0, 0, 0, 0}
#define WD_OPAQUE(type, channel, phase, direction, payload_type, maximum)                                                                     \
    {type, #type, channel, phase, direction, WD_PROTOCOL_PAYLOAD_OPAQUE_TAIL, sizeof(struct payload_type), 0, sizeof(struct payload_type),    \
     maximum}
#define WD_REPEAT(type, channel, phase, direction, header_type, entry_type, maximum)                                                          \
    {type, #type, channel, phase, direction, WD_PROTOCOL_PAYLOAD_REPEATED, sizeof(struct header_type), sizeof(struct entry_type),             \
     sizeof(struct header_type), maximum}

static const struct wd_protocol_message_descriptor wd_message_descriptors[] = {
    WD_FIXED(WD_MSG_CLIENT_HELLO, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_CLIENT_TO_SERVER,
             wd_client_hello_payload),
    WD_FIXED(WD_MSG_SERVER_CONFIG, WD_PROTOCOL_CHANNEL_CONTROL,
             WD_PROTOCOL_PHASE_NEGOTIATION | WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT, wd_server_config_payload),
    WD_REPEAT(WD_MSG_TILE_GENERATION_SUMMARY, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED,
              WD_PROTOCOL_SERVER_TO_CLIENT, wd_tile_summary_payload_header, wd_tile_generation_entry, WD_TCP_MAX_PAYLOAD_SIZE),
    WD_REPEAT(WD_MSG_TILE_REPAIR_REQUEST, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED,
              WD_PROTOCOL_CLIENT_TO_SERVER, wd_tile_repair_request_payload_header, wd_tile_repair_entry, WD_TCP_MAX_PAYLOAD_SIZE),
    WD_FIXED(WD_MSG_KEYBOARD_KEY, WD_PROTOCOL_CHANNEL_INPUT, WD_PROTOCOL_PHASE_ESTABLISHED,
             WD_PROTOCOL_CLIENT_TO_SERVER, wd_keyboard_event_payload),
    WD_FIXED(WD_MSG_POINTER_EVENT, WD_PROTOCOL_CHANNEL_INPUT, WD_PROTOCOL_PHASE_ESTABLISHED,
             WD_PROTOCOL_CLIENT_TO_SERVER, wd_pointer_event_payload),
    WD_FIXED(WD_MSG_MTU_PROBE_START, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_SERVER_TO_CLIENT,
             wd_mtu_probe_start_payload),
    WD_FIXED(WD_MSG_MTU_PROBE_RESULT, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_CLIENT_TO_SERVER,
             wd_mtu_probe_result_payload),
    WD_OPAQUE(WD_MSG_CLIPBOARD_SET, WD_PROTOCOL_CHANNEL_SELECTION, WD_PROTOCOL_PHASE_ESTABLISHED,
              WD_PROTOCOL_CLIENT_TO_SERVER | WD_PROTOCOL_SERVER_TO_CLIENT, wd_selection_payload_header,
              sizeof(struct wd_selection_payload_header) + WD_SELECTION_MAX_TEXT_BYTES),
    WD_EMPTY(WD_MSG_CLIPBOARD_REQUEST, WD_PROTOCOL_CHANNEL_SELECTION, WD_PROTOCOL_PHASE_ESTABLISHED,
             WD_PROTOCOL_CLIENT_TO_SERVER | WD_PROTOCOL_SERVER_TO_CLIENT),
    WD_OPAQUE(WD_MSG_PRIMARY_SET, WD_PROTOCOL_CHANNEL_SELECTION, WD_PROTOCOL_PHASE_ESTABLISHED,
              WD_PROTOCOL_CLIENT_TO_SERVER | WD_PROTOCOL_SERVER_TO_CLIENT, wd_selection_payload_header,
              sizeof(struct wd_selection_payload_header) + WD_SELECTION_MAX_TEXT_BYTES),
    WD_EMPTY(WD_MSG_PRIMARY_REQUEST, WD_PROTOCOL_CHANNEL_SELECTION, WD_PROTOCOL_PHASE_ESTABLISHED,
             WD_PROTOCOL_CLIENT_TO_SERVER | WD_PROTOCOL_SERVER_TO_CLIENT),
    WD_FIXED(WD_MSG_CURSOR_SHAPE, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT,
             wd_cursor_shape_payload),
    WD_FIXED(WD_MSG_DISPLAY_RESIZE, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
             wd_display_resize_payload),
    WD_FIXED(WD_MSG_THROUGHPUT_PROBE_START, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_SERVER_TO_CLIENT,
             wd_throughput_probe_start_payload),
    WD_FIXED(WD_MSG_THROUGHPUT_PROBE_RESULT, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_CLIENT_TO_SERVER,
             wd_throughput_probe_result_payload),
    WD_FIXED(WD_MSG_INPUT_CHANNEL_HELLO, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE, WD_PROTOCOL_PHASE_NEGOTIATION,
             WD_PROTOCOL_CLIENT_TO_SERVER, wd_input_channel_hello_payload),
    WD_FIXED(WD_MSG_SELECTION_CHANNEL_HELLO, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE, WD_PROTOCOL_PHASE_NEGOTIATION,
             WD_PROTOCOL_CLIENT_TO_SERVER, wd_selection_channel_hello_payload),
    WD_FIXED(WD_MSG_CLIENT_STATS, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
             wd_client_stats_payload),
    WD_FIXED(WD_MSG_LINK_PROBE_PING, WD_PROTOCOL_CHANNEL_CONTROL,
             WD_PROTOCOL_PHASE_NEGOTIATION | WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT, wd_link_probe_payload),
    WD_FIXED(WD_MSG_LINK_PROBE_PONG, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_CLIENT_TO_SERVER,
             wd_link_probe_payload),
    WD_FIXED(WD_MSG_VIDEO_CHANNEL_HELLO, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE, WD_PROTOCOL_PHASE_NEGOTIATION,
             WD_PROTOCOL_CLIENT_TO_SERVER, wd_video_channel_hello_payload),
    WD_OPAQUE(WD_MSG_VIDEO_FRAME, WD_PROTOCOL_CHANNEL_VIDEO, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT,
              wd_video_frame_payload_header, sizeof(struct wd_video_frame_payload_header) + WD_VIDEO_FRAME_MAX_PAYLOAD_BYTES),
    WD_FIXED(WD_MSG_CONFIG_APPLIED, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
             wd_config_applied_payload),
    WD_FIXED(WD_MSG_AUDIO_CHANNEL_HELLO, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE, WD_PROTOCOL_PHASE_NEGOTIATION,
             WD_PROTOCOL_CLIENT_TO_SERVER, wd_audio_channel_hello_payload),
    WD_FIXED(WD_MSG_AUDIO_CONFIG, WD_PROTOCOL_CHANNEL_AUDIO, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT,
             wd_audio_config_payload),
    WD_OPAQUE(WD_MSG_AUDIO_PACKET, WD_PROTOCOL_CHANNEL_AUDIO, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT,
              wd_audio_packet_payload_header, sizeof(struct wd_audio_packet_payload_header) + WD_AUDIO_PACKET_MAX_PAYLOAD_BYTES),
};

#undef WD_FIXED
#undef WD_EMPTY
#undef WD_OPAQUE
#undef WD_REPEAT

const struct wd_protocol_message_descriptor* wd_protocol_message_descriptor_find(uint16_t message_type) {
    for (size_t i = 0; i < sizeof(wd_message_descriptors) / sizeof(wd_message_descriptors[0]); ++i)
    {
        if (wd_message_descriptors[i].message_type == message_type)
        {
            return &wd_message_descriptors[i];
        }
    }
    return NULL;
}

static bool wd_protocol_descriptor_size_is_valid(const struct wd_protocol_message_descriptor* descriptor, uint32_t payload_size) {
    if (!descriptor || payload_size < descriptor->minimum_payload_size || payload_size > descriptor->maximum_payload_size)
    {
        return false;
    }

    switch (descriptor->payload_kind)
    {
    case WD_PROTOCOL_PAYLOAD_EMPTY:
        return payload_size == 0;
    case WD_PROTOCOL_PAYLOAD_FIXED:
        return payload_size == descriptor->payload_prefix_size;
    case WD_PROTOCOL_PAYLOAD_OPAQUE_TAIL:
        return payload_size >= descriptor->payload_prefix_size;
    case WD_PROTOCOL_PAYLOAD_REPEATED:
        return payload_size >= descriptor->payload_prefix_size && descriptor->repeated_entry_size != 0 &&
               (payload_size - descriptor->payload_prefix_size) % descriptor->repeated_entry_size == 0;
    default:
        return false;
    }
}

bool wd_protocol_payload_size_is_valid(uint16_t message_type, uint32_t payload_size) {
    return wd_protocol_descriptor_size_is_valid(wd_protocol_message_descriptor_find(message_type), payload_size);
}

bool wd_protocol_message_allowed(uint16_t message_type, enum wd_protocol_channel channel, enum wd_protocol_phase phase,
                                 enum wd_protocol_direction direction, uint32_t payload_size) {
    const struct wd_protocol_message_descriptor* descriptor = wd_protocol_message_descriptor_find(message_type);
    return descriptor && (descriptor->channels & (uint32_t)channel) != 0 && (descriptor->phases & (uint32_t)phase) != 0 &&
           (descriptor->directions & (uint32_t)direction) != 0 && wd_protocol_descriptor_size_is_valid(descriptor, payload_size);
}

uint32_t wd_protocol_channel_max_payload(enum wd_protocol_channel channel, enum wd_protocol_phase phase,
                                         enum wd_protocol_direction direction) {
    uint32_t maximum = 0;
    for (size_t i = 0; i < sizeof(wd_message_descriptors) / sizeof(wd_message_descriptors[0]); ++i)
    {
        const struct wd_protocol_message_descriptor* descriptor = &wd_message_descriptors[i];
        if ((descriptor->channels & (uint32_t)channel) != 0 && (descriptor->phases & (uint32_t)phase) != 0 &&
            (descriptor->directions & (uint32_t)direction) != 0 && descriptor->maximum_payload_size > maximum)
        {
            maximum = descriptor->maximum_payload_size;
        }
    }
    return maximum;
}

const char* wd_protocol_message_name(uint16_t message_type) {
    const struct wd_protocol_message_descriptor* descriptor = wd_protocol_message_descriptor_find(message_type);
    return descriptor ? descriptor->name : "WD_MSG_UNKNOWN";
}
