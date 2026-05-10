#include "wd_server.h"

#include <drm_fourcc.h>

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
    wlr_data_device_manager_create(server->display);

    server->scene = wlr_scene_create();
    if (!server->scene) {
        return false;
    }

    server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
    if (!server->xdg_shell) {
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
