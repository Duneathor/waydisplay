#ifndef WAYDISPLAY_SERVER_SELECTION_CAPTURE_H
#define WAYDISPLAY_SERVER_SELECTION_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_selection_capture_buffer {
    uint8_t* data;
    uint32_t size;
    uint32_t capacity;
    bool     failed;
};

void wd_selection_capture_buffer_init(struct wd_selection_capture_buffer* buffer);
void wd_selection_capture_buffer_destroy(struct wd_selection_capture_buffer* buffer);

bool wd_selection_capture_buffer_append(struct wd_selection_capture_buffer* buffer, const uint8_t* data, size_t size);

bool wd_selection_capture_buffer_finish(struct wd_selection_capture_buffer* buffer, uint8_t** out_text, uint32_t* out_size);

#ifdef __cplusplus
}
#endif

#endif
