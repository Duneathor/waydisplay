#pragma once

#include <stdbool.h>

/* Pure preflight policy. A video-owner readback contributes exactly one
 * outcome to the interval counters, without per-frame logging. */
enum wd_video_snapshot_decision {
    WD_VIDEO_SNAPSHOT_ACCEPT,
    WD_VIDEO_SNAPSHOT_UNAVAILABLE,
    WD_VIDEO_SNAPSHOT_PENDING_SEND,
};

static inline enum wd_video_snapshot_decision wd_video_snapshot_preflight_decide(bool available, bool pending_send) {
    if (!available) return WD_VIDEO_SNAPSHOT_UNAVAILABLE;
    if (pending_send) return WD_VIDEO_SNAPSHOT_PENDING_SEND;
    return WD_VIDEO_SNAPSHOT_ACCEPT;
}

/* The denominator is compositor readbacks considered while video is selected,
 * not tile-mode readbacks or the configured FPS. */
static inline double wd_video_snapshot_admission_percent(unsigned long long accepted,
                                                          unsigned long long considered) {
    return considered ? 100.0 * (double)accepted / (double)considered : 0.0;
}
