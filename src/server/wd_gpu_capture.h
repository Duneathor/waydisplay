#pragma once

#include "waydisplay/wd_frame.h"

#include <stdbool.h>
#include <stdint.h>

struct wlr_buffer;

/* Export one wlroots render buffer into an owned DRM PRIME frame. out_frame
 * must already have been initialized with wd_frame_init(); on success its prior
 * contents are released and replaced. All plane FDs in out_frame are independent
 * duplicates. The attributes returned by wlr_buffer_get_dmabuf() are borrowed
 * from wlroots and remain owned by the source wlr_buffer. */
bool wd_gpu_capture_export_wlr_buffer(struct wlr_buffer* buffer, uint64_t pts_usec,
                                      struct wd_frame* out_frame);
