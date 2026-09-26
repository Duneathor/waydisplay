#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_buffer;

typedef void (*wd_buffer_release_fn)(void* user_data, uint8_t* data, size_t size);

/*
 * Thread-safe shared byte storage.
 *
 * wd_buffer_alloc() stores bytes in the same allocation as the control block.
 * wd_buffer_wrap() adopts externally managed bytes and invokes release exactly
 * once after the last reference is released.
 */
struct wd_buffer* wd_buffer_alloc(size_t size);
/* Allocate logical size bytes plus zero-filled tail padding. Padding is not
 * included in wd_buffer_size(), but remains addressable storage for consumers
 * that require bounded over-read space (for example FFmpeg bitstream parsers). */
struct wd_buffer* wd_buffer_alloc_padded(size_t size, size_t padding);
struct wd_buffer* wd_buffer_wrap(uint8_t* data, size_t size, wd_buffer_release_fn release, void* user_data);

struct wd_buffer* wd_buffer_retain(struct wd_buffer* buffer);
void              wd_buffer_release(struct wd_buffer* buffer);

uint8_t*       wd_buffer_data(struct wd_buffer* buffer);
const uint8_t* wd_buffer_const_data(const struct wd_buffer* buffer);
size_t         wd_buffer_size(const struct wd_buffer* buffer);
size_t         wd_buffer_capacity(const struct wd_buffer* buffer);

/* Validate a byte range without forming an out-of-bounds pointer. */
bool wd_buffer_range_valid(const struct wd_buffer* buffer, size_t offset, size_t size);

#ifdef __cplusplus
}
#endif
