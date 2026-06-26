#include "wd_frame_pacing.h"

#include "waydisplay/wd_time.h"

uint32_t wd_frame_service_interval_ms(uint16_t capture_fps, uint32_t minimum_ms, uint32_t maximum_ms) {
    if (minimum_ms == 0)
    {
        minimum_ms = 1;
    }
    if (maximum_ms < minimum_ms)
    {
        maximum_ms = minimum_ms;
    }
    if (capture_fps == 0)
    {
        capture_fps = 1;
    }

    uint32_t interval_ms = 1000u / (uint32_t)capture_fps;
    if (interval_ms < minimum_ms)
    {
        interval_ms = minimum_ms;
    }
    if (interval_ms > maximum_ms)
    {
        interval_ms = maximum_ms;
    }
    return interval_ms;
}

bool wd_frame_pacing_due(uint64_t last_frame_ns, uint64_t now_ns, uint16_t capture_fps, uint32_t service_interval_ms) {
    if (last_frame_ns == 0 || now_ns < last_frame_ns)
    {
        return true;
    }
    if (capture_fps == 0)
    {
        capture_fps = 1;
    }

    const uint64_t interval_ns  = WD_NSEC_PER_SEC / (uint64_t)capture_fps;
    const uint64_t tolerance_ns = (uint64_t)service_interval_ms * WD_NSEC_PER_MSEC;
    const uint64_t elapsed_ns   = now_ns - last_frame_ns;
    return elapsed_ns >= interval_ns || tolerance_ns >= interval_ns - elapsed_ns;
}
