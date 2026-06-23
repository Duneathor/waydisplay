#pragma once

#include "waydisplay/wd_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_selection_text_view {
    uint16_t       mime_type;
    const uint8_t* data;
    uint32_t       size;
};

/* Clipboard text is transported as UTF-8 without an embedded NUL. */
bool wd_selection_text_is_valid(const uint8_t* data, uint32_t size);

/* Normalize text read from a Wayland source. Trailing NUL terminators are
 * ignored, but embedded NULs and invalid UTF-8 are rejected. */
bool wd_selection_text_normalize_size(const uint8_t* data, uint32_t size, uint32_t* normalized_size);

bool wd_selection_payload_decode(const uint8_t* payload, uint32_t payload_size, uint8_t expected_session_id,
                                 uint64_t expected_connection_token, struct wd_selection_text_view* out);

bool wd_selection_payload_encode(uint8_t session_id, uint64_t connection_token, uint16_t mime_type, const uint8_t* text, uint32_t text_size,
                                 uint8_t* payload, size_t payload_capacity, uint32_t* payload_size);

#ifdef __cplusplus
}
#endif
