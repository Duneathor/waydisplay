#include "wd_server.h"
#include "wd_dirty_region_scheduler.h"
#include "wd_async_tcp.h"
#include "wd_async_udp.h"

#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

static struct wd_server*     g_server_for_signal   = NULL;
static volatile sig_atomic_t g_terminate_requested = 0;


static uint64_t wd_server_clamp_u64(uint64_t value, uint64_t min_value, uint64_t max_value) {
    if (value < min_value)
    {
        return min_value;
    }
    if (value > max_value)
    {
        return max_value;
    }
    return value;
}

static uint64_t wd_server_delta_summary_interval_locked(struct wd_server* server, uint64_t now_ns) {
    (void)now_ns;
    if (!server)
    {
        return WD_LINK_CLEAN_SUMMARY_INTERVAL_DEFAULT_NS;
    }

    struct wd_net_state* net = &server->net;
    bool active = net->retransmit_queue_count != 0 || net->stats.retx_req_rx != 0 ||
                  net->stats.retx_req_stale_generation != 0 || net->stats.retx_req_upgraded_generation != 0 ||
                  net->stats.retx_tiles_superseded_by_fresh != 0 ||
                  net->stats.client_partial_tiles_timed_out != 0 || net->stats.client_retx_requests_tx != 0;

    uint64_t interval_ns = active ? net->active_summary_interval_ns : net->clean_summary_interval_ns;
    if (interval_ns == 0)
    {
        interval_ns = active ? WD_LINK_ACTIVE_SUMMARY_INTERVAL_DEFAULT_NS : WD_LINK_CLEAN_SUMMARY_INTERVAL_DEFAULT_NS;
    }

    if (active)
    {
        uint64_t stale_or_superseded = net->stats.retx_req_stale_generation + net->stats.retx_tiles_superseded_by_fresh;
        uint64_t repair_activity = stale_or_superseded + net->stats.retx_tiles_req + net->stats.retx_req_ignored_live;
        if (stale_or_superseded >= 16 && repair_activity != 0 &&
            stale_or_superseded * 100ull >= repair_activity * (uint64_t)WD_LINK_STALE_REPAIR_BACKOFF_PERCENT)
        {
            interval_ns *= WD_LINK_STALE_REPAIR_BACKOFF_MULTIPLIER;
            interval_ns = wd_server_clamp_u64(interval_ns,
                                              WD_LINK_ACTIVE_SUMMARY_INTERVAL_MIN_NS,
                                              WD_LINK_ACTIVE_SUMMARY_INTERVAL_MAX_NS);
            net->stats.tcp_summary_repair_backoff++;
        }
    }

    net->stats.tcp_summary_budget_interval_ns = interval_ns;
    return interval_ns;
}

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

    WD_LOG_INFO("launched app pid=%d command=%s", pid, server->startup_command);

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

static void server_drain_pending_input(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    /*
     * Keep this ordering shared by the eventfd wakeup and the periodic frame
     * timer. A held Ctrl key must be injected before clipboard paste
     * synthesizes V, and primary selection must be published before a queued
     * middle-click is injected.
     */
    wd_keyboard_drain_and_inject(server);
    wd_clipboard_drain_and_apply(server);
    wd_pointer_drain_and_inject(server);
}

void wd_server_wake_input(struct wd_server* server) {
    if (!server || server->input_wakeup_fd < 0)
    {
        return;
    }

    const uint64_t value = 1;
    ssize_t        written;

    do
    {
        written = write(server->input_wakeup_fd, &value, sizeof(value));
    } while (written < 0 && errno == EINTR);

    pthread_mutex_lock(&server->net.lock);
    if (written == (ssize_t)sizeof(value))
    {
        server->net.stats.input_wakeup_signals++;
    }
    else
    {
        server->net.stats.input_wakeup_failures++;
    }
    pthread_mutex_unlock(&server->net.lock);
}

