#pragma once

#include "waydisplay/wd_buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_FRAME_MAX_PLANES 4u

enum wd_frame_storage {
    WD_FRAME_STORAGE_NONE = 0,
    WD_FRAME_STORAGE_CPU_XRGB8888 = 1,
    WD_FRAME_STORAGE_DRM_PRIME = 2,
};

struct wd_frame_drm_plane {
    int      fd;
    uint32_t stride;
    uint32_t offset;
    uint64_t modifier;
};

struct wd_frame {
    enum wd_frame_storage storage;
    uint32_t              width;
    uint32_t              height;
    uint32_t              fourcc;
    uint64_t              pts_usec;

    union {
        struct {
            struct wd_buffer* buffer;
            size_t            offset;
            uint32_t          stride_bytes;
        } cpu;

        struct {
            uint32_t                  plane_count;
            struct wd_frame_drm_plane planes[WD_FRAME_MAX_PLANES];
        } drm;
    } data;
};

void wd_frame_init(struct wd_frame* frame);
void wd_frame_reset(struct wd_frame* frame);

bool wd_frame_set_cpu_xrgb8888(struct wd_frame* frame, struct wd_buffer* buffer, size_t offset,
                               uint32_t width, uint32_t height, uint32_t stride_bytes,
                               uint32_t fourcc, uint64_t pts_usec);

/* Duplicate all supplied descriptors with close-on-exec ownership. The caller
 * retains ownership of the input descriptors. */
bool wd_frame_set_drm_prime_dup(struct wd_frame* frame, uint32_t width, uint32_t height,
                                uint32_t fourcc, uint64_t pts_usec,
                                const struct wd_frame_drm_plane* planes, uint32_t plane_count);

bool wd_frame_clone(struct wd_frame* destination, const struct wd_frame* source);
bool wd_frame_valid(const struct wd_frame* frame);

const uint8_t* wd_frame_cpu_data(const struct wd_frame* frame);

#ifdef __cplusplus
}
#endif
