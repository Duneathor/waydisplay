#include "waydisplay/wd_frame.h"

#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

static int wd_frame_dup_cloexec(int fd) {
    if (fd < 0)
    {
        return -1;
    }
#ifdef F_DUPFD_CLOEXEC
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
    int duplicate = dup(fd);
    if (duplicate < 0)
    {
        return -1;
    }

    const int flags = fcntl(duplicate, F_GETFD);
    if (flags < 0 || fcntl(duplicate, F_SETFD, flags | FD_CLOEXEC) < 0)
    {
        close(duplicate);
        return -1;
    }
    return duplicate;
#endif
}

void wd_frame_init(struct wd_frame* frame) {
    if (!frame)
    {
        return;
    }
    memset(frame, 0, sizeof(*frame));
    for (uint32_t i = 0; i < WD_FRAME_MAX_PLANES; ++i)
    {
        frame->data.drm.planes[i].fd = -1;
    }
}

void wd_frame_reset(struct wd_frame* frame) {
    if (!frame)
    {
        return;
    }

    if (frame->storage == WD_FRAME_STORAGE_CPU_XRGB8888)
    {
        wd_buffer_release(frame->data.cpu.buffer);
    }
    else if (frame->storage == WD_FRAME_STORAGE_DRM_PRIME)
    {
        const uint32_t count = frame->data.drm.plane_count < WD_FRAME_MAX_PLANES
                                   ? frame->data.drm.plane_count
                                   : WD_FRAME_MAX_PLANES;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (frame->data.drm.planes[i].fd >= 0)
            {
                close(frame->data.drm.planes[i].fd);
            }
        }
    }

    wd_frame_init(frame);
}

static bool wd_frame_cpu_range_valid(const struct wd_buffer* buffer, size_t offset,
                                     uint32_t width, uint32_t height, uint32_t stride_bytes) {
    const uint64_t row_bytes = (uint64_t)width * 4u;
    if (!buffer || width == 0 || height == 0 || row_bytes > stride_bytes)
    {
        return false;
    }
    if (height > 1u && (size_t)(height - 1u) > (SIZE_MAX - (size_t)row_bytes) / stride_bytes)
    {
        return false;
    }
    const size_t bytes = (size_t)(height - 1u) * stride_bytes + (size_t)row_bytes;
    return offset <= wd_buffer_size(buffer) && bytes <= wd_buffer_size(buffer) - offset;
}

bool wd_frame_set_cpu_xrgb8888(struct wd_frame* frame, struct wd_buffer* buffer, size_t offset,
                               uint32_t width, uint32_t height, uint32_t stride_bytes,
                               uint32_t fourcc, uint64_t pts_usec) {
    if (!frame || !wd_frame_cpu_range_valid(buffer, offset, width, height, stride_bytes))
    {
        return false;
    }

    struct wd_buffer* retained = wd_buffer_retain(buffer);
    if (!retained)
    {
        return false;
    }

    wd_frame_reset(frame);
    frame->storage               = WD_FRAME_STORAGE_CPU_XRGB8888;
    frame->width                 = width;
    frame->height                = height;
    frame->fourcc                = fourcc;
    frame->pts_usec              = pts_usec;
    frame->data.cpu.buffer       = retained;
    frame->data.cpu.offset       = offset;
    frame->data.cpu.stride_bytes = stride_bytes;
    return true;
}

bool wd_frame_set_drm_prime_dup(struct wd_frame* frame, uint32_t width, uint32_t height,
                                uint32_t fourcc, uint64_t pts_usec,
                                const struct wd_frame_drm_plane* planes, uint32_t plane_count) {
    if (!frame || !planes || width == 0 || height == 0 || plane_count == 0 ||
        plane_count > WD_FRAME_MAX_PLANES)
    {
        return false;
    }

    struct wd_frame temporary;
    wd_frame_init(&temporary);
    temporary.storage              = WD_FRAME_STORAGE_DRM_PRIME;
    temporary.width                = width;
    temporary.height               = height;
    temporary.fourcc               = fourcc;
    temporary.pts_usec             = pts_usec;
    temporary.data.drm.plane_count = plane_count;

    for (uint32_t i = 0; i < plane_count; ++i)
    {
        if (planes[i].fd < 0 || planes[i].stride == 0)
        {
            wd_frame_reset(&temporary);
            return false;
        }
        temporary.data.drm.planes[i] = planes[i];
        temporary.data.drm.planes[i].fd = wd_frame_dup_cloexec(planes[i].fd);
        if (temporary.data.drm.planes[i].fd < 0)
        {
            wd_frame_reset(&temporary);
            return false;
        }
    }

    wd_frame_reset(frame);
    *frame = temporary;
    return true;
}

bool wd_frame_clone(struct wd_frame* destination, const struct wd_frame* source) {
    if (!destination || !source || destination == source || !wd_frame_valid(source))
    {
        return false;
    }

    if (source->storage == WD_FRAME_STORAGE_CPU_XRGB8888)
    {
        return wd_frame_set_cpu_xrgb8888(destination, source->data.cpu.buffer,
                                         source->data.cpu.offset, source->width, source->height,
                                         source->data.cpu.stride_bytes, source->fourcc,
                                         source->pts_usec);
    }

    return wd_frame_set_drm_prime_dup(destination, source->width, source->height, source->fourcc,
                                      source->pts_usec, source->data.drm.planes,
                                      source->data.drm.plane_count);
}

bool wd_frame_valid(const struct wd_frame* frame) {
    if (!frame || frame->width == 0 || frame->height == 0)
    {
        return false;
    }

    if (frame->storage == WD_FRAME_STORAGE_CPU_XRGB8888)
    {
        return wd_frame_cpu_range_valid(frame->data.cpu.buffer, frame->data.cpu.offset,
                                        frame->width, frame->height,
                                        frame->data.cpu.stride_bytes);
    }

    if (frame->storage == WD_FRAME_STORAGE_DRM_PRIME)
    {
        if (frame->data.drm.plane_count == 0 || frame->data.drm.plane_count > WD_FRAME_MAX_PLANES)
        {
            return false;
        }
        for (uint32_t i = 0; i < frame->data.drm.plane_count; ++i)
        {
            if (frame->data.drm.planes[i].fd < 0 || frame->data.drm.planes[i].stride == 0)
            {
                return false;
            }
        }
        return true;
    }

    return false;
}

const uint8_t* wd_frame_cpu_data(const struct wd_frame* frame) {
    if (!frame || frame->storage != WD_FRAME_STORAGE_CPU_XRGB8888 || !wd_frame_valid(frame))
    {
        return NULL;
    }
    const uint8_t* bytes = wd_buffer_const_data(frame->data.cpu.buffer);
    return bytes ? bytes + frame->data.cpu.offset : NULL;
}
