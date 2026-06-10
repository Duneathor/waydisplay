#include "wd_server.h"

#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct wd_server*     g_server_for_signal   = NULL;
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

static bool launch_startup_command(struct wd_server* server) {
    if (!server->startup_command || server->startup_command[0] == '\0')
    {
        return true;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        return false;
    }

    if (pid == 0)
    {
        setenv("WAYLAND_DISPLAY", server->socket_name, 1);
        setenv("XDG_SESSION_TYPE", "wayland", 1);

#if WAYDISPLAY_ENABLE_XWAYLAND
        if (server->xwayland && server->xwayland->display_name)
        {
            setenv("DISPLAY", server->xwayland->display_name, 1);
        }
        else
        {
            unsetenv("DISPLAY");
        }
#else
        unsetenv("DISPLAY");
#endif
        unsetenv("WAYLAND_SOCKET");

        setenv("GDK_BACKEND", "wayland", 1);
        setenv("QT_QPA_PLATFORM", "wayland", 1);
        setenv("SDL_VIDEODRIVER", "wayland", 1);
        setenv("CLUTTER_BACKEND", "wayland", 1);
        setenv("MOZ_ENABLE_WAYLAND", "1", 1);

#if WAYDISPLAY_ENABLE_XWAYLAND
        /*
         * Java AWT/Swing uses the window-manager identity to decide whether it
         * should use reparenting-window assumptions. wlroots compositors,
         * including Sway, commonly require this override for JetBrains, Vivado,
         * Logisim/Geogebra and similar Xwayland Java applications; otherwise
         * the top-level X11 window can map successfully while the client keeps
         * painting only a blank/white surface.
         *
         * Set it for WayDisplay-launched processes so shells launched via
         * --app inherit the safer Xwayland behavior. Existing user-provided
         * values are preserved.
         */
        setenv("_JAVA_AWT_WM_NONREPARENTING", "1", 0);
#endif

        execl("/bin/sh", "/bin/sh", "-c", server->startup_command, (char*)NULL);

        _exit(127);
    }

    WD_LOG_INFO("WayDisplay: launched app pid=%d command=%s", pid, server->startup_command);

    return true;
}


static void server_process_pending_display_resize(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);
    if (!net->display_resize_pending)
    {
        pthread_mutex_unlock(&net->lock);
        return;
    }

    uint64_t serial = net->display_resize_request_serial;
    uint32_t width  = net->display_resize_width;
    uint32_t height = net->display_resize_height;
    net->display_resize_pending = false;
    pthread_mutex_unlock(&net->lock);

    bool ok = wd_server_apply_display_size(server, width, height);

    pthread_mutex_lock(&net->lock);
    net->display_resize_result           = ok;
    net->display_resize_completed_serial = serial;
    pthread_cond_broadcast(&net->display_resize_cond);
    pthread_mutex_unlock(&net->lock);
}

static int server_frame_timer(void* data) {
    struct wd_server* server = data;

    if (!server || !server->display)
    {
        return 0;
    }

    if (g_terminate_requested)
    {
        wl_display_terminate(server->display);
        return 0;
    }

    server_process_pending_display_resize(server);

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

    if (should_render)
    {
        if (server->output)
        {
            wlr_output_schedule_frame(server->output);
        }

        bool have_frame = wd_render_scene_and_readback_xrgb8888(server);

        if (have_frame)
        {
            wd_stream_send_dirty_tiles(server);
        }
        else
        {
            /*
             * Avoid spinning forever on transient readback failures.
             * Surface commits/map/unmap will mark dirty again.
             */
            server->scene_dirty = false;
        }
    }

    if (server->last_summary_ns == 0 || t - server->last_summary_ns >= WD_GENERATION_SUMMARY_FULL_INTERVAL_NS)
    {
        pthread_mutex_lock(&server->net.lock);
        wd_stream_send_generation_summary_locked(server);
        pthread_mutex_unlock(&server->net.lock);

        server->last_summary_ns       = t;
        server->last_delta_summary_ns = t;
    }
    else
    {
        uint64_t delta_interval_ns = WD_GENERATION_SUMMARY_CLEAN_DELTA_INTERVAL_NS;
        pthread_mutex_lock(&server->net.lock);
        if (server->net.retransmit_queue_count != 0 || server->net.stats.client_partial_tiles_timed_out != 0 ||
            server->net.stats.client_retx_requests_tx != 0)
        {
            delta_interval_ns = WD_GENERATION_SUMMARY_DELTA_INTERVAL_NS;
        }
        const bool should_send_delta = server->last_delta_summary_ns == 0 || t - server->last_delta_summary_ns >= delta_interval_ns;
        if (should_send_delta)
        {
            wd_stream_send_pending_generation_summary_locked(server);
            server->last_delta_summary_ns = t;
        }
        pthread_mutex_unlock(&server->net.lock);
    }

    if (t - server->last_stats_ns > 1000000000ull)
    {
        wd_stream_print_and_reset_stats(server);
        server->last_stats_ns = t;
    }

    wl_event_source_timer_update(server->frame_timer, 8);

    return 0;
}

