#include "waydisplay/wd_protocol_codec.h"

#include "waydisplay/wd_protocol_dispatch.h"

#include <string.h>

bool wd_tcp_header_encode(uint8_t destination[WD_TCP_HEADER_WIRE_SIZE], const struct wd_tcp_header* header) {
    if (!destination || !header)
    {
        return false;
    }

    memcpy(destination, header, sizeof(*header));
    return true;
}

bool wd_tcp_header_decode(const uint8_t source[WD_TCP_HEADER_WIRE_SIZE], struct wd_tcp_header* header) {
    if (!source || !header)
    {
        return false;
    }

    memcpy(header, source, sizeof(*header));
    return true;
}

bool wd_protocol_payload_validate(uint16_t message_type, const void* payload, uint32_t payload_size) {
    if (payload_size != 0 && !payload)
    {
        return false;
    }
    return wd_protocol_payload_size_is_valid(message_type, payload_size);
}

bool wd_protocol_payload_wire_size(uint16_t message_type, const void* payload, uint32_t payload_size, uint32_t* out_wire_size) {
    if (!out_wire_size || !wd_protocol_payload_validate(message_type, payload, payload_size))
    {
        return false;
    }

    *out_wire_size = payload_size;
    return true;
}
