#pragma once

#include "waydisplay/wd_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_protocol_channel {
    WD_PROTOCOL_CHANNEL_CONTROL       = 1u << 0,
    WD_PROTOCOL_CHANNEL_INPUT         = 1u << 1,
    WD_PROTOCOL_CHANNEL_SELECTION     = 1u << 2,
    WD_PROTOCOL_CHANNEL_VIDEO         = 1u << 3,
    WD_PROTOCOL_CHANNEL_AUDIO         = 1u << 4,
    WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE = 1u << 5,
};

enum wd_protocol_phase {
    WD_PROTOCOL_PHASE_NEGOTIATION = 1u << 0,
    WD_PROTOCOL_PHASE_ESTABLISHED = 1u << 1,
};

enum wd_protocol_direction {
    WD_PROTOCOL_CLIENT_TO_SERVER = 1u << 0,
    WD_PROTOCOL_SERVER_TO_CLIENT = 1u << 1,
};

enum wd_protocol_payload_kind {
    WD_PROTOCOL_PAYLOAD_EMPTY = 0,
    WD_PROTOCOL_PAYLOAD_FIXED,
    WD_PROTOCOL_PAYLOAD_OPAQUE_TAIL,
    WD_PROTOCOL_PAYLOAD_REPEATED,
};

struct wd_protocol_message_descriptor {
    uint16_t                      message_type;
    const char*                   name;
    uint32_t                      channels;
    uint32_t                      phases;
    uint32_t                      directions;
    enum wd_protocol_payload_kind payload_kind;
    uint32_t                      payload_prefix_size;
    uint32_t                      repeated_entry_size;
    uint32_t                      minimum_payload_size;
    uint32_t                      maximum_payload_size;
};

const struct wd_protocol_message_descriptor* wd_protocol_message_descriptor_find(uint16_t message_type);
bool wd_protocol_payload_size_is_valid(uint16_t message_type, uint32_t payload_size);
bool wd_protocol_message_allowed(uint16_t message_type, enum wd_protocol_channel channel, enum wd_protocol_phase phase,
                                 enum wd_protocol_direction direction, uint32_t payload_size);
uint32_t wd_protocol_channel_max_payload(enum wd_protocol_channel channel, enum wd_protocol_phase phase,
                                         enum wd_protocol_direction direction);
const char* wd_protocol_message_name(uint16_t message_type);

#ifdef __cplusplus
}
#endif