static void mark_damage_tile(struct wd_server* server, uint32_t tile_id) {
    if (!server || tile_id >= server->total_base_tiles)
    {
        return;
    }

    if (!server->damage_tiles)
    {
        server->damage_all_tiles = true;
        return;
    }

    if (!server->damage_tiles[tile_id])
    {
        server->damage_tiles[tile_id] = true;
        server->damage_tile_count++;
    }
}

void wd_server_mark_scene_dirty(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    server->scene_dirty       = true;
    server->damage_all_tiles  = true;
    server->damage_tile_count = 0;

    if (server->damage_tiles && server->total_base_tiles > 0)
    {
        memset(server->damage_tiles, 0, (size_t)server->total_base_tiles * sizeof(*server->damage_tiles));
    }
}

void wd_server_mark_rect_dirty(struct wd_server* server, int x, int y, int width, int height) {
    if (!server || width <= 0 || height <= 0 || server->total_base_tiles == 0)
    {
        return;
    }

    server->scene_dirty = true;

    int x1 = x;
    int y1 = y;
    int x2 = x + width;
    int y2 = y + height;

    if (x1 < 0)
    {
        x1 = 0;
    }
    if (y1 < 0)
    {
        y1 = 0;
    }
    if (x2 > (int)server->display_width)
    {
        x2 = (int)server->display_width;
    }
    if (y2 > (int)server->display_height)
    {
        y2 = (int)server->display_height;
    }

    if (x1 >= x2 || y1 >= y2)
    {
        return;
    }

    if (!server->damage_tiles || server->damage_all_tiles)
    {
        server->damage_all_tiles = true;
        return;
    }

    uint16_t start_tile_x = (uint16_t)((uint32_t)x1 / server->base_tile_width);
    uint16_t end_tile_x   = (uint16_t)(((uint32_t)(x2 - 1)) / server->base_tile_width);
    uint16_t start_tile_y = (uint16_t)((uint32_t)y1 / server->base_tile_height);
    uint16_t end_tile_y   = (uint16_t)(((uint32_t)(y2 - 1)) / server->base_tile_height);

    if (end_tile_x >= server->base_tiles_x)
    {
        end_tile_x = (uint16_t)(server->base_tiles_x - 1);
    }
    if (end_tile_y >= server->base_tiles_y)
    {
        end_tile_y = (uint16_t)(server->base_tiles_y - 1);
    }

    for (uint16_t ty = start_tile_y; ty <= end_tile_y; ++ty)
    {
        for (uint16_t tx = start_tile_x; tx <= end_tile_x; ++tx)
        {
            uint32_t tile_id = (uint32_t)ty * (uint32_t)server->base_tiles_x + (uint32_t)tx;
            mark_damage_tile(server, tile_id);
        }
    }
}

static void view_bounds_for_damage(struct wd_view* view, int origin_x, int origin_y, int* out_x, int* out_y, int* out_width,
                                   int* out_height) {
    if (!view || !out_x || !out_y || !out_width || !out_height)
    {
        return;
    }

    int width  = 0;
    int height = 0;

    if (view->xdg_surface && view->xdg_surface->surface)
    {
        width  = view->xdg_surface->surface->current.width;
        height = view->xdg_surface->surface->current.height;
    }
#if WAYDISPLAY_ENABLE_XWAYLAND
    else if (view->xwayland_surface && view->xwayland_surface->surface)
    {
        width  = view->xwayland_surface->surface->current.width;
        height = view->xwayland_surface->surface->current.height;
        if (wd_xwayland_view_has_decoration(view))
        {
            height += WD_XWAYLAND_TITLEBAR_HEIGHT;
        }
    }
#endif

    if (width <= 0)
    {
        width = (int)view->bounds_width;
    }
    if (height <= 0)
    {
        height = (int)view->bounds_height;
    }
    if (width <= 0)
    {
        width = (int)view->server->display_width;
    }
    if (height <= 0)
    {
        height = (int)view->server->display_height;
    }

    *out_x      = origin_x;
    *out_y      = origin_y;
    *out_width  = width;
    *out_height = height;
}

