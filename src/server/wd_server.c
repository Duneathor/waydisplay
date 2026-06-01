#include "wd_server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "waydisplay/wd_time.h"
#include "waydisplay/wd_tile.h"

static struct wd_server *g_server_for_signal = NULL;
static volatile sig_atomic_t g_terminate_requested = 0;

static void handle_signal(int signo) {
    (void)signo;

    /*
     * Do not call wl_display_terminate() from a POSIX signal handler. It is not
     * async-signal-safe. The frame timer runs on the Wayland event loop and
     * will perform the actual termination.
     */
    g_terminate_requested = 1;
}

static bool launch_startup_command(struct wd_server *server) {
    if (!server->startup_command || server->startup_command[0] == '\0') {
        return true;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", server->socket_name, 1);
        setenv("XDG_SESSION_TYPE", "wayland", 1);

        unsetenv("DISPLAY");
        unsetenv("WAYLAND_SOCKET");

        setenv("GDK_BACKEND", "wayland", 1);
        setenv("QT_QPA_PLATFORM", "wayland", 1);
        setenv("SDL_VIDEODRIVER", "wayland", 1);
        setenv("CLUTTER_BACKEND", "wayland", 1);
        setenv("MOZ_ENABLE_WAYLAND", "1", 1);

        execl("/bin/sh",
              "/bin/sh",
              "-c",
              server->startup_command,
              (char *)NULL);

        _exit(127);
    }

    WD_LOG_INFO(
            "WayDisplay: launched app pid=%d command=%s",
            pid,
            server->startup_command);

    return true;
}

static int server_frame_timer(void *data) {
    struct wd_server *server = data;

    if (!server || !server->display) {
        return 0;
    }

    if (g_terminate_requested) {
        wl_display_terminate(server->display);
        return 0;
    }

    /*
     * Drain keyboard first so a held Ctrl modifier is active before a clipboard
     * paste request synthesizes the V key. Drain pointer after clipboard so
     * middle-click primary paste sees the freshly published primary selection.
     */
    wd_keyboard_drain_and_inject(server);
    wd_clipboard_drain_and_apply(server);
    wd_pointer_drain_and_inject(server);

    uint64_t t = wd_now_ns();

    bool should_render = wd_stream_policy_should_render_now(server, t);

    if (should_render) {
        if (server->output) {
            wlr_output_schedule_frame(server->output);
        }

        bool have_frame = wd_render_scene_and_readback_xrgb8888(server);

        if (have_frame) {
            wd_stream_send_dirty_tiles(server);
        } else {
            /*
             * Avoid spinning forever on transient readback failures.
             * Surface commits/map/unmap will mark dirty again.
             */
            server->scene_dirty = false;
        }
    }

    if (t - server->last_summary_ns > 2000000000ull) {
        pthread_mutex_lock(&server->net.lock);
        wd_stream_send_generation_summary_locked(server);
        pthread_mutex_unlock(&server->net.lock);

        server->last_summary_ns = t;
    }

    if (t - server->last_stats_ns > 1000000000ull) {
        wd_stream_print_and_reset_stats(server);
        server->last_stats_ns = t;
    }

    wl_event_source_timer_update(server->frame_timer, 8);

    return 0;
}

void wd_server_mark_scene_dirty(struct wd_server *server) {
    if (server) {
        server->scene_dirty = true;
    }
}

bool wd_server_set_geometry(struct wd_server *server,
                            uint32_t width,
                            uint32_t height) {
    if (!server || width == 0 || height == 0 ||
        width > UINT16_MAX || height > UINT16_MAX) {
        return false;
    }

    const uint16_t tiles_x = wd_tiles_for_width(width);
    const uint16_t tiles_y = wd_tiles_for_height(height);
    const uint32_t total_tiles = (uint32_t)tiles_x * (uint32_t)tiles_y;

    if (tiles_x == 0 || tiles_y == 0 ||
        total_tiles == 0 || total_tiles > UINT16_MAX) {
        return false;
    }

    server->display_width = width;
    server->display_height = height;
    server->tiles_x = tiles_x;
    server->tiles_y = tiles_y;
    server->total_tiles = (uint16_t)total_tiles;
    server->framebuffer_pixels =
        server->display_width * server->display_height;
    server->framebuffer_bytes =
        server->framebuffer_pixels * WD_BYTES_PER_PIXEL;

    return true;
}

