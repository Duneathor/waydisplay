#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "wd_server_internal.h"
#include "wd_readback_regions.h"

#include <drm_fourcc.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>

#define WD_READBACK_REGION_CAPACITY 16u

static uint32_t xrgb_from_pixel(uint32_t pixel, uint32_t format) {
    switch (format)
    {
    case DRM_FORMAT_XRGB8888:
        return pixel | 0xff000000u;

    case DRM_FORMAT_ARGB8888:
        return pixel | 0xff000000u;

    case DRM_FORMAT_XBGR8888: {
        uint32_t r = pixel & 0x000000ffu;
        uint32_t g = pixel & 0x0000ff00u;
        uint32_t b = pixel & 0x00ff0000u;
        return 0xff000000u | (r << 16) | g | (b >> 16);
    }

    case DRM_FORMAT_ABGR8888: {
        uint32_t r = pixel & 0x000000ffu;
        uint32_t g = pixel & 0x0000ff00u;
        uint32_t b = pixel & 0x00ff0000u;
        return 0xff000000u | (r << 16) | g | (b >> 16);
    }

    default:
        return 0xff000000u;
    }
}

static bool readback_buffer_data_ptr_xrgb8888(struct wd_server* server, struct wlr_buffer* buffer, int read_width, int read_height) {
    if (!server || !buffer || !server->framebuffer_xrgb8888 || read_width <= 0 || read_height <= 0)
    {
        return false;
    }

    void*    data   = NULL;
    uint32_t format = 0;
    size_t   stride = 0;

    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride))
    {
        return false;
    }

    bool ok = false;

    switch (format)
    {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
        ok = true;
        break;

    default: {
        static uint64_t last_log_ns = 0;
        uint64_t        now         = wd_now_ns();

        if (wd_log_rate_limit_should_log(&last_log_ns, now, WD_LOG_RATE_LIMIT_INTERVAL_NS))
        {
            WD_LOG_ERROR("data-ptr readback unsupported DRM format 0x%08x", format);
        }
        break;
    }
    }

    if (ok)
    {
        int copy_width  = read_width;
        int copy_height = read_height;

        if (copy_width > buffer->width)
        {
            copy_width = buffer->width;
        }

        if (copy_height > buffer->height)
        {
            copy_height = buffer->height;
        }

        if (copy_width < (int)server->display_width || copy_height < (int)server->display_height)
        {
            memset(server->framebuffer_xrgb8888, 0, server->framebuffer_bytes);
        }

        for (int y = 0; y < copy_height; ++y)
        {
            const uint32_t* src = (const uint32_t*)((const uint8_t*)data + (size_t)y * stride);
            uint32_t*       dst = server->framebuffer_xrgb8888 + (size_t)y * server->display_width;

            for (int x = 0; x < copy_width; ++x)
            {
                dst[x] = xrgb_from_pixel(src[x], format);
            }
        }
    }

    wlr_buffer_end_data_ptr_access(buffer);
    return ok;
}

static void merge_wlroots_output_damage(struct wd_server* server, struct wlr_output_state* state) {
    if (!server || !state)
    {
        return;
    }

    if ((state->committed & WLR_OUTPUT_STATE_DAMAGE) == 0)
    {
        /* A rendered buffer without an explicit damage region must be treated
         * as a complete replacement. */
        wd_server_mark_scene_dirty(server);
        return;
    }

    int                       rect_count = 0;
    const pixman_box32_t*     rects      = pixman_region32_rectangles(&state->damage, &rect_count);
    for (int i = 0; i < rect_count; ++i)
    {
        const int width  = rects[i].x2 - rects[i].x1;
        const int height = rects[i].y2 - rects[i].y1;
        if (width > 0 && height > 0)
        {
            wd_server_mark_rect_dirty(server, rects[i].x1, rects[i].y1, width, height);
        }
    }
}

enum wd_render_result wd_render_scene_and_readback_xrgb8888(struct wd_server* server) {
    if (!server || !server->scene_output || !server->output || !server->renderer || !server->framebuffer_xrgb8888)
    {
        return WD_RENDER_RESULT_ERROR;
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);

    enum wd_render_result result                  = WD_RENDER_RESULT_ERROR;
    bool                  built_state             = false;

    if (!wlr_scene_output_build_state(server->scene_output, &state, NULL))
    {
        static uint64_t last_log_ns = 0;
        uint64_t        now         = wd_now_ns();

        if (wd_log_rate_limit_should_log(&last_log_ns, now, WD_LOG_RATE_LIMIT_INTERVAL_NS))
        {
            WD_LOG_ERROR("wlr_scene_output_build_state failed");
        }

        goto out;
    }