void wd_server_mark_view_dirty(struct wd_view* view) {
    if (!view || !view->server)
    {
        return;
    }

    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;
    view_bounds_for_damage(view, view->x, view->y, &x, &y, &width, &height);
    wd_server_mark_rect_dirty(view->server, x, y, width, height);
}

void wd_server_mark_view_move_dirty(struct wd_view* view, int old_x, int old_y) {
    if (!view || !view->server)
    {
        return;
    }

    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;
    view_bounds_for_damage(view, old_x, old_y, &x, &y, &width, &height);
    wd_server_mark_rect_dirty(view->server, x, y, width, height);

    view_bounds_for_damage(view, view->x, view->y, &x, &y, &width, &height);
    wd_server_mark_rect_dirty(view->server, x, y, width, height);
}

bool wd_server_set_tile_size(struct wd_server* server, uint16_t tile_width, uint16_t tile_height) {
    if (!server || tile_width == 0 || tile_height == 0)
    {
        return false;
    }

    /* Keep tile buffers comfortably within the existing TCP/UDP payload limits and
     * client framebuffer limits. These are protocol dimensions, not renderer pixels. */
    uint32_t tile_bytes = (uint32_t)tile_width * (uint32_t)tile_height * WD_BYTES_PER_PIXEL;
    if (tile_bytes == 0 || tile_bytes > WD_TCP_MAX_PAYLOAD_SIZE)
    {
        return false;
    }

    server->tile_width              = tile_width;
    server->tile_height             = tile_height;
    server->uncompressed_tile_bytes = tile_bytes;
    return true;
}

bool wd_server_set_geometry(struct wd_server* server, uint32_t width, uint32_t height) {
    if (!server || width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX)
    {
        return false;
    }

    if (server->tile_width == 0 || server->tile_height == 0)
    {
        if (!wd_server_set_tile_size(server, WD_TILE_WIDTH, WD_TILE_HEIGHT))
        {
            return false;
        }
    }

    const uint16_t tiles_x          = wd_tiles_for_width_with_tile(width, server->tile_width);
    const uint16_t tiles_y          = wd_tiles_for_height_with_tile(height, server->tile_height);
    const uint32_t total_tiles      = (uint32_t)tiles_x * (uint32_t)tiles_y;
    const uint16_t base_tile_width  = WD_BASE_TILE_WIDTH;
    const uint16_t base_tile_height = WD_BASE_TILE_HEIGHT;
    const uint16_t base_tiles_x     = wd_tiles_for_width_with_tile(width, base_tile_width);
    const uint16_t base_tiles_y     = wd_tiles_for_height_with_tile(height, base_tile_height);
    const uint32_t total_base_tiles = (uint32_t)base_tiles_x * (uint32_t)base_tiles_y;

    if (tiles_x == 0 || tiles_y == 0 || total_tiles == 0 || total_tiles > UINT16_MAX || base_tiles_x == 0 || base_tiles_y == 0 ||
        total_base_tiles == 0)
    {
        return false;
    }

    server->display_width      = width;
    server->display_height     = height;
    server->tiles_x            = tiles_x;
    server->tiles_y            = tiles_y;
    server->total_tiles        = (uint16_t)total_tiles;
    server->base_tile_width    = base_tile_width;
    server->base_tile_height   = base_tile_height;
    server->base_tiles_x       = base_tiles_x;
    server->base_tiles_y       = base_tiles_y;
    server->total_base_tiles   = total_base_tiles;
    server->framebuffer_pixels = server->display_width * server->display_height;
    server->framebuffer_bytes  = server->framebuffer_pixels * WD_BYTES_PER_PIXEL;

    return true;
}

void wd_server_set_default_geometry(struct wd_server* server) {
    (void)wd_server_set_geometry(server, WD_DISPLAY_WIDTH, WD_DISPLAY_HEIGHT);
}