void wd_server_set_default_geometry(struct wd_server *server) {
    (void)wd_server_set_geometry(server, WD_DISPLAY_WIDTH, WD_DISPLAY_HEIGHT);
}

bool wd_server_apply_display_size(struct wd_server *server,
                                  uint32_t width,
                                  uint32_t height) {
    if (!server || width == 0 || height == 0 ||
        width > UINT16_MAX || height > UINT16_MAX) {
        return false;
    }

    if (server->display_width == width && server->display_height == height) {
        return true;
    }

    pthread_mutex_lock(&server->net.lock);

    wd_stream_destroy(server);

    free(server->net.dirty_queue);
    server->net.dirty_queue = NULL;

    free(server->net.dirty_queued);
    server->net.dirty_queued = NULL;

    free(server->framebuffer_xrgb8888);
    server->framebuffer_xrgb8888 = NULL;

    bool ok = wd_server_set_geometry(server, width, height);

    if (ok) {
        server->framebuffer_xrgb8888 =
            calloc(server->framebuffer_pixels, sizeof(uint32_t));
        ok = server->framebuffer_xrgb8888 != NULL;
    }

    if (ok) {
        server->net.dirty_queue =
            calloc(server->total_tiles, sizeof(*server->net.dirty_queue));
        server->net.dirty_queued =
            calloc(server->total_tiles, sizeof(*server->net.dirty_queued));
        ok = server->net.dirty_queue && server->net.dirty_queued;
    }

    if (ok) {
        ok = wd_stream_init(server);
    }

    if (ok) {
        ok = wd_wlroots_resize_headless_output(server);
    }

    if (ok) {
        server->net.full_frame_needed = true;
        server->net.full_frame_next_tile = 0;
        server->net.dirty_scan_next_tile = 0;
        server->net.dirty_queue_read = 0;
        server->net.dirty_queue_write = 0;
        server->net.dirty_queue_count = 0;
        server->scene_dirty = true;
    }

    pthread_mutex_unlock(&server->net.lock);

    return ok;
}

