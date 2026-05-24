#include "wd_server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "waydisplay/wd_time.h"

static struct wd_server *g_server_for_signal = NULL;

static void handle_signal(int signo) {
    (void)signo;

    if (g_server_for_signal && g_server_for_signal->display) {
        wl_display_terminate(g_server_for_signal->display);
    }
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

    wlr_log(WLR_INFO,
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
    wd_pointer_drain_and_inject(server);
    wd_keyboard_drain_and_inject(server);

    /*
     * Remote clipboard/primary messages are paste-text commands. Drain them
     * after normal keyboard events so a Ctrl key press already forwarded by
     * SDL can be released before the pasted characters are injected.
     */
    wd_clipboard_drain_and_apply(server);

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

bool wd_server_init(struct wd_server *server,
                    uint16_t tcp_port,
                    const char *app_cmd,
                    double output_scale) {
    memset(server, 0, sizeof(*server));

    wl_list_init(&server->views);

    server->scene_dirty = true;

    server->startup_command = app_cmd;
    server->output_scale = output_scale;

    if (server->output_scale <= 0.0) {
        server->output_scale = 1.0;
    }

    server->framebuffer_xrgb8888 =
    calloc(WD_FRAMEBUFFER_PIXELS, sizeof(uint32_t));

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
        wlr_log(WLR_ERROR,
                "WayDisplay: failed to create frame timer");
        return false;
    }

    wl_event_source_timer_update(server->frame_timer, 1);

    setenv("WAYLAND_DISPLAY", server->socket_name, 1);

    wlr_log(WLR_INFO,
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

                        free(server->framebuffer_xrgb8888);
                        server->framebuffer_xrgb8888 = NULL;
                    }

                    int wd_server_run(struct wd_server *server) {
                        wlr_log_init(WLR_DEBUG, NULL);

                        g_server_for_signal = server;

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
