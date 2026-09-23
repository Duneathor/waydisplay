#pragma once

/* Diagnostic sampling only: the hash is for locating byte differences,
 * not for authentication, integrity enforcement, or protocol decisions. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waydisplay/wd_log.h"

static inline bool wd_video_trace_sample(uint64_t frame_id) {
    return frame_id != 0 && (frame_id <= 8u || (frame_id % 128u) == 0);
}

/* DEBUG traces are fully disabled in INFO/STATS builds, including the
 * per-packet hash scan. Keep the sampling policy independent for tests and
 * for warning-level reports of unusual encoder drops. */
static inline bool wd_video_trace_debug_sample(uint64_t frame_id) {
#if WAYDISPLAY_LOG_LEVEL >= WD_LOG_LEVEL_VALUE_DEBUG
    return wd_video_trace_sample(frame_id);
#else
    (void)frame_id;
    return false;
#endif
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
