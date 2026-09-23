#pragma once

#include <stdbool.h>

/* Tile shadow can only be skipped when video owns the displayed content and
 * no full-refresh, reconfiguration or tile transition is pending. */
static inline bool wd_video_shadow_skip_diff(bool video_owns_display, bool force_refresh,
                                             bool tile_refresh_pending, bool config_update_pending) {
    return video_owns_display && !force_refresh && !tile_refresh_pending && !config_update_pending;
}
