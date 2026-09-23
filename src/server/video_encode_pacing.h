#pragma once

#include <stdint.h>

/* Capture-only pacing for slow *software AV1*.  Preserve the negotiated
 * encoder framerate/codec context and the tiles path.  Ignore the first
 * few interframes and all keyframes so startup/intra cost does not force
 * a long-lived low-FPS mode. The encoder worker supplies samples under
 * net->lock, and the compositor reads the cap under that same lock. */
#define WD_VIDEO_ENCODE_PACING_WARMUP_SAMPLES 4u
#define WD_VIDEO_ENCODE_PACING_MIN_FPS 5u
#define WD_VIDEO_ENCODE_PACING_HEADROOM_PERCENT 85u

static inline uint64_t wd_video_encode_pacing_ewma(uint64_t previous_ns, uint64_t sample_ns) {
    if (sample_ns == 0)
    {
        return previous_ns;
    }
    if (previous_ns == 0)
    {
        return sample_ns;
    }
    return sample_ns >= previous_ns ? previous_ns + (sample_ns - previous_ns) / 4u
                                    : previous_ns - (previous_ns - sample_ns) / 4u;
}

static inline uint16_t wd_video_encode_pacing_cap(uint16_t requested_fps, uint64_t avg_ns, uint32_t samples) {
    if (requested_fps == 0 || avg_ns == 0 || samples < WD_VIDEO_ENCODE_PACING_WARMUP_SAMPLES)
    {
        return requested_fps;
    }
    const uint64_t safe_fps = (UINT64_C(1000000000) * WD_VIDEO_ENCODE_PACING_HEADROOM_PERCENT / 100u) / avg_ns;
    if (safe_fps >= requested_fps)
    {
        return requested_fps;
    }
    return safe_fps < WD_VIDEO_ENCODE_PACING_MIN_FPS ? WD_VIDEO_ENCODE_PACING_MIN_FPS : (uint16_t)safe_fps;
}
