#pragma once

/* Diagnostic sampling only: the hash is for locating byte differences,
 * not for authentication, integrity enforcement, or protocol decisions. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool wd_video_trace_sample(uint64_t frame_id) {
    return frame_id != 0 && (frame_id <= 8u || (frame_id % 128u) == 0);
}

static inline uint64_t wd_video_trace_hash(const uint8_t* data, size_t size) {
    if (!data && size != 0)
    {
        return 0;
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* First eight bytes in network-reading order, zero-padded on the right. */
static inline uint64_t wd_video_trace_prefix(const uint8_t* data, size_t size) {
    uint64_t prefix = 0;
    for (size_t i = 0; i < 8; ++i)
    {
        prefix <<= 8;
        if (data && i < size)
        {
            prefix |= data[i];
        }
    }
    return prefix;
}