bool wd_server_request_display_size(struct wd_server* server, uint32_t width, uint32_t height) {
    if (!server || width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    uint64_t serial = ++net->display_resize_request_serial;
    net->display_resize_width  = width;
    net->display_resize_height = height;
    net->display_resize_pending = true;

    while (net->running && net->display_resize_completed_serial < serial)
    {
        pthread_cond_wait(&net->display_resize_cond, &net->lock);
    }

    bool ok = net->running && net->display_resize_completed_serial >= serial && net->display_resize_result;

    pthread_mutex_unlock(&net->lock);

    return ok;
}

bool wd_server_apply_display_size(struct wd_server* server, uint32_t width, uint32_t height) {
    if (!server || width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX)
    {
        return false;
    }

    if (server->display_width == width && server->display_height == height)
    {
        return true;
    }

    const uint32_t old_width              = server->display_width;
    const uint32_t old_height             = server->display_height;
    const uint16_t old_tiles_x            = server->tiles_x;
    const uint16_t old_tiles_y            = server->tiles_y;
    const uint16_t old_total_tiles        = server->total_tiles;
    const uint16_t old_base_tile_width    = server->base_tile_width;
    const uint16_t old_base_tile_height   = server->base_tile_height;
    const uint16_t old_base_tiles_x       = server->base_tiles_x;
    const uint16_t old_base_tiles_y       = server->base_tiles_y;
    const uint32_t old_total_base_tiles   = server->total_base_tiles;
    const uint32_t old_framebuffer_pixels = server->framebuffer_pixels;
    const uint32_t old_framebuffer_bytes  = server->framebuffer_bytes;

    /*
     * Resize the wlroots output before tearing down the current stream buffers.
     * Some renderers can reject a live resize transiently; keeping the old
     * buffers/state intact lets the server reject the client request without
     * leaving the compositor in a half-destroyed state.
     */
    if (!wd_server_set_geometry(server, width, height))
    {
        return false;
    }

    bool output_resized = wd_wlroots_resize_headless_output(server);

    server->display_width      = old_width;
    server->display_height     = old_height;
    server->tiles_x            = old_tiles_x;
    server->tiles_y            = old_tiles_y;
    server->total_tiles        = old_total_tiles;
    server->base_tile_width    = old_base_tile_width;
    server->base_tile_height   = old_base_tile_height;
    server->base_tiles_x       = old_base_tiles_x;
    server->base_tiles_y       = old_base_tiles_y;
    server->total_base_tiles   = old_total_base_tiles;
    server->framebuffer_pixels = old_framebuffer_pixels;
    server->framebuffer_bytes  = old_framebuffer_bytes;

    if (!output_resized)
    {
        return false;
    }

    pthread_mutex_lock(&server->net.lock);

    wd_stream_wait_for_encoder_idle_locked(server);
    server->framebuffer_generation++;
    if (server->framebuffer_generation == 0)
    {
        server->framebuffer_generation = 1;
    }

    wd_stream_destroy(server);

    free(server->net.dirty_regions);
    server->net.dirty_regions = NULL;
    free(server->net.dirty_region_queued);
    server->net.dirty_region_queued = NULL;
    server->net.dirty_region_count = 0;

    free(server->net.dirty_queue);
    server->net.dirty_queue = NULL;

    free(server->net.dirty_queued);
    server->net.dirty_queued = NULL;

    free(server->net.dirty_epochs);
    server->net.dirty_epochs = NULL;

    free(server->net.dirty_queue_enqueued_ns);
    server->net.dirty_queue_enqueued_ns = NULL;

    free(server->net.retransmit_queue);
    server->net.retransmit_queue = NULL;

    free(server->net.retransmit_queued);
    server->net.retransmit_queued = NULL;

    free(server->net.retransmit_queue_enqueued_ns);
    server->net.retransmit_queue_enqueued_ns = NULL;

    free(server->net.retransmit_requested_generation);
    server->net.retransmit_requested_generation = NULL;

    free(server->net.summary_dirty_tiles);
    server->net.summary_dirty_tiles = NULL;
    server->net.summary_dirty_count = 0;


    free(server->framebuffer_xrgb8888);
    server->framebuffer_xrgb8888 = NULL;

    bool ok = wd_server_set_geometry(server, width, height);

    if (ok)
    {
        server->framebuffer_xrgb8888 = calloc(server->framebuffer_pixels, sizeof(uint32_t));
        ok                           = server->framebuffer_xrgb8888 != NULL;
        if (ok)
        {
            server->framebuffer_generation++;
            if (server->framebuffer_generation == 0)
            {
                server->framebuffer_generation = 1;
            }
        }
    }

    if (ok)
    {
        server->net.dirty_regions                = calloc(server->total_tiles, sizeof(*server->net.dirty_regions));
        server->net.dirty_region_queued          = calloc(server->total_tiles, sizeof(*server->net.dirty_region_queued));
        server->net.dirty_region_count           = 0;
        server->net.dirty_epochs                 = calloc(server->total_tiles, sizeof(*server->net.dirty_epochs));
        server->net.dirty_queue                  = calloc(server->total_tiles, sizeof(*server->net.dirty_queue));
        server->net.dirty_queued                 = calloc(server->total_tiles, sizeof(*server->net.dirty_queued));
        server->net.dirty_queue_enqueued_ns      = calloc(server->total_tiles, sizeof(*server->net.dirty_queue_enqueued_ns));
        server->net.retransmit_queue             = calloc(server->total_tiles, sizeof(*server->net.retransmit_queue));
        server->net.retransmit_queued            = calloc(server->total_tiles, sizeof(*server->net.retransmit_queued));
        server->net.retransmit_queue_enqueued_ns = calloc(server->total_tiles, sizeof(*server->net.retransmit_queue_enqueued_ns));
        server->net.retransmit_requested_generation = calloc(server->total_tiles, sizeof(*server->net.retransmit_requested_generation));
        server->net.summary_dirty_tiles          = calloc(server->total_tiles, sizeof(*server->net.summary_dirty_tiles));
        ok = server->net.dirty_regions && server->net.dirty_region_queued && server->net.dirty_epochs && server->net.dirty_queue &&
             server->net.dirty_queued && server->net.dirty_queue_enqueued_ns &&
             server->net.retransmit_queue && server->net.retransmit_queued && server->net.retransmit_queue_enqueued_ns &&
             server->net.retransmit_requested_generation && server->net.summary_dirty_tiles;
    }

    if (ok)
    {
        ok = wd_stream_init(server);
    }

    if (ok)
    {
        server->net.dirty_region_rng = 0;
        server->net.dirty_region_count     = 0;
        server->net.dirty_queue_read       = 0;
        server->net.dirty_queue_write      = 0;
        server->net.dirty_queue_count      = 0;
        server->net.retransmit_queue_count = 0;
        server->net.summary_dirty_count    = 0;
        wd_stream_invalidate_all_tiles_locked(server);
        server->last_summary_ns            = 0;
        server->last_delta_summary_ns      = 0;
        server->scene_dirty                = true;
    }

    pthread_mutex_unlock(&server->net.lock);

    return ok;
}

bool wd_server_init(struct wd_server* server, uint16_t tcp_port, const char* app_cmd, double output_scale, uint32_t display_width,
                    uint32_t display_height, uint16_t tile_width, uint16_t tile_height, bool enable_xwayland) {
    memset(server, 0, sizeof(*server));

    wl_list_init(&server->views);
    wl_list_init(&server->popup_commit_trackers);
    wl_list_init(&server->keyboard_shortcuts_inhibitors);
    wl_list_init(&server->new_keyboard_shortcuts_inhibitor.link);
    wl_list_init(&server->keyboard_shortcuts_inhibit_manager_destroy.link);
    wl_list_init(&server->pointer_button_grab_surface_destroy.link);

    if (!wd_server_set_tile_size(server, tile_width, tile_height) || !wd_server_set_geometry(server, display_width, display_height))
    {
        return false;
    }

    server->scene_dirty = true;

    server->startup_command = app_cmd;
    server->output_scale    = output_scale;
#if WAYDISPLAY_ENABLE_XWAYLAND
    server->enable_xwayland = enable_xwayland;
#else
    (void)enable_xwayland;
#endif

    if (server->output_scale <= 0.0)
    {
        server->output_scale = 1.0;
    }

    server->framebuffer_xrgb8888 = calloc(server->framebuffer_pixels, sizeof(uint32_t));
    if (server->framebuffer_xrgb8888)
    {
        server->framebuffer_generation = 1;
    }

    if (!server->framebuffer_xrgb8888)
    {
        return false;
    }

    if (!wd_net_init(server, tcp_port))
    {
        return false;
    }

    if (!wd_stream_init(server))
    {
        return false;
    }

    if (!wd_wlroots_init(server))
    {
        return false;
    }

    server->last_summary_ns = wd_now_ns();
    server->last_stats_ns   = wd_now_ns();

    server->socket_name = wl_display_add_socket_auto(server->display);
    if (!server->socket_name)
    {
        return false;
    }

    if (!wd_wlroots_start(server))
    {
        return false;
    }

    if (!wd_wlroots_create_headless_output(server))
    {
        return false;
    }

    server->frame_timer = wl_event_loop_add_timer(server->event_loop, server_frame_timer, server);

    if (!server->frame_timer)
    {
        WD_LOG_ERROR("WayDisplay: failed to create frame timer");
        return false;
    }

    wl_event_source_timer_update(server->frame_timer, 1);

    setenv("WAYLAND_DISPLAY", server->socket_name, 1);

    WD_LOG_INFO("WayDisplay: running on WAYLAND_DISPLAY=%s", server->socket_name);

    return true;
}

void wd_server_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    if (server->frame_timer)
    {
        wl_event_source_remove(server->frame_timer);
        server->frame_timer = NULL;
    }

    wd_clipboard_destroy(server);
#if WAYDISPLAY_ENABLE_XWAYLAND
    wd_xwayland_destroy(server);
#endif
    wd_keyboard_shortcuts_inhibit_destroy(server);
    wd_cursor_destroy(server);
    wd_xdg_activation_destroy(server);
    wd_xdg_foreign_destroy(server);
    wd_xdg_dialog_destroy(server);
    wd_xdg_toplevel_icon_destroy(server);
    wd_xdg_decoration_destroy(server);
    wd_net_destroy(server);
    wd_stream_destroy(server);

    if (server->new_xdg_surface.link.prev && server->new_xdg_surface.link.next)
    {
        wl_list_remove(&server->new_xdg_surface.link);
        wl_list_init(&server->new_xdg_surface.link);
    }

    if (server->new_xdg_toplevel.link.prev && server->new_xdg_toplevel.link.next)
    {
        wl_list_remove(&server->new_xdg_toplevel.link);
        wl_list_init(&server->new_xdg_toplevel.link);
    }

    if (server->new_xdg_popup.link.prev && server->new_xdg_popup.link.next)
    {
        wl_list_remove(&server->new_xdg_popup.link);
        wl_list_init(&server->new_xdg_popup.link);
    }

    if (server->new_xdg_toplevel_decoration.link.prev && server->new_xdg_toplevel_decoration.link.next)
    {
        wl_list_remove(&server->new_xdg_toplevel_decoration.link);
        wl_list_init(&server->new_xdg_toplevel_decoration.link);
    }

    if (server->output && server->output_frame.link.prev && server->output_frame.link.next)
    {
        wl_list_remove(&server->output_frame.link);
        wl_list_init(&server->output_frame.link);
    }

    if (server->output && server->output_destroy.link.prev && server->output_destroy.link.next)
    {
        wl_list_remove(&server->output_destroy.link);
        wl_list_init(&server->output_destroy.link);
    }

    if (server->display)
    {
        wl_display_destroy_clients(server->display);
        wl_display_destroy(server->display);
        server->display = NULL;
    }

    if (server->output_layout)
    {
        wlr_output_layout_destroy(server->output_layout);
        server->output_layout = NULL;
    }

    free(server->framebuffer_xrgb8888);
    server->framebuffer_xrgb8888 = NULL;
}

