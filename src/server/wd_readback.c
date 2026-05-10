#include "wd_server.h"

#include <string.h>

#include <drm_fourcc.h>

#include "waydisplay/wd_time.h"

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

    bool readback_ok = false;
    bool built_state = false;

    if (!wlr_scene_output_build_state(server->scene_output, &state, NULL)) {
        static uint64_t last_log_ns = 0;
        uint64_t now = wd_now_ns();

        if (now - last_log_ns > 1000000000ull) {
            wlr_log(WLR_ERROR,
                    "WayDisplay: wlr_scene_output_build_state failed");
            last_log_ns = now;
        }

        goto out;
    }

    built_state = true;

    if (!(state.committed & WLR_OUTPUT_STATE_BUFFER) || state.buffer == NULL) {
        /*
         * Nothing to read. This can happen during transient surface churn.
         */
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
        static uint64_t last_log_ns = 0;
        uint64_t now = wd_now_ns();

        if (now - last_log_ns > 1000000000ull) {
            wlr_log(WLR_ERROR,
                    "WayDisplay: invalid readback buffer size %dx%d",
                    state.buffer->width,
                    state.buffer->height);
            last_log_ns = now;
        }

        goto commit_only;
    }

    memset(server->framebuffer_xrgb8888, 0, WD_FRAMEBUFFER_BYTES);

    struct wlr_texture *texture =
    wlr_texture_from_buffer(server->renderer, state.buffer);

    if (!texture) {
        static uint64_t last_log_ns = 0;
        uint64_t now = wd_now_ns();

        if (now - last_log_ns > 1000000000ull) {
            wlr_log(WLR_ERROR,
                    "WayDisplay: wlr_texture_from_buffer failed for buffer %dx%d",
                    state.buffer->width,
                    state.buffer->height);
            last_log_ns = now;
        }

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

        static uint64_t last_log_ns = 0;
        uint64_t now = wd_now_ns();

        if (now - last_log_ns > 1000000000ull) {
            wlr_log(WLR_ERROR,
                    "WayDisplay: wlr_texture_read_pixels(XRGB8888) failed; preferred DRM format is 0x%08x",
                    preferred);
            last_log_ns = now;
        }

        wlr_texture_destroy(texture);
        goto commit_only;
    }

    wlr_texture_destroy(texture);
    readback_ok = true;

    commit_only:
    if (built_state) {
        if (!wlr_output_commit_state(server->output, &state)) {
            static uint64_t last_log_ns = 0;
            uint64_t now = wd_now_ns();

            if (now - last_log_ns > 1000000000ull) {
                wlr_log(WLR_ERROR,
                        "WayDisplay: wlr_output_commit_state failed");
                last_log_ns = now;
            }

            readback_ok = false;
        }
    }

    out:
    wlr_output_state_finish(&state);
    return readback_ok;
}