bool wd_server_init(struct wd_server *server,
                    uint16_t tcp_port,
                    const char *app_cmd,
                    double output_scale,
                    uint32_t display_width,
                    uint32_t display_height) {
    memset(server, 0, sizeof(*server));

    wl_list_init(&server->views);
    wl_list_init(&server->popup_commit_trackers);
    wl_list_init(&server->keyboard_shortcuts_inhibitors);
    wl_list_init(&server->new_keyboard_shortcuts_inhibitor.link);
    wl_list_init(&server->keyboard_shortcuts_inhibit_manager_destroy.link);

    if (!wd_server_set_geometry(server, display_width, display_height)) {
        return false;
    }

    server->scene_dirty = true;

    server->startup_command = app_cmd;
    server->output_scale = output_scale;

    if (server->output_scale <= 0.0) {
        server->output_scale = 1.0;
    }

    server->framebuffer_xrgb8888 =
    calloc(server->framebuffer_pixels, sizeof(uint32_t));

    if (!server->framebuffer_xrgb8888) {
        return false;
    }

    if (!wd_net_init(server, tcp_port)) {
        return false;
    }

    if (!wd_stream_init(server)) {
        return false;
    }

    if (!wd_wlroots_init(server)) {
        return false;
    }

    server->last_summary_ns = wd_now_ns();
    server->last_stats_ns = wd_now_ns();

    server->socket_name = wl_display_add_socket_auto(server->display);
    if (!server->socket_name) {
        return false;
    }

    if (!wd_wlroots_start(server)) {
        return false;
    }

    if (!wd_wlroots_create_headless_output(server)) {
        return false;
    }

    server->frame_timer =
    wl_event_loop_add_timer(server->event_loop,
                            server_frame_timer,
                            server);

    if (!server->frame_timer) {
        WD_LOG_ERROR(
                "WayDisplay: failed to create frame timer");
        return false;
    }

    wl_event_source_timer_update(server->frame_timer, 1);

    setenv("WAYLAND_DISPLAY", server->socket_name, 1);

    WD_LOG_INFO(
            "WayDisplay: running on WAYLAND_DISPLAY=%s",
            server->socket_name);

    return true;
                    }

                    void wd_server_destroy(struct wd_server *server) {
                        if (!server) {
                            return;
                        }

                        if (server->frame_timer) {
                            wl_event_source_remove(server->frame_timer);
                            server->frame_timer = NULL;
                        }

                        wd_clipboard_destroy(server);
                        wd_keyboard_shortcuts_inhibit_destroy(server);
                        wd_cursor_destroy(server);
                        wd_xdg_activation_destroy(server);
                        wd_xdg_foreign_destroy(server);
                        wd_xdg_dialog_destroy(server);
                        wd_xdg_toplevel_icon_destroy(server);
                        wd_xdg_decoration_destroy(server);
                        wd_net_destroy(server);
                        wd_stream_destroy(server);

                        if (server->new_xdg_surface.link.prev && server->new_xdg_surface.link.next) {
                            wl_list_remove(&server->new_xdg_surface.link);
                            wl_list_init(&server->new_xdg_surface.link);
                        }

                        if (server->new_xdg_toplevel.link.prev && server->new_xdg_toplevel.link.next) {
                            wl_list_remove(&server->new_xdg_toplevel.link);
                            wl_list_init(&server->new_xdg_toplevel.link);
                        }

                        if (server->new_xdg_toplevel_decoration.link.prev && server->new_xdg_toplevel_decoration.link.next) {
                            wl_list_remove(&server->new_xdg_toplevel_decoration.link);
                            wl_list_init(&server->new_xdg_toplevel_decoration.link);
                        }

                        if (server->output && server->output_frame.link.prev && server->output_frame.link.next) {
                            wl_list_remove(&server->output_frame.link);
                            wl_list_init(&server->output_frame.link);
                        }

                        if (server->output && server->output_destroy.link.prev && server->output_destroy.link.next) {
                            wl_list_remove(&server->output_destroy.link);
                            wl_list_init(&server->output_destroy.link);
                        }

                        if (server->display) {
                            wl_display_destroy_clients(server->display);
                            wl_display_destroy(server->display);
                            server->display = NULL;
                        }

                        if (server->output_layout) {
                            wlr_output_layout_destroy(server->output_layout);
                            server->output_layout = NULL;
                        }

                        free(server->framebuffer_xrgb8888);
                        server->framebuffer_xrgb8888 = NULL;
                    }

                    int wd_server_run(struct wd_server *server) {
#if WAYDISPLAY_ENABLE_LOGGING && WAYDISPLAY_ENABLE_DEBUG_LOGGING
                        wlr_log_init(WLR_DEBUG, NULL);
#else
                        wlr_log_init(WLR_ERROR, NULL);
#endif

                        g_server_for_signal = server;
                        g_terminate_requested = 0;

                        signal(SIGINT, handle_signal);
                        signal(SIGTERM, handle_signal);
                        signal(SIGPIPE, SIG_IGN);

                        pthread_t net_thread;

                        if (pthread_create(&net_thread,
                            NULL,
                            wd_net_thread_main,
                            server) != 0) {
                            return 1;
                            }

                            if (!launch_startup_command(server)) {
                                server->net.running = false;
                                pthread_join(net_thread, NULL);
                                return 1;
                            }

                            wl_display_run(server->display);

                            server->net.running = false;

                            if (server->net.listen_fd >= 0) {
                                shutdown(server->net.listen_fd, SHUT_RDWR);
                            }

                            if (server->net.tcp_fd >= 0) {
                                shutdown(server->net.tcp_fd, SHUT_RDWR);
                            }

                            pthread_join(net_thread, NULL);

                            return 0;
                    }
