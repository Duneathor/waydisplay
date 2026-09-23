#pragma once

#include <stdbool.h>

/* X11 map requests do not guarantee that an associated Wayland surface is mapped. */
static inline bool wd_xwayland_should_show(bool associated, bool surface_mapped, bool minimized) {
    return associated && surface_mapped && !minimized;
}