int wd_server_run(struct wd_server* server) {
#if WAYDISPLAY_ENABLE_LOGGING && WAYDISPLAY_ENABLE_DEBUG_LOGGING
    wlr_log_init(WLR_DEBUG, NULL);
#else
    wlr_log_init(WLR_ERROR, NULL);
#endif

    g_server_for_signal   = server;
    g_terminate_requested = 0;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    pthread_t net_thread;

    if (pthread_create(&net_thread, NULL, wd_net_thread_main, server) != 0)
    {
        return 1;
    }

    if (!launch_startup_command(server))
    {
        pthread_mutex_lock(&server->net.lock);
        server->net.running = false;
        pthread_cond_broadcast(&server->net.display_resize_cond);
        pthread_mutex_unlock(&server->net.lock);
        pthread_join(net_thread, NULL);
        return 1;
    }

    wl_display_run(server->display);

    pthread_mutex_lock(&server->net.lock);
    server->net.running = false;
    pthread_cond_broadcast(&server->net.display_resize_cond);
    pthread_mutex_unlock(&server->net.lock);

    if (server->net.listen_fd >= 0)
    {
        shutdown(server->net.listen_fd, SHUT_RDWR);
    }

    if (server->net.tcp_fd >= 0)
    {
        shutdown(server->net.tcp_fd, SHUT_RDWR);
    }

    pthread_join(net_thread, NULL);

    return 0;
}
