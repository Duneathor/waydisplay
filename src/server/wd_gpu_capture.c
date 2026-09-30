#include "wd_gpu_capture.h"

#include <wlr/render/dmabuf.h>
#include <wlr/types/wlr_buffer.h>

bool wd_gpu_capture_export_wlr_buffer(struct wlr_buffer* buffer, uint64_t pts_usec,
                                      struct wd_frame* out_frame) {
    if (!buffer || !out_frame)
    {
        return false;
    }

    struct wlr_dmabuf_attributes attributes;
    if (!wlr_buffer_get_dmabuf(buffer, &attributes))
    {
        return false;
    }

    bool ok = false;
    if (attributes.width > 0 && attributes.height > 0 && attributes.n_planes > 0 &&
        attributes.n_planes <= (int)WD_FRAME_MAX_PLANES)
    {
        struct wd_frame_drm_plane planes[WD_FRAME_MAX_PLANES];
        for (int i = 0; i < attributes.n_planes; ++i)
        {
            planes[i].fd       = attributes.fd[i];
            planes[i].stride   = attributes.stride[i];
            planes[i].offset   = attributes.offset[i];
            planes[i].modifier = attributes.modifier;
        }

        ok = wd_frame_set_drm_prime_dup(out_frame, (uint32_t)attributes.width,
                                        (uint32_t)attributes.height,
                                        attributes.format, pts_usec, planes,
                                        (uint32_t)attributes.n_planes);
    }

    return ok;
}
