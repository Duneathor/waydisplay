#include "wd_frame_pacing.h"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_time.h"

uint16_t wd_frame_rate_normalize_client_request(uint16_t requested_fps) {
    if (requested_fps == 0)
    {
        return WD_DEFAULT_SESSION_FPS;
    }
    if (requested_fps > WD_MAX_SESSION_FPS)
    {
        return WD_MAX_SESSION_FPS;
    }
    return requested_fps;
}

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

void wd_frame_pacing_reset(struct wd_frame_pacing_state* state) {
    if (!state)
    {
        return;
    }
    state->next_deadline_ns = 0;
    state->last_admitted_ns = 0;
    state->capture_fps      = 0;
}

static uint64_t wd_frame_interval_ns(uint16_t capture_fps) {
    if (capture_fps == 0)
    {
        capture_fps = 1;
    }

    /* Round up so the configured FPS remains a hard upper bound. */
    return (WD_NSEC_PER_SEC + (uint64_t)capture_fps - 1u) / (uint64_t)capture_fps;
}

bool wd_frame_pacing_due(struct wd_frame_pacing_state* state, uint64_t now_ns, uint16_t capture_fps) {
    if (!state)
    {
        return false;
    }
    if (capture_fps == 0)
    {
        capture_fps = 1;
    }

    const uint64_t interval_ns = wd_frame_interval_ns(capture_fps);
    if (state->last_admitted_ns == 0)
    {
        state->capture_fps      = capture_fps;
        state->last_admitted_ns = now_ns != 0 ? now_ns : 1u;
        state->next_deadline_ns = UINT64_MAX - now_ns < interval_ns ? UINT64_MAX : now_ns + interval_ns;
        return true;
    }

    if (state->capture_fps != capture_fps)
    {
        state->capture_fps = capture_fps;
        state->next_deadline_ns = UINT64_MAX - state->last_admitted_ns < interval_ns
                                      ? UINT64_MAX
                                      : state->last_admitted_ns + interval_ns;
    }

    if (now_ns < state->next_deadline_ns)
    {
        return false;
    }

    state->last_admitted_ns = now_ns;
    const uint64_t lateness_ns = now_ns - state->next_deadline_ns;
    if (lateness_ns >= interval_ns)
    {
        /* A long idle interval is not pacing debt. Restart from the admitted
         * frame so later wakeups cannot burst trying to catch up. */
        state->next_deadline_ns = UINT64_MAX - now_ns < interval_ns ? UINT64_MAX : now_ns + interval_ns;
    }
    else
    {
        /* Preserve the fractional cadence when ordinary polling wakes a little
         * after the deadline; this avoids accumulating poll jitter. */
        state->next_deadline_ns = UINT64_MAX - state->next_deadline_ns < interval_ns
                                      ? UINT64_MAX
                                      : state->next_deadline_ns + interval_ns;
    }
    return true;
}
