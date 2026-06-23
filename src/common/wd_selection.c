#include "waydisplay/wd_selection.h"

#include <string.h>

static bool wd_selection_mime_is_valid(uint16_t mime_type) {
    return mime_type == WD_SELECTION_MIME_TEXT_UTF8 || mime_type == WD_SELECTION_MIME_TEXT_PLAIN;
}

static bool wd_utf8_continuation(uint8_t byte) {
    return (byte & 0xc0u) == 0x80u;
}

bool wd_selection_text_is_valid(const uint8_t* data, uint32_t size) {
    if (!data && size != 0)
    {
        return false;
    }

    uint32_t i = 0;
    while (i < size)
    {
        const uint8_t first = data[i++];
        if (first == 0)
        {
            return false;
        }
        if (first <= 0x7fu)
        {
            continue;
        }

        if (first >= 0xc2u && first <= 0xdfu)
        {
            if (i >= size || !wd_utf8_continuation(data[i]))
            {
                return false;
            }
            i++;
            continue;
        }

        if (first >= 0xe0u && first <= 0xefu)
        {
            if (size - i < 2u || !wd_utf8_continuation(data[i]) || !wd_utf8_continuation(data[i + 1u]))
            {
                return false;
            }
            if ((first == 0xe0u && data[i] < 0xa0u) || (first == 0xedu && data[i] >= 0xa0u))
            {
                return false;
            }
            i += 2u;
            continue;
        }

        if (first >= 0xf0u && first <= 0xf4u)
        {
            if (size - i < 3u || !wd_utf8_continuation(data[i]) || !wd_utf8_continuation(data[i + 1u]) ||
                !wd_utf8_continuation(data[i + 2u]))
            {
                return false;
            }
            if ((first == 0xf0u && data[i] < 0x90u) || (first == 0xf4u && data[i] > 0x8fu))
            {
                return false;
            }
            i += 3u;
            continue;
        }

        return false;
    }

    return true;
}

bool wd_selection_text_normalize_size(const uint8_t* data, uint32_t size, uint32_t* normalized_size) {
    if (!normalized_size || (!data && size != 0))
    {
        return false;
    }

    while (size != 0 && data[size - 1u] == 0)
    {
        size--;
    }

    if (size > WD_SELECTION_MAX_TEXT_BYTES || !wd_selection_text_is_valid(data, size))
    {
        return false;
    }

    *normalized_size = size;
    return true;
}

bool wd_selection_payload_decode(const uint8_t* payload, uint32_t payload_size, uint8_t expected_session_id,
                                 uint64_t expected_connection_token, struct wd_selection_text_view* out) {
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }

    if (!payload || !out || expected_session_id == 0 || expected_connection_token == 0 ||
        payload_size < sizeof(struct wd_selection_payload_header))
    {
        return false;
    }

    struct wd_selection_payload_header header;
    memcpy(&header, payload, sizeof(header));

    if (header.session_id != expected_session_id || header.connection_token != expected_connection_token ||
        !wd_selection_mime_is_valid(header.mime_type) || !wd_selection_payload_size_is_valid(&header, payload_size))
    {
        return false;
    }

    const uint8_t* text = payload + sizeof(header);
    if (!wd_selection_text_is_valid(text, header.data_size))
    {
        return false;
    }

    out->mime_type = header.mime_type;
    out->data      = text;
    out->size      = header.data_size;
    return true;
}

bool wd_selection_payload_encode(uint8_t session_id, uint64_t connection_token, uint16_t mime_type, const uint8_t* text, uint32_t text_size,
                                 uint8_t* payload, size_t payload_capacity, uint32_t* payload_size) {
    if (payload_size)
    {
        *payload_size = 0;
    }

    if (session_id == 0 || connection_token == 0 || !wd_selection_mime_is_valid(mime_type) || text_size > WD_SELECTION_MAX_TEXT_BYTES ||
        !wd_selection_text_is_valid(text, text_size) || !payload || !payload_size)
    {
        return false;
    }

    const size_t needed = sizeof(struct wd_selection_payload_header) + (size_t)text_size;
    if (needed > payload_capacity || needed > UINT32_MAX)
    {
        return false;
    }

    struct wd_selection_payload_header header;
    memset(&header, 0, sizeof(header));
    header.session_id       = session_id;
    header.connection_token = connection_token;
    header.mime_type        = mime_type;
    header.data_size        = text_size;

    memcpy(payload, &header, sizeof(header));
    if (text_size != 0)
    {
        memcpy(payload + sizeof(header), text, text_size);
    }

    *payload_size = (uint32_t)needed;
    return true;
}
