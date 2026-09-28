#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WD_VIDEO_GPU_CAPTURE_BACKOFF_NS 2000000000ull

static inline bool wd_video_gpu_capture_backoff_active(uint64_t now_ns, uint64_t until_ns) {
    return until_ns != 0 && now_ns < until_ns;
}

static inline uint64_t wd_video_gpu_capture_backoff_deadline(uint64_t now_ns) {
    if (UINT64_MAX - now_ns < WD_VIDEO_GPU_CAPTURE_BACKOFF_NS)
    {
        return UINT64_MAX;
    }
    return now_ns + WD_VIDEO_GPU_CAPTURE_BACKOFF_NS;
}
