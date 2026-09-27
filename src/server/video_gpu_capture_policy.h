#pragma once

#include "waydisplay/wd_frame.h"

#include <stdbool.h>
#include <stdint.h>

#define WD_DRM_FOURCC_XRGB8888 0x34325258u
#define WD_DRM_FOURCC_ARGB8888 0x34325241u

static inline bool wd_video_gpu_capture_frame_eligible(const struct wd_frame* frame,
                                                        uint32_t width,
                                                        uint32_t height) {
    if (!frame || !wd_frame_valid(frame) ||
        frame->storage != WD_FRAME_STORAGE_DRM_PRIME ||
        frame->width != width || frame->height != height ||
        frame->data.drm.plane_count != 1)
    {
        return false;
    }

    if (frame->fourcc != WD_DRM_FOURCC_XRGB8888 &&
        frame->fourcc != WD_DRM_FOURCC_ARGB8888)
    {
        return false;
    }

    const struct wd_frame_drm_plane* plane = &frame->data.drm.planes[0];
    return plane->stride >= width * 4u;
}
