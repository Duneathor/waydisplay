#include "wd_server.h"

#include <drm_fourcc.h>
#include <stdlib.h>

static void output_handle_frame(struct wl_listener *listener, void *data) {
    (void)data;

    struct wd_server *server =
    wl_container_of(listener, server, output_frame);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    if (server->scene_output) {
        wlr_scene_output_send_frame_done(server->scene_output, &ts);
    }
}

static void output_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;

    struct wd_server *server =
    wl_container_of(listener, server, output_destroy);

    server->output = NULL;
    server->scene_output = NULL;
}

bool wd_wlroots_init(struct wd_server *server) {
    server->display = wl_display_create();
    if (!server->display) {
        return false;
    }

    server->event_loop = wl_display_get_event_loop(server->display);

    server->backend = wlr_headless_backend_create(server->event_loop);
    if (!server->backend) {
        return false;
    }

    server->renderer = wlr_renderer_autocreate(server->backend);
    if (!server->renderer) {
        return false;
    }

    wlr_renderer_init_wl_display(server->renderer, server->display);

    server->allocator =
    wlr_allocator_autocreate(server->backend, server->renderer);

    if (!server->allocator) {
        return false;
    }

    wlr_compositor_create(server->display, 5, server->renderer);
    wlr_subcompositor_create(server->display);

    server->viewporter = wlr_viewporter_create(server->display);
    if (!server->viewporter) {
        wlr_log(WLR_ERROR, "WayDisplay: failed to create wp_viewporter global");
        return false;
    }

    server->output_layout = wlr_output_layout_create(server->display);
    if (!server->output_layout) {
        wlr_log(WLR_ERROR, "WayDisplay: failed to create output layout");
        return false;
    }

    /*
     * xdg-output reports logical output name, description, position, and size.
     * Qt/Electron/toolkits often query this in addition to wl_output.
     */
    server->xdg_output_manager =
        wlr_xdg_output_manager_v1_create(server->display,
                                         server->output_layout);
    if (!server->xdg_output_manager) {
        wlr_log(WLR_ERROR, "WayDisplay: failed to create xdg-output manager");
        return false;
    }

    server->fractional_scale_manager =
        wlr_fractional_scale_manager_v1_create(server->display, 1);
    if (!server->fractional_scale_manager) {
        wlr_log(WLR_ERROR, "WayDisplay: failed to create fractional scale manager");
        return false;
    }

    /*
     * WayDisplay is not a normal hardware-output compositor: every frame needs
     * to become a readable framebuffer for tile hashing/compression/streaming.
     *
     * If wlroots scene direct scan-out is enabled, a single fullscreen client
     * can bypass compositing and state.buffer may be the client's own buffer.
     * Some of those buffers cannot be converted with wlr_texture_from_buffer(),
     * which causes repeated readback failures with fullscreen clients.
     */
    setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);

    server->scene = wlr_scene_create();
    if (!server->scene) {
        return false;
    }

    server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
    if (!server->xdg_shell) {
        return false;
    }

    if (!wd_xdg_activation_init(server)) {
        return false;
    }

    if (!wd_xdg_foreign_init(server)) {
        return false;
    }

    if (!wd_xdg_dialog_init(server)) {
        return false;
    }

    if (!wd_xdg_toplevel_icon_init(server)) {
        return false;
    }

    if (!wd_cursor_init(server)) {
        return false;
    }

    if (!wd_xdg_decoration_init(server)) {
        return false;
    }

    server->seat = wlr_seat_create(server->display, "seat0");
    if (!server->seat) {
        return false;
    }

    wlr_seat_set_capabilities(server->seat,
                              WL_SEAT_CAPABILITY_POINTER |
                              WL_SEAT_CAPABILITY_KEYBOARD);

    if (!wd_keyboard_init(server)) {
        return false;
    }

    if (!wd_clipboard_init(server)) {
        return false;
    }

    wd_scene_init_listeners(server);

    return true;
}

bool wd_wlroots_start(struct wd_server *server) {
    if (!wlr_backend_start(server->backend)) {
        return false;
    }

    return true;
}

bool wd_wlroots_create_headless_output(struct wd_server *server) {
    server->output =
    wlr_headless_add_output(server->backend,
                            WD_DISPLAY_WIDTH,
                            WD_DISPLAY_HEIGHT);

    if (!server->output) {
        return false;
    }

    if (!wlr_output_init_render(server->output,
        server->allocator,
        server->renderer)) {
        return false;
        }

        wlr_output_set_name(server->output, "WayDisplay-0");
    wlr_output_set_description(server->output,
                               "WayDisplay headless remote output");

    struct wlr_output_state state;
    wlr_output_state_init(&state);

    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state,
                                     WD_DISPLAY_WIDTH,
                                     WD_DISPLAY_HEIGHT,
                                     60000);
    wlr_output_state_set_scale(&state, server->output_scale);
    wlr_output_state_set_render_format(&state, DRM_FORMAT_XRGB8888);

    bool ok = wlr_output_commit_state(server->output, &state);

    wlr_output_state_finish(&state);

    if (!ok) {
        return false;
    }

    /*
     * Important: clients need a wl_output global.
     * Without this, Qt and other clients may create placeholder screens.
     */
    wlr_output_create_global(server->output, server->display);

    /*
     * xdg-output is driven by wlr_output_layout. WayDisplay has one logical
     * output at compositor-space origin 0,0.
     */
    if (server->output_layout) {
        wlr_output_layout_add(server->output_layout,
                              server->output,
                              0,
                              0);
    }

    server->scene_output =
    wlr_scene_output_create(server->scene, server->output);

    if (!server->scene_output) {
        return false;
    }

    wlr_scene_output_set_position(server->scene_output, 0, 0);

    server->output_frame.notify = output_handle_frame;
    wl_signal_add(&server->output->events.frame, &server->output_frame);

    server->output_destroy.notify = output_handle_destroy;
    wl_signal_add(&server->output->events.destroy, &server->output_destroy);

    wlr_log(WLR_INFO,
            "WayDisplay: created headless output %ux%u",
            WD_DISPLAY_WIDTH,
            WD_DISPLAY_HEIGHT);

    return true;
}
