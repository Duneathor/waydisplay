#pragma once

#include "waydisplay/wd_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool wd_tcp_header_encode(uint8_t destination[WD_TCP_HEADER_WIRE_SIZE], const struct wd_tcp_header* header);
bool wd_tcp_header_decode(const uint8_t source[WD_TCP_HEADER_WIRE_SIZE], struct wd_tcp_header* header);

/* Protocol zero uses the packed little-endian payload structs directly on the
 * wire. This function validates only the structural payload shape; semantic
 * field validation remains with the protocol handlers. */
bool wd_protocol_payload_validate(uint16_t message_type, const void* payload, uint32_t payload_size);
bool wd_protocol_payload_wire_size(uint16_t message_type, const void* payload, uint32_t payload_size, uint32_t* out_wire_size);

#ifdef __cplusplus
}
#endif
