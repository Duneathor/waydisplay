#pragma once

#include <stdbool.h>

/* Tile shadow can only be skipped when video owns the displayed content and
 * no full-refresh, reconfiguration or tile transition is pending. */
static inline bool wd_video_shadow_skip_diff(bool video_owns_display, bool force_refresh,
                                             bool tile_refresh_pending, bool config_update_pending) {
    return video_owns_display && !force_refresh && !tile_refresh_pending && !config_update_pending;
}

/* A GPU-only render is valid input while video owns the display, but it may
 * never be reinterpreted as a CPU-backed tile diff after an ownership change. */
static inline bool wd_video_shadow_cpu_capture_required(bool video_owns_display, bool force_refresh,
                                                        bool tile_refresh_pending, bool config_update_pending,
                                                        bool cpu_framebuffer_refreshed) {
    return !wd_video_shadow_skip_diff(video_owns_display, force_refresh, tile_refresh_pending, config_update_pending) &&
           !cpu_framebuffer_refreshed;
}
