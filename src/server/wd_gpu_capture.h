#pragma once

#include "waydisplay/wd_frame.h"

#include <stdbool.h>
#include <stdint.h>

struct wlr_buffer;

/* Export one wlroots render buffer into an owned DRM PRIME frame. All plane FDs
 * in out_frame are independent duplicates, so the output state may be committed
 * and released immediately after this call. */
bool wd_gpu_capture_export_wlr_buffer(struct wlr_buffer* buffer, uint64_t pts_usec,
                                      struct wd_frame* out_frame);
