#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Main/compositor-thread counters transferred to network stats on the health tick.
 * x11_* counts mapped Xwayland root surfaces (Wine and other X11 clients).
 * All other counters cover the entire composed output, including native Wayland.
 * x11_commit_bounds_pixels sums buffer bounds, NOT unique damaged pixels.
 * texture_read_* measures wlr_texture_read_pixels attempts, excluding the
 * separate buffer-data fallback path. */
struct wd_compositor_capture_stats {
    uint64_t x11_surface_commits;
    uint64_t x11_commit_bounds_pixels;
    uint64_t x11_maps;
    uint64_t x11_unmaps;
    uint64_t x11_configures;
    uint64_t x11_decoration_layout_updates;
    uint64_t x11_decoration_layout_reused;
    uint64_t scene_build_calls;
    uint64_t scene_build_ns;
    uint64_t texture_read_calls;
    uint64_t texture_read_pixels;
    uint64_t texture_read_ns;
    uint64_t texture_read_failures;
    uint64_t buffer_data_fallbacks;
};

static inline void wd_compositor_capture_x11_surface_commit(struct wd_compositor_capture_stats* s, int width, int height) {
    if (!s)
    {
        return;
    }
    ++s->x11_surface_commits;
    if (width > 0 && height > 0)
    {
        s->x11_commit_bounds_pixels += (uint64_t)(unsigned)width * (uint64_t)(unsigned)height;
    }
}

static inline void wd_compositor_capture_scene_build(struct wd_compositor_capture_stats* s, uint64_t duration_ns) {
    if (!s)
    {
        return;
    }
    ++s->scene_build_calls;
    s->scene_build_ns += duration_ns;
}

static inline void wd_compositor_capture_texture_read(struct wd_compositor_capture_stats* s, uint32_t width, uint32_t height,
                                                       uint64_t duration_ns, bool ok) {
    if (!s)
    {
        return;
    }
    ++s->texture_read_calls;
    s->texture_read_pixels += (uint64_t)width * (uint64_t)height;
    s->texture_read_ns += duration_ns;
    if (!ok)
    {
        ++s->texture_read_failures;
    }
}