    built_state = true;

    if (!(state.committed & WLR_OUTPUT_STATE_BUFFER) || state.buffer == NULL)
    {
        /*
         * No scene damage was rendered for this output state. This is not a
         * readback failure: callers must distinguish an idle scene from a
         * renderer/backend error so they do not break the frame-callback loop.
         */
        result = WD_RENDER_RESULT_IDLE;
        goto commit_only;
    }

    merge_wlroots_output_damage(server, &state);

    int read_width = state.buffer->width < (int)server->display_width ? state.buffer->width : (int)server->display_width;

    int read_height = state.buffer->height < (int)server->display_height ? state.buffer->height : (int)server->display_height;

    int full_read_width  = read_width;
    int full_read_height = read_height;

    if (read_width <= 0 || read_height <= 0)
    {
        static uint64_t last_log_ns = 0;
        uint64_t        now         = wd_now_ns();

        if (wd_log_rate_limit_should_log(&last_log_ns, now, WD_LOG_RATE_LIMIT_INTERVAL_NS))
        {
            WD_LOG_ERROR("invalid readback buffer size %dx%d", state.buffer->width, state.buffer->height);
        }

        goto commit_only;
    }

    struct wd_readback_region read_regions[WD_READBACK_REGION_CAPACITY];
    bool                      full_readback = true;
    const size_t read_region_count = wd_readback_plan_regions(
        server->damage_all_tiles, server->damage_tiles, server->damage_tile_count, server->total_base_tiles,
        server->base_tiles_x, server->base_tile_width, server->base_tile_height, read_width, read_height,
        read_regions, WD_READBACK_REGION_CAPACITY, &full_readback);
    if (read_region_count == 0)
    {
        goto commit_only;
    }

    if (full_readback && (full_read_width < (int)server->display_width || full_read_height < (int)server->display_height))
    {
        memset(server->framebuffer_xrgb8888, 0, server->framebuffer_bytes);
    }

    struct wlr_texture* texture = wlr_texture_from_buffer(server->renderer, state.buffer);

    if (!texture)
    {
        static uint64_t last_log_ns = 0;

        if (readback_buffer_data_ptr_xrgb8888(server, state.buffer, full_read_width, full_read_height))
        {
            result                  = WD_RENDER_RESULT_FRAME;
            goto commit_only;
        }

        uint64_t now = wd_now_ns();

        if (wd_log_rate_limit_should_log(&last_log_ns, now, WD_LOG_RATE_LIMIT_INTERVAL_NS))
        {
            WD_LOG_ERROR("wlr_texture_from_buffer failed for buffer %dx%d", state.buffer->width, state.buffer->height);
        }

        goto commit_only;
    }

    bool readback_ok = true;
    for (size_t i = 0; i < read_region_count; ++i)
    {
        const struct wd_readback_region* region = &read_regions[i];
        struct wlr_texture_read_pixels_options read_options = {
            .data   = server->framebuffer_xrgb8888,
            .format = DRM_FORMAT_XRGB8888,
            .stride = server->display_width * WD_BYTES_PER_PIXEL,
            .dst_x  = region->x,
            .dst_y  = region->y,
            .src_box =
                {
                    .x      = region->x,
                    .y      = region->y,
                    .width  = region->width,
                    .height = region->height,
                },
        };

        if (!wlr_texture_read_pixels(texture, &read_options))
        {
            readback_ok = false;
            break;
        }
    }

    if (!readback_ok)
    {
        uint32_t preferred = wlr_texture_preferred_read_format(texture);

        static uint64_t last_log_ns = 0;
        uint64_t        now         = wd_now_ns();

        if (wd_log_rate_limit_should_log(&last_log_ns, now, WD_LOG_RATE_LIMIT_INTERVAL_NS))
        {
            WD_LOG_ERROR("wlr_texture_read_pixels(XRGB8888) failed; preferred DRM "
                         "format is 0x%08x",
                         preferred);
        }

        wlr_texture_destroy(texture);
        goto commit_only;
    }

    wlr_texture_destroy(texture);
    result                  = WD_RENDER_RESULT_FRAME;

commit_only:
    if (built_state)
    {
        if (!wlr_output_commit_state(server->output, &state))
        {
            static uint64_t last_log_ns = 0;
            uint64_t        now         = wd_now_ns();

            if (wd_log_rate_limit_should_log(&last_log_ns, now, WD_LOG_RATE_LIMIT_INTERVAL_NS))
            {
                WD_LOG_ERROR("wlr_output_commit_state failed");
            }

            result = WD_RENDER_RESULT_ERROR;
        }
    }

out:
    wlr_output_state_finish(&state);

    return result;
}
