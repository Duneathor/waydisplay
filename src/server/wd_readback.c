#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "wd_server.h"

#include <drm_fourcc.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>

static void readback_box_from_damage(struct wd_server* server, int max_width, int max_height, int* out_x, int* out_y, int* out_width,
                                     int* out_height, bool* out_full_readback) {
    int  x1   = 0;
    int  y1   = 0;
    int  x2   = max_width;
    int  y2   = max_height;
    bool full = true;

    if (server && !server->damage_all_tiles && server->damage_tiles && server->damage_tile_count > 0)
    {
        bool have = false;

        for (uint16_t tile_id = 0; tile_id < server->total_tiles; ++tile_id)
        {
            if (!server->damage_tiles[tile_id])
            {
                continue;
            }

            int tx1 = (int)wd_tile_start_x_for_tile(tile_id, server->tiles_x, server->tile_width);
            int ty1 = (int)wd_tile_start_y_for_tile(tile_id, server->tiles_x, server->tile_height);
            int tx2 = tx1 + (int)wd_tile_visible_width_for_tile(server->display_width, tile_id, server->tiles_x, server->tile_width);
            int ty2 = ty1 + (int)wd_tile_visible_height_for_tile(server->display_height, tile_id, server->tiles_x, server->tile_height);

            if (tx1 < 0)
            {
                tx1 = 0;
            }
            if (ty1 < 0)
            {
                ty1 = 0;
            }
            if (tx2 > max_width)
            {
                tx2 = max_width;
            }
            if (ty2 > max_height)
            {
                ty2 = max_height;
            }
            if (tx1 >= tx2 || ty1 >= ty2)
            {
                continue;
            }

            if (!have)
            {
                x1   = tx1;
                y1   = ty1;
                x2   = tx2;
                y2   = ty2;
                have = true;
            }
            else
            {
                if (tx1 < x1)
                {
                    x1 = tx1;
                }
                if (ty1 < y1)
                {
                    y1 = ty1;
                }
                if (tx2 > x2)
                {
                    x2 = tx2;
                }
                if (ty2 > y2)
                {
                    y2 = ty2;
                }
            }
        }

        if (have)
        {
            full = false;
        }
    }

    if (x1 < 0)
    {
        x1 = 0;
    }
    if (y1 < 0)
    {
        y1 = 0;
    }
    if (x2 > max_width)
    {
        x2 = max_width;
    }
    if (y2 > max_height)
    {
        y2 = max_height;
    }
    if (x1 >= x2 || y1 >= y2)
    {
        x1   = 0;
        y1   = 0;
        x2   = max_width;
        y2   = max_height;
        full = true;
    }

    *out_x             = x1;
    *out_y             = y1;
    *out_width         = x2 - x1;
    *out_height        = y2 - y1;
    *out_full_readback = full;
}

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

        if (now - last_log_ns > 1000000000ull)
        {
            WD_LOG_ERROR("WayDisplay: data-ptr readback unsupported DRM format 0x%08x", format);
            last_log_ns = now;
        }
        break;
    }
    }

    if (ok)
    {
        memset(server->framebuffer_xrgb8888, 0, server->framebuffer_bytes);

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

bool wd_render_scene_and_readback_xrgb8888(struct wd_server* server) {
    if (!server || !server->scene_output || !server->output || !server->renderer || !server->framebuffer_xrgb8888)
    {
        return false;
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);

    bool readback_ok = false;
    bool built_state = false;

    if (!wlr_scene_output_build_state(server->scene_output, &state, NULL))
    {
        static uint64_t last_log_ns = 0;
        uint64_t        now         = wd_now_ns();

        if (now - last_log_ns > 1000000000ull)
        {
            WD_LOG_ERROR("WayDisplay: wlr_scene_output_build_state failed");
            last_log_ns = now;
        }

        goto out;
    }

    built_state = true;

    if (!(state.committed & WLR_OUTPUT_STATE_BUFFER) || state.buffer == NULL)
    {
        /*
         * Nothing to read. This can happen during transient surface churn.
         */
        goto commit_only;
    }

    int read_width = state.buffer->width < (int)server->display_width ? state.buffer->width : (int)server->display_width;

    int read_height = state.buffer->height < (int)server->display_height ? state.buffer->height : (int)server->display_height;

    int full_read_width  = read_width;
    int full_read_height = read_height;

    if (read_width <= 0 || read_height <= 0)
    {
        static uint64_t last_log_ns = 0;
        uint64_t        now         = wd_now_ns();

        if (now - last_log_ns > 1000000000ull)
        {
            WD_LOG_ERROR("WayDisplay: invalid readback buffer size %dx%d", state.buffer->width, state.buffer->height);
            last_log_ns = now;
        }

        goto commit_only;
    }

    int  read_x        = 0;
    int  read_y        = 0;
    bool full_readback = true;
    readback_box_from_damage(server, read_width, read_height, &read_x, &read_y, &read_width, &read_height, &full_readback);

    if (full_readback)
    {
        memset(server->framebuffer_xrgb8888, 0, server->framebuffer_bytes);
    }

    struct wlr_texture* texture = wlr_texture_from_buffer(server->renderer, state.buffer);

    if (!texture)
    {
        static uint64_t last_log_ns = 0;

        if (readback_buffer_data_ptr_xrgb8888(server, state.buffer, full_read_width, full_read_height))
        {
            readback_ok = true;
            goto commit_only;
        }

        uint64_t now = wd_now_ns();

        if (now - last_log_ns > 1000000000ull)
        {
            WD_LOG_ERROR("WayDisplay: wlr_texture_from_buffer failed for buffer %dx%d", state.buffer->width, state.buffer->height);
            last_log_ns = now;
        }

        goto commit_only;
    }

    struct wlr_texture_read_pixels_options read_options = {
        .data   = server->framebuffer_xrgb8888,
        .format = DRM_FORMAT_XRGB8888,
        .stride = server->display_width * WD_BYTES_PER_PIXEL,
        .dst_x  = read_x,
        .dst_y  = read_y,
        .src_box =
            {
                .x      = read_x,
                .y      = read_y,
                .width  = read_width,
                .height = read_height,
            },
    };

    if (!wlr_texture_read_pixels(texture, &read_options))
    {
        uint32_t preferred = wlr_texture_preferred_read_format(texture);

        static uint64_t last_log_ns = 0;
        uint64_t        now         = wd_now_ns();

        if (now - last_log_ns > 1000000000ull)
        {
            WD_LOG_ERROR("WayDisplay: wlr_texture_read_pixels(XRGB8888) failed; preferred DRM "
                         "format is 0x%08x",
                         preferred);
            last_log_ns = now;
        }

        wlr_texture_destroy(texture);
        goto commit_only;
    }

    wlr_texture_destroy(texture);
    readback_ok = true;

commit_only:
    if (built_state)
    {
        if (!wlr_output_commit_state(server->output, &state))
        {
            static uint64_t last_log_ns = 0;
            uint64_t        now         = wd_now_ns();

            if (now - last_log_ns > 1000000000ull)
            {
                WD_LOG_ERROR("WayDisplay: wlr_output_commit_state failed");
                last_log_ns = now;
            }

            readback_ok = false;
        }
    }

out:
    wlr_output_state_finish(&state);
    return readback_ok;
}
