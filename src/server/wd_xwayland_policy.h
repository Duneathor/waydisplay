#pragma once

#include <stdbool.h>
#include <stdint.h>

/* X11 accepts tiny managed utility windows. Only zero is an invalid size. */
static inline uint16_t wd_xwayland_valid_dimension(uint16_t requested, uint16_t fallback) {
    return requested ? requested : fallback;
}

static inline bool wd_xwayland_needs_decoration(bool managed, bool map_requested, bool fullscreen) {
    return managed && map_requested && !fullscreen;
}

static inline uint16_t wd_xwayland_content_height(uint16_t logical_height, bool decorated, uint16_t titlebar_height) {
    return decorated && logical_height > titlebar_height ? (uint16_t)(logical_height - titlebar_height) : logical_height;
}

/* Scene-node mutation is needed only when a decoration was first created,
 * its visibility toggles, or the content width changes. */
static inline bool wd_xwayland_decoration_layout_changed(bool valid, uint16_t previous_width,
                                                          bool previous_decorated, uint16_t width,
                                                          bool decorated) {
    return !valid || previous_width != width || previous_decorated != decorated;
}
