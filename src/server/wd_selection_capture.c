#include "wd_selection_capture.h"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_selection.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define WD_SELECTION_CAPTURE_RAW_MAX_BYTES (WD_SELECTION_MAX_TEXT_BYTES + 1u)

void wd_selection_capture_buffer_init(struct wd_selection_capture_buffer* buffer) {
    if (!buffer)
    {
        return;
    }

    memset(buffer, 0, sizeof(*buffer));
}

void wd_selection_capture_buffer_destroy(struct wd_selection_capture_buffer* buffer) {
    if (!buffer)
    {
        return;
    }

    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static bool reserve_capture_bytes(struct wd_selection_capture_buffer* buffer,
                                  uint32_t required) {
    if (required <= buffer->capacity)
    {
        return true;
    }

    uint32_t capacity = buffer->capacity ? buffer->capacity : WD_SELECTION_CAPTURE_INITIAL_BYTES;
    while (capacity < required)
    {
        if (capacity > WD_SELECTION_CAPTURE_RAW_MAX_BYTES / 2u)
        {
            capacity = WD_SELECTION_CAPTURE_RAW_MAX_BYTES;
            break;
        }
        capacity *= 2u;
    }

    uint8_t* resized = realloc(buffer->data, (size_t)capacity + 1u);
    if (!resized)
    {
        return false;
    }

    buffer->data     = resized;
    buffer->capacity = capacity;
    return true;
}

bool wd_selection_capture_buffer_append(struct wd_selection_capture_buffer* buffer,
                                        const uint8_t* data, size_t size) {
    if (!buffer || (!data && size > 0) || buffer->failed)
    {
        return false;
    }

    if (size > UINT32_MAX || size > WD_SELECTION_CAPTURE_RAW_MAX_BYTES - buffer->size)
    {
        buffer->failed = true;
        return false;
    }

    const uint32_t required = buffer->size + (uint32_t)size;
    if (!reserve_capture_bytes(buffer, required))
    {
        buffer->failed = true;
        return false;
    }

    if (size > 0)
    {
        memcpy(buffer->data + buffer->size, data, size);
        buffer->size = required;
    }

    if (buffer->data)
    {
        buffer->data[buffer->size] = 0;
    }
    return true;
}

bool wd_selection_capture_buffer_finish(struct wd_selection_capture_buffer* buffer,
                                        uint8_t** out_text, uint32_t* out_size) {
    if (out_text)
    {
        *out_text = NULL;
    }
    if (out_size)
    {
        *out_size = 0;
    }

    if (!buffer || !out_text || !out_size || buffer->failed)
    {
        return false;
    }

    uint32_t normalized_size = 0;
    if (!wd_selection_text_normalize_size(buffer->data, buffer->size,
                                          &normalized_size))
    {
        buffer->failed = true;
        return false;
    }

    uint8_t* text = calloc((size_t)normalized_size + 1u, 1);
    if (!text)
    {
        buffer->failed = true;
        return false;
    }

    if (normalized_size > 0)
    {
        memcpy(text, buffer->data, normalized_size);
    }

    *out_text = text;
    *out_size = normalized_size;
    return true;
}
