#include "wd_server.h"

#include <string.h>

#include <drm_fourcc.h>

bool wd_render_scene_and_readback_xrgb8888(struct wd_server *server) {
    if (!server ||
        !server->scene_output ||
        !server->output ||
        !server->renderer ||
        !server->framebuffer_xrgb8888) {
        return false;
        }

        struct wlr_output_state state;
    wlr_output_state_init(&state);

    bool ok = false;

    if (!wlr_scene_output_build_state(server->scene_output, &state, NULL)) {
        wlr_log(WLR_ERROR, "WayDisplay: wlr_scene_output_build_state failed");
        goto out;
    }

    if (!(state.committed & WLR_OUTPUT_STATE_BUFFER) || state.buffer == NULL) {
        wlr_log(WLR_DEBUG,
                "WayDisplay: scene output state did not contain a rendered buffer");
        goto commit_only;
    }

    int read_width =
    state.buffer->width < (int)WD_DISPLAY_WIDTH
    ? state.buffer->width
    : (int)WD_DISPLAY_WIDTH;

    int read_height =
    state.buffer->height < (int)WD_DISPLAY_HEIGHT
    ? state.buffer->height
    : (int)WD_DISPLAY_HEIGHT;

    if (read_width <= 0 || read_height <= 0) {
        wlr_log(WLR_ERROR,
                "WayDisplay: invalid readback buffer size %dx%d",
                state.buffer->width,
                state.buffer->height);
        goto commit_only;
    }

    if (state.buffer->width != (int)WD_DISPLAY_WIDTH ||
        state.buffer->height != (int)WD_DISPLAY_HEIGHT) {
        static bool warned_size_mismatch = false;

    if (!warned_size_mismatch) {
        wlr_log(WLR_INFO,
                "WayDisplay: clamping readback from buffer %dx%d to %dx%d inside %ux%u framebuffer",
                state.buffer->width,
                state.buffer->height,
                read_width,
                read_height,
                WD_DISPLAY_WIDTH,
                WD_DISPLAY_HEIGHT);
        warned_size_mismatch = true;
    }
        }

        memset(server->framebuffer_xrgb8888, 0, WD_FRAMEBUFFER_BYTES);

        struct wlr_texture *texture =
        wlr_texture_from_buffer(server->renderer, state.buffer);

        if (!texture) {
            wlr_log(WLR_ERROR, "WayDisplay: wlr_texture_from_buffer failed");
            goto commit_only;
        }

        struct wlr_texture_read_pixels_options read_options = {
            .data = server->framebuffer_xrgb8888,
            .format = DRM_FORMAT_XRGB8888,
            .stride = WD_DISPLAY_WIDTH * WD_BYTES_PER_PIXEL,
            .dst_x = 0,
            .dst_y = 0,
            .src_box = {
                .x = 0,
                .y = 0,
                .width = read_width,
                .height = read_height,
            },
        };

        if (!wlr_texture_read_pixels(texture, &read_options)) {
            uint32_t preferred = wlr_texture_preferred_read_format(texture);

            wlr_log(WLR_ERROR,
                    "WayDisplay: wlr_texture_read_pixels(XRGB8888) failed; preferred DRM format is 0x%08x",
                    preferred);

            wlr_texture_destroy(texture);
            goto commit_only;
        }

        wlr_texture_destroy(texture);
        ok = true;

        commit_only:
        if (!wlr_output_commit_state(server->output, &state)) {
            wlr_log(WLR_ERROR, "WayDisplay: wlr_output_commit_state failed");
            ok = false;
        }

        out:
        wlr_output_state_finish(&state);
        return ok;
}