static int server_input_wakeup(int fd, uint32_t mask, void* data) {
    struct wd_server* server = data;
    uint64_t          wake_events = 0;
    bool              read_failed = false;

    if (!server)
    {
        return 0;
    }

    if ((mask & WL_EVENT_READABLE) != 0)
    {
        for (;;)
        {
            uint64_t value = 0;
            ssize_t  received = read(fd, &value, sizeof(value));

            if (received == (ssize_t)sizeof(value))
            {
                wake_events += value;
                continue;
            }
            if (received < 0 && errno == EINTR)
            {
                continue;
            }
            if (received < 0 && errno == EAGAIN)
            {
                break;
            }

            read_failed = true;
            break;
        }
    }

    server_drain_pending_input(server);

    pthread_mutex_lock(&server->net.lock);
    server->net.stats.input_wakeup_callbacks++;
    server->net.stats.input_wakeup_events += wake_events;
    if (wake_events > 1)
    {
        server->net.stats.input_wakeup_coalesced += wake_events - 1;
    }
    if (read_failed || (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0)
    {
        server->net.stats.input_wakeup_failures++;
    }
    pthread_mutex_unlock(&server->net.lock);

    return 0;
}

static void wd_server_reap_and_sample_async_locked(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    wd_async_tcp_sender_reap(server->net.control_tx);
    wd_async_tcp_sender_reap(server->net.video_tx);
    wd_async_udp_sender_reap(server->net.udp_tx);

    if (server->net.control_tx)
    {
        uint64_t queued = wd_async_tcp_sender_queued(server->net.control_tx);
        uint64_t completed = wd_async_tcp_sender_completed(server->net.control_tx);
        uint64_t failed = wd_async_tcp_sender_failed(server->net.control_tx);
        uint64_t partial = wd_async_tcp_sender_partial_resubmits(server->net.control_tx);
        uint64_t overflows = wd_async_tcp_sender_overflows(server->net.control_tx);
        if (queued > server->net.control_tx_queued_seen)
        {
            server->net.stats.tcp_async_queued += queued - server->net.control_tx_queued_seen;
            server->net.control_tx_queued_seen = queued;
        }
        if (completed > server->net.control_tx_completed_seen)
        {
            server->net.stats.tcp_async_completed += completed - server->net.control_tx_completed_seen;
            server->net.control_tx_completed_seen = completed;
        }
        if (partial > server->net.control_tx_partial_seen)
        {
            server->net.stats.tcp_async_partial_resubmits += partial - server->net.control_tx_partial_seen;
            server->net.control_tx_partial_seen = partial;
        }
        if (overflows > server->net.control_tx_overflow_seen)
        {
            server->net.stats.tcp_async_queue_overflow += overflows - server->net.control_tx_overflow_seen;
            server->net.control_tx_overflow_seen = overflows;
        }
        uint64_t inflight_max = wd_async_tcp_sender_inflight_max(server->net.control_tx);
        if (inflight_max > server->net.stats.tcp_async_inflight_max)
        {
            server->net.stats.tcp_async_inflight_max = inflight_max;
        }
        if (failed > server->net.control_tx_failed_seen)
        {
            server->net.stats.tcp_async_completion_failed += failed - server->net.control_tx_failed_seen;
            server->net.control_tx_failed_seen = failed;
            if (server->net.tcp_fd >= 0)
            {
                (void)shutdown(server->net.tcp_fd, SHUT_RDWR);
            }
        }
    }

    if (server->net.video_tx)
    {
        const uint64_t failed = wd_async_tcp_sender_failed(server->net.video_tx);
        if (failed > server->net.video_tx_failed_seen)
        {
            const uint64_t new_failures = failed - server->net.video_tx_failed_seen;
            server->net.video_tx_failed_seen = failed;
            server->net.stats.video_tcp_send_failed += new_failures;

            if (server->net.video_tcp_fd >= 0)
            {
                WD_LOG_ERROR("video TCP async completion failed; returning display ownership to tiles");
                wd_stream_video_reset_locked(server, "video async completion failed", false, false);
                wd_stream_invalidate_all_tiles_locked(server);
                (void)shutdown(server->net.video_tcp_fd, SHUT_RDWR);
            }
        }
    }

    if (server->net.udp_tx)
    {
        uint64_t queued = wd_async_udp_sender_queued(server->net.udp_tx);
        uint64_t completed = wd_async_udp_sender_completed(server->net.udp_tx);
        uint64_t failed = wd_async_udp_sender_failed(server->net.udp_tx);
        uint64_t fallbacks = wd_async_udp_sender_fallbacks(server->net.udp_tx);
        uint64_t submit_calls = wd_async_udp_sender_submit_calls(server->net.udp_tx);
        uint64_t partial_submits = wd_async_udp_sender_partial_submits(server->net.udp_tx);
        if (queued > server->net.udp_tx_queued_seen)
        {
            server->net.stats.udp_async_queued += queued - server->net.udp_tx_queued_seen;
            server->net.udp_tx_queued_seen = queued;
        }
        if (completed > server->net.udp_tx_completed_seen)
        {
            server->net.stats.udp_async_completed += completed - server->net.udp_tx_completed_seen;
            server->net.udp_tx_completed_seen = completed;
        }
        if (fallbacks > server->net.udp_tx_fallback_seen)
        {
            server->net.stats.udp_async_fallback_sync += fallbacks - server->net.udp_tx_fallback_seen;
            server->net.udp_tx_fallback_seen = fallbacks;
        }
        if (submit_calls > server->net.udp_tx_submit_calls_seen)
        {
            server->net.stats.udp_async_submit_calls += submit_calls - server->net.udp_tx_submit_calls_seen;
            server->net.udp_tx_submit_calls_seen = submit_calls;
        }
        if (partial_submits > server->net.udp_tx_partial_submits_seen)
        {
            server->net.stats.udp_async_partial_submits += partial_submits - server->net.udp_tx_partial_submits_seen;
            server->net.udp_tx_partial_submits_seen = partial_submits;
        }
        uint64_t inflight_max = wd_async_udp_sender_inflight_max(server->net.udp_tx);
        if (inflight_max > server->net.stats.udp_async_inflight_max)
        {
            server->net.stats.udp_async_inflight_max = inflight_max;
        }
        if (failed > server->net.udp_tx_failed_seen)
        {
            server->net.stats.udp_async_completion_failed += failed - server->net.udp_tx_failed_seen;
            server->net.udp_tx_failed_seen = failed;
        }
    }
}

static bool server_promote_wlroots_scene_damage(struct wd_server* server) {
    if (!server || !server->scene_output || !wlr_scene_output_needs_frame(server->scene_output))
    {
        return false;
    }

    /*
     * The wlroots scene graph is the authoritative source of render damage.
     * Manual view/surface commit trackers are useful for deriving a smaller
     * readback box, but they can miss toolkit-specific subsurface or buffer
     * commits. If wlroots says a frame is needed and WayDisplay has no matching
     * damage, conservatively promote it to a full-output update.
     */
    if (!server->scene_dirty || (!server->damage_all_tiles && server->damage_tile_count == 0))
    {
        wd_server_mark_scene_dirty(server);
        return true;
    }

    return false;
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

    const uint64_t timer_start_ns = wd_now_ns();

    /* Rearm before render/readback/encode. If that work exceeds the 8 ms
     * compositor tick, the timer is already ready when this callback returns
     * instead of adding another unconditional 8 ms of latency. */
    wl_event_source_timer_update(server->frame_timer, 8);

    server_process_pending_display_resize(server);

    /* Periodic fallback in case a wakeup is coalesced or unavailable. */
    server_drain_pending_input(server);

    uint64_t t = wd_now_ns();

    pthread_mutex_lock(&server->net.lock);
    wd_server_reap_and_sample_async_locked(server);
    wd_cursor_flush_pending_locked(server);
    pthread_mutex_unlock(&server->net.lock);

    const bool scene_damage_promoted = server_promote_wlroots_scene_damage(server);

    bool should_render = wd_stream_policy_should_render_now(server, t);
    bool render_attempted = false;
    bool tile_stream_pass = false;
    uint64_t render_readback_ns = 0;
    enum wd_render_result render_result = WD_RENDER_RESULT_IDLE;

    if (should_render)
    {
        if (server->output)
        {
            wlr_output_schedule_frame(server->output);
        }

        const uint64_t render_start_ns = wd_now_ns();
        render_result = wd_render_scene_and_readback_xrgb8888(server);
        render_readback_ns = wd_now_ns() - render_start_ns;
        render_attempted = true;

        if (render_result == WD_RENDER_RESULT_FRAME)
        {
            tile_stream_pass = wd_stream_send_dirty_tiles(server);
        }
        else
        {
            /*
             * An idle scene is expected when an output was scheduled without
             * new wlroots damage. A renderer/backend failure remains distinct
             * for diagnostics. In both cases stop this attempt; the next
             * wlroots needs-frame transition is promoted above.
             */
            server->scene_dirty = false;
        }
    }

    /* Dirty and repair queues consume the most recently captured framebuffer;
     * they do not require wlroots to render an unchanged scene. Service queued
     * work every compositor tick, including after a static-scene idle result. */
    if (!tile_stream_pass)
    {
        (void)wd_stream_service_tile_queues(server);
    }

    if (server->last_summary_ns == 0 || t - server->last_summary_ns >= WD_GENERATION_SUMMARY_FULL_SANITY_INTERVAL_NS)
    {
        pthread_mutex_lock(&server->net.lock);
        bool summary_sent = wd_stream_send_generation_summary_locked(server);
        pthread_mutex_unlock(&server->net.lock);

        if (summary_sent)
        {
            server->last_summary_ns       = t;
            server->last_delta_summary_ns = t;
        }
    }
    else
    {
        uint64_t delta_interval_ns = WD_LINK_CLEAN_SUMMARY_INTERVAL_DEFAULT_NS;
        pthread_mutex_lock(&server->net.lock);
        delta_interval_ns = wd_server_delta_summary_interval_locked(server, t);
        const bool should_send_delta = server->last_delta_summary_ns == 0 || t - server->last_delta_summary_ns >= delta_interval_ns;
        if (should_send_delta && server->net.summary_dirty_count != 0)
        {
            if (wd_stream_send_pending_generation_summary_locked(server))
            {
                server->last_delta_summary_ns = t;
            }
        }
        pthread_mutex_unlock(&server->net.lock);
    }

    if (t - server->last_stats_ns >= WD_SERVER_STATS_HEALTH_INTERVAL_NS)
    {
        const bool log_stats = server->last_stats_log_ns == 0 || t - server->last_stats_log_ns >= WD_STATS_LOG_INTERVAL_NS;

        pthread_mutex_lock(&server->net.lock);
        wd_server_reap_and_sample_async_locked(server);
        pthread_mutex_unlock(&server->net.lock);

        wd_stream_sample_and_maybe_log_stats(server, log_stats);
        server->last_stats_ns = t;
        if (log_stats)
        {
            server->last_stats_log_ns = t;
        }
    }

    const uint64_t timer_elapsed_ns = wd_now_ns() - timer_start_ns;
    pthread_mutex_lock(&server->net.lock);
    server->net.stats.server_frame_timer_samples++;
    server->net.stats.server_frame_timer_sum_ns += timer_elapsed_ns;
    if (timer_elapsed_ns > server->net.stats.server_frame_timer_max_ns)
    {
        server->net.stats.server_frame_timer_max_ns = timer_elapsed_ns;
    }
    if (scene_damage_promoted)
    {
        server->net.stats.server_scene_damage_promotions++;
    }
    if (render_attempted)
    {
        server->net.stats.server_render_readback_samples++;
        server->net.stats.server_render_readback_sum_ns += render_readback_ns;
        if (render_readback_ns > server->net.stats.server_render_readback_max_ns)
        {
            server->net.stats.server_render_readback_max_ns = render_readback_ns;
        }
        if (render_result == WD_RENDER_RESULT_IDLE)
        {
            server->net.stats.server_render_idle_results++;
        }
        else if (render_result == WD_RENDER_RESULT_ERROR)
        {
            server->net.stats.server_render_failed_results++;
        }
    }
    pthread_mutex_unlock(&server->net.lock);

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

    uint8_t tile_size = 0;
    if (!wd_tile_size_code_for_dimensions(tile_width, tile_height, &tile_size))
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
    if (!server || width == 0 || height == 0 || width > WD_MAX_RENDER_WIDTH || height > WD_MAX_RENDER_HEIGHT)
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
    if (!server || width == 0 || height == 0 || width > WD_MAX_RENDER_WIDTH || height > WD_MAX_RENDER_HEIGHT)
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


struct wd_display_geometry_snapshot {
    uint32_t display_width;
    uint32_t display_height;
    uint16_t tiles_x;
    uint16_t tiles_y;
    uint16_t total_tiles;
    uint16_t base_tile_width;
    uint16_t base_tile_height;
    uint16_t base_tiles_x;
    uint16_t base_tiles_y;
    uint32_t total_base_tiles;
    uint32_t framebuffer_pixels;
    uint32_t framebuffer_bytes;
};

struct wd_resize_allocations {
    uint32_t* framebuffer_xrgb8888;
    uint32_t* framebuffer_shadow_xrgb8888;
    struct wd_tile_state* tiles;
    bool* damage_tiles;
    uint16_t* dirty_regions;
    bool* dirty_region_queued;
    uint64_t* dirty_region_enqueued_ns;
    uint64_t* dirty_epochs;
    uint16_t* dirty_queue;
    bool* dirty_queued;
    uint64_t* dirty_queue_enqueued_ns;
    uint16_t* retransmit_queue;
    bool* retransmit_queued;
    uint64_t* retransmit_queue_enqueued_ns;
    uint64_t* retransmit_requested_generation;
    bool* summary_dirty_tiles;
    uint16_t* summary_dirty_queue;
};

static struct wd_display_geometry_snapshot wd_server_capture_geometry(const struct wd_server* server) {
    struct wd_display_geometry_snapshot geometry;
    memset(&geometry, 0, sizeof(geometry));
    if (!server)
    {
        return geometry;
    }

    geometry.display_width      = server->display_width;
    geometry.display_height     = server->display_height;
    geometry.tiles_x            = server->tiles_x;
    geometry.tiles_y            = server->tiles_y;
    geometry.total_tiles        = server->total_tiles;
    geometry.base_tile_width    = server->base_tile_width;
    geometry.base_tile_height   = server->base_tile_height;
    geometry.base_tiles_x       = server->base_tiles_x;
    geometry.base_tiles_y       = server->base_tiles_y;
    geometry.total_base_tiles   = server->total_base_tiles;
    geometry.framebuffer_pixels = server->framebuffer_pixels;
    geometry.framebuffer_bytes  = server->framebuffer_bytes;
    return geometry;
}

static void wd_server_restore_geometry(struct wd_server* server, const struct wd_display_geometry_snapshot* geometry) {
    if (!server || !geometry)
    {
        return;
    }

    server->display_width      = geometry->display_width;
    server->display_height     = geometry->display_height;
    server->tiles_x            = geometry->tiles_x;
    server->tiles_y            = geometry->tiles_y;
    server->total_tiles        = geometry->total_tiles;
    server->base_tile_width    = geometry->base_tile_width;
    server->base_tile_height   = geometry->base_tile_height;
    server->base_tiles_x       = geometry->base_tiles_x;
    server->base_tiles_y       = geometry->base_tiles_y;
    server->total_base_tiles   = geometry->total_base_tiles;
    server->framebuffer_pixels = geometry->framebuffer_pixels;
    server->framebuffer_bytes  = geometry->framebuffer_bytes;
}

static void wd_resize_allocations_free(struct wd_resize_allocations* allocs) {
    if (!allocs)
    {
        return;
    }

    free(allocs->framebuffer_xrgb8888);
    free(allocs->framebuffer_shadow_xrgb8888);
    free(allocs->tiles);
    free(allocs->damage_tiles);
    free(allocs->dirty_regions);
    free(allocs->dirty_region_queued);
    free(allocs->dirty_region_enqueued_ns);
    free(allocs->dirty_epochs);
    free(allocs->dirty_queue);
    free(allocs->dirty_queued);
    free(allocs->dirty_queue_enqueued_ns);
    free(allocs->retransmit_queue);
    free(allocs->retransmit_queued);
    free(allocs->retransmit_queue_enqueued_ns);
    free(allocs->retransmit_requested_generation);
    free(allocs->summary_dirty_tiles);
    free(allocs->summary_dirty_queue);
    memset(allocs, 0, sizeof(*allocs));
}

static bool wd_resize_allocations_prepare(struct wd_resize_allocations* allocs, const struct wd_server* server) {
    if (!allocs || !server || server->total_tiles == 0 || server->total_base_tiles == 0 || server->framebuffer_pixels == 0)
    {
        return false;
    }

    memset(allocs, 0, sizeof(*allocs));

    allocs->framebuffer_xrgb8888 = calloc(server->framebuffer_pixels, sizeof(*allocs->framebuffer_xrgb8888));
    allocs->framebuffer_shadow_xrgb8888 = calloc(server->framebuffer_pixels, sizeof(*allocs->framebuffer_shadow_xrgb8888));
    allocs->tiles                = calloc(server->total_tiles, sizeof(*allocs->tiles));
    allocs->damage_tiles         = calloc(server->total_base_tiles, sizeof(*allocs->damage_tiles));
    allocs->dirty_regions        = calloc(server->total_tiles, sizeof(*allocs->dirty_regions));
    allocs->dirty_region_queued  = calloc(server->total_tiles, sizeof(*allocs->dirty_region_queued));
    allocs->dirty_region_enqueued_ns = calloc(server->total_tiles, sizeof(*allocs->dirty_region_enqueued_ns));
    allocs->dirty_epochs         = calloc(server->total_tiles, sizeof(*allocs->dirty_epochs));
    allocs->dirty_queue          = calloc(server->total_tiles, sizeof(*allocs->dirty_queue));
    allocs->dirty_queued         = calloc(server->total_tiles, sizeof(*allocs->dirty_queued));
    allocs->dirty_queue_enqueued_ns = calloc(server->total_tiles, sizeof(*allocs->dirty_queue_enqueued_ns));
    allocs->retransmit_queue     = calloc(server->total_tiles, sizeof(*allocs->retransmit_queue));
    allocs->retransmit_queued    = calloc(server->total_tiles, sizeof(*allocs->retransmit_queued));
    allocs->retransmit_queue_enqueued_ns = calloc(server->total_tiles, sizeof(*allocs->retransmit_queue_enqueued_ns));
    allocs->retransmit_requested_generation = calloc(server->total_tiles, sizeof(*allocs->retransmit_requested_generation));
    allocs->summary_dirty_tiles  = calloc(server->total_tiles, sizeof(*allocs->summary_dirty_tiles));
    allocs->summary_dirty_queue  = calloc(server->total_tiles, sizeof(*allocs->summary_dirty_queue));

    if (!allocs->framebuffer_xrgb8888 || !allocs->framebuffer_shadow_xrgb8888 || !allocs->tiles ||
        !allocs->damage_tiles || !allocs->dirty_regions ||
        !allocs->dirty_region_queued || !allocs->dirty_region_enqueued_ns || !allocs->dirty_epochs || !allocs->dirty_queue || !allocs->dirty_queued ||
        !allocs->dirty_queue_enqueued_ns || !allocs->retransmit_queue || !allocs->retransmit_queued ||
        !allocs->retransmit_queue_enqueued_ns || !allocs->retransmit_requested_generation ||
        !allocs->summary_dirty_tiles || !allocs->summary_dirty_queue)
    {
        wd_resize_allocations_free(allocs);
        return false;
    }

    return true;
}

static void wd_server_free_net_resize_arrays(struct wd_net_state* net) {
    if (!net)
    {
        return;
    }

    wd_dirty_region_scheduler_destroy(net->dirty_region_scheduler);
    net->dirty_region_scheduler = NULL;

    free(net->dirty_regions);
    net->dirty_regions = NULL;
    free(net->dirty_region_queued);
    net->dirty_region_queued = NULL;
    free(net->dirty_region_enqueued_ns);
    net->dirty_region_enqueued_ns = NULL;
    free(net->dirty_epochs);
    net->dirty_epochs = NULL;
    free(net->dirty_queue);
    net->dirty_queue = NULL;
    free(net->dirty_queued);
    net->dirty_queued = NULL;
    free(net->dirty_queue_enqueued_ns);
    net->dirty_queue_enqueued_ns = NULL;
    free(net->retransmit_queue);
    net->retransmit_queue = NULL;
    free(net->retransmit_queued);
    net->retransmit_queued = NULL;
    free(net->retransmit_queue_enqueued_ns);
    net->retransmit_queue_enqueued_ns = NULL;
    free(net->retransmit_requested_generation);
    net->retransmit_requested_generation = NULL;
    free(net->summary_dirty_tiles);
    net->summary_dirty_tiles = NULL;
    free(net->summary_dirty_queue);
    net->summary_dirty_queue = NULL;
}

static void wd_server_free_resize_stream_state(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    free(server->net.tiles);
    server->net.tiles = NULL;

    free(server->damage_tiles);
    server->damage_tiles      = NULL;
    server->damage_all_tiles  = false;
    server->damage_tile_count = 0;

    wd_server_free_net_resize_arrays(&server->net);
}

static void wd_server_begin_config_update_locked(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    /* session_id and connection_token identify the transport lifetime. A
     * resize is only a new configuration/content epoch. Keeping the transport
     * identity stable lets the client retain its UDP socket and io_uring
     * receiver. UDP safety comes from two independent barriers:
     *
     *  - resize advances content_epoch, so packets queued before the resize
     *    are stale after the new configuration is installed; and
     *  - config_update_pending blocks all newly generated tile traffic until
     *    the client acknowledges the matching config_epoch.
     */
    server->net.config_update_pending = true;
    server->net.config_update_sent_ns = 0;
}

bool wd_server_apply_display_size(struct wd_server* server, uint32_t width, uint32_t height) {
    if (!server || width == 0 || height == 0 || width > WD_MAX_RENDER_WIDTH || height > WD_MAX_RENDER_HEIGHT)
    {
        return false;
    }

    if (server->display_width == width && server->display_height == height)
    {
        return true;
    }

    const struct wd_display_geometry_snapshot old_geometry = wd_server_capture_geometry(server);

    if (!wd_server_set_geometry(server, width, height))
    {
        wd_server_restore_geometry(server, &old_geometry);
        return false;
    }

    struct wd_resize_allocations next_allocs;
    if (!wd_resize_allocations_prepare(&next_allocs, server))
    {
        wd_server_restore_geometry(server, &old_geometry);
        return false;
    }

    if (!wd_wlroots_resize_headless_output(server))
    {
        wd_resize_allocations_free(&next_allocs);
        wd_server_restore_geometry(server, &old_geometry);
        return false;
    }

    pthread_mutex_lock(&server->net.lock);

    if (server->net.control_tx)
    {
        (void)wd_async_tcp_sender_drop_message_type(server->net.control_tx, WD_MSG_TILE_GENERATION_SUMMARY);
    }
    server->net.summary_epoch++;
    if (server->net.summary_epoch == 0)
    {
        server->net.summary_epoch = 1;
    }

    wd_stream_wait_for_encoder_idle_locked(server);

    uint32_t* old_framebuffer = server->framebuffer_xrgb8888;
    uint32_t* old_framebuffer_shadow = server->framebuffer_shadow_xrgb8888;
    wd_server_free_resize_stream_state(server);

    server->framebuffer_xrgb8888 = next_allocs.framebuffer_xrgb8888;
    server->framebuffer_shadow_xrgb8888 = next_allocs.framebuffer_shadow_xrgb8888;
    server->framebuffer_shadow_valid = false;
    server->net.tiles = next_allocs.tiles;
    server->damage_tiles = next_allocs.damage_tiles;
    server->net.dirty_regions = next_allocs.dirty_regions;
    server->net.dirty_region_queued = next_allocs.dirty_region_queued;
    server->net.dirty_region_enqueued_ns = next_allocs.dirty_region_enqueued_ns;
    server->net.dirty_epochs = next_allocs.dirty_epochs;
    server->net.dirty_queue = next_allocs.dirty_queue;
    server->net.dirty_queued = next_allocs.dirty_queued;
    server->net.dirty_queue_enqueued_ns = next_allocs.dirty_queue_enqueued_ns;
    server->net.retransmit_queue = next_allocs.retransmit_queue;
    server->net.retransmit_queued = next_allocs.retransmit_queued;
    server->net.retransmit_queue_enqueued_ns = next_allocs.retransmit_queue_enqueued_ns;
    server->net.retransmit_requested_generation = next_allocs.retransmit_requested_generation;
    server->net.summary_dirty_tiles = next_allocs.summary_dirty_tiles;
    server->net.summary_dirty_queue = next_allocs.summary_dirty_queue;
    memset(&next_allocs, 0, sizeof(next_allocs));

    free(old_framebuffer);
    free(old_framebuffer_shadow);

    server->framebuffer_generation++;
    if (server->framebuffer_generation == 0)
    {
        server->framebuffer_generation = 1;
    }
    server->net.config_epoch++;
    server->net.input_correlation_inflight_sequence = 0;
    if (server->net.config_epoch == 0)
    {
        server->net.config_epoch = 1;
    }

    server->net.dirty_region_cursor    = 0;
    server->net.dirty_region_count     = 0;
    server->net.dirty_queue_read       = 0;
    server->net.dirty_queue_write      = 0;
    server->net.dirty_queue_count      = 0;
    server->net.retransmit_queue_count = 0;
    server->net.summary_dirty_count    = 0;
    server->last_summary_ns            = 0;
    server->last_delta_summary_ns      = 0;
    wd_stream_video_reset_locked(server, "display resize", true, true);
    wd_server_begin_config_update_locked(server);
    wd_stream_invalidate_all_tiles_locked(server);

    pthread_mutex_unlock(&server->net.lock);

    /*
     * Reconfigure mapped views only after the stream/framebuffer state has
     * been rebuilt for the new geometry. These hooks can send configure
     * events that trigger client commits and dirty tracking, so running them
     * while resize is still swapping the old stream state can corrupt
     * resize-time queues.
     */
    wd_scene_handle_output_resize(server);
#if WAYDISPLAY_ENABLE_XWAYLAND
    wd_xwayland_handle_output_resize(server);
#endif

    return true;
}

bool wd_server_init(struct wd_server* server, uint16_t tcp_port, const char* app_cmd, double output_scale,
                    uint16_t output_refresh_hz, uint32_t display_width, uint32_t display_height,
                    uint16_t tile_width, uint16_t tile_height, bool enable_xwayland,
                    const char* video_encoder_backend, const char* vaapi_device) {
    memset(server, 0, sizeof(*server));
    server->input_wakeup_fd = -1;

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

    server->startup_command      = app_cmd;
    server->video_encoder_backend = video_encoder_backend;
    server->vaapi_device         = vaapi_device;
    server->output_scale         = output_scale;
    server->output_refresh_mhz = (uint32_t)(output_refresh_hz != 0 ? output_refresh_hz : 60u) * 1000u;
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
    server->framebuffer_shadow_xrgb8888 = calloc(server->framebuffer_pixels, sizeof(uint32_t));
    if (server->framebuffer_xrgb8888 && server->framebuffer_shadow_xrgb8888)
    {
        server->framebuffer_generation = 1;
        server->framebuffer_shadow_valid = false;
    }

    if (!server->framebuffer_xrgb8888 || !server->framebuffer_shadow_xrgb8888)
    {
        free(server->framebuffer_xrgb8888);
        server->framebuffer_xrgb8888 = NULL;
        free(server->framebuffer_shadow_xrgb8888);
        server->framebuffer_shadow_xrgb8888 = NULL;
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

    server->last_summary_ns   = wd_now_ns();
    server->last_stats_ns     = server->last_summary_ns;
    server->last_stats_log_ns = server->last_summary_ns;

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

    server->input_wakeup_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (server->input_wakeup_fd < 0)
    {
        WD_LOG_ERROR("failed to create input eventfd: %s", strerror(errno));
        return false;
    }

    server->input_wakeup_source = wl_event_loop_add_fd(server->event_loop, server->input_wakeup_fd, WL_EVENT_READABLE,
                                                        server_input_wakeup, server);
    if (!server->input_wakeup_source)
    {
        WD_LOG_ERROR("failed to add input eventfd to Wayland event loop");
        close(server->input_wakeup_fd);
        server->input_wakeup_fd = -1;
        return false;
    }

    server->frame_timer = wl_event_loop_add_timer(server->event_loop, server_frame_timer, server);

    if (!server->frame_timer)
    {
        WD_LOG_ERROR("failed to create frame timer");
        return false;
    }

    wl_event_source_timer_update(server->frame_timer, 1);

    setenv("WAYLAND_DISPLAY", server->socket_name, 1);

    WD_LOG_INFO("running on WAYLAND_DISPLAY=%s", server->socket_name);

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

    if (server->input_wakeup_source)
    {
        wl_event_source_remove(server->input_wakeup_source);
        server->input_wakeup_source = NULL;
    }
    if (server->input_wakeup_fd >= 0)
    {
        close(server->input_wakeup_fd);
        server->input_wakeup_fd = -1;
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
    free(server->framebuffer_shadow_xrgb8888);
    server->framebuffer_shadow_xrgb8888 = NULL;
    server->framebuffer_shadow_valid = false;
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
