#include "waydisplay/wd_eventfd.h"
#include "wd_server_internal.h"

#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "wd_async_tcp.h"
#include "wd_async_udp.h"
#include "wd_audio_routing.h"
#include "wd_audio_stream.h"
#include "wd_dirty_region_scheduler.h"
#include "wd_stream_pipeline_internal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
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
    bool active = net->retransmit_queue_count != 0 || net->stats.retx_req_rx != 0 || net->stats.retx_req_stale_generation != 0 ||
                  net->stats.retx_req_upgraded_generation != 0 || net->stats.retx_tiles_superseded_by_fresh != 0 ||
                  net->stats.client_partial_tiles_timed_out != 0 || net->stats.client_retx_requests_tx != 0;

    uint64_t interval_ns = active ? net->active_summary_interval_ns : net->clean_summary_interval_ns;
    if (interval_ns == 0)
    {
        interval_ns = active ? WD_LINK_ACTIVE_SUMMARY_INTERVAL_DEFAULT_NS : WD_LINK_CLEAN_SUMMARY_INTERVAL_DEFAULT_NS;
    }

    if (active)
    {
        uint64_t stale_or_superseded = net->stats.retx_req_stale_generation + net->stats.retx_tiles_superseded_by_fresh;
        uint64_t repair_activity     = stale_or_superseded + net->stats.retx_tiles_req + net->stats.retx_req_ignored_live;
        if (stale_or_superseded >= WD_SERVER_STALE_REPAIR_MIN_SAMPLES && repair_activity != 0 &&
            stale_or_superseded * 100ull >= repair_activity * (uint64_t)WD_LINK_STALE_REPAIR_BACKOFF_PERCENT)
        {
            interval_ns *= WD_LINK_STALE_REPAIR_BACKOFF_MULTIPLIER;
            interval_ns = wd_server_clamp_u64(interval_ns, WD_LINK_ACTIVE_SUMMARY_INTERVAL_MIN_NS, WD_LINK_ACTIVE_SUMMARY_INTERVAL_MAX_NS);
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

static void server_log_startup_process_status(const char* prefix, int status) {
    if (WIFEXITED(status))
    {
        WD_LOG_INFO("%s: exit_status=%d", prefix, WEXITSTATUS(status));
    }
    else if (WIFSIGNALED(status))
    {
        WD_LOG_INFO("%s: signal=%d", prefix, WTERMSIG(status));
    }
    else
    {
        WD_LOG_INFO("%s: status=0x%x", prefix, status);
    }
}

static void server_reap_startup_process(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    int                         status     = 0;
    int                         error_code = 0;
    enum wd_process_reap_result result     = wd_spawned_process_reap_nonblocking(&server->startup_process, &status, &error_code);
    if (result == WD_PROCESS_REAP_EXITED)
    {
        server_log_startup_process_status("startup command exited", status);
    }
    else if (result == WD_PROCESS_REAP_ERROR)
    {
        WD_LOG_ERROR("failed to reap startup command: %s", strerror(error_code));
    }

    if (server->startup_process.pid <= 0 && !wd_spawned_process_group_alive(&server->startup_process))
    {
        server->startup_process.process_group = -1;
    }
}

static void server_terminate_startup_process(struct wd_server* server) {
    if (!server || server->startup_process.process_group <= 0)
    {
        return;
    }

    int status     = 0;
    int error_code = 0;
    if (!wd_spawned_process_terminate_group(&server->startup_process, WD_SERVER_PROCESS_TERM_GRACE_MS, WD_SERVER_PROCESS_KILL_GRACE_MS,
                                            &status, &error_code))
    {
        WD_LOG_ERROR("failed to terminate startup process group: %s", strerror(error_code));
        return;
    }

    if (status != 0)
    {
        server_log_startup_process_status("startup command stopped", status);
    }
}

static bool launch_startup_command(struct wd_server* server) {
    if (!server->startup_command || server->startup_command[0] == '\0')
    {
        return true;
    }

    const char*                 audio_sink   = wd_audio_stream_sink_name(server->net.audio_stream);
    const char*                 audio_target = wd_audio_stream_sink_target(server->net.audio_stream);
    struct wd_audio_routing_env audio_routing;
    if (!wd_audio_routing_env_build(&audio_routing, audio_sink, audio_target, getpid()))
    {
        WD_LOG_ERROR("failed to construct private audio routing environment");
        return false;
    }

    struct wd_process_env_change environment[20];
    size_t                       environment_count = 0;
#define WD_ADD_ENV(name_value, value_value, action_value)                                                                                  \
    do                                                                                                                                     \
    {                                                                                                                                      \
        environment[environment_count++] =                                                                                                 \
            (struct wd_process_env_change){.name = (name_value), .value = (value_value), .action = (action_value)};                        \
    } while (0)

    WD_ADD_ENV("WAYLAND_DISPLAY", server->socket_name, WD_PROCESS_ENV_SET);
    WD_ADD_ENV("XDG_SESSION_TYPE", "wayland", WD_PROCESS_ENV_SET);
#if WAYDISPLAY_ENABLE_XWAYLAND
    if (server->xwayland && server->xwayland->display_name)
    {
        WD_ADD_ENV("DISPLAY", server->xwayland->display_name, WD_PROCESS_ENV_SET);
    }
    else
    {
        WD_ADD_ENV("DISPLAY", NULL, WD_PROCESS_ENV_UNSET);
    }
#else
    WD_ADD_ENV("DISPLAY", NULL, WD_PROCESS_ENV_UNSET);
#endif
    WD_ADD_ENV("WAYLAND_SOCKET", NULL, WD_PROCESS_ENV_UNSET);
    WD_ADD_ENV("GDK_BACKEND", "wayland", WD_PROCESS_ENV_SET);
    WD_ADD_ENV("QT_QPA_PLATFORM", "wayland", WD_PROCESS_ENV_SET);
    WD_ADD_ENV("SDL_VIDEODRIVER", "wayland", WD_PROCESS_ENV_SET);
    WD_ADD_ENV("CLUTTER_BACKEND", "wayland", WD_PROCESS_ENV_SET);
    WD_ADD_ENV("MOZ_ENABLE_WAYLAND", "1", WD_PROCESS_ENV_SET);

    if (audio_routing.enabled)
    {
        WD_ADD_ENV("PULSE_SINK", audio_routing.pulse_sink, WD_PROCESS_ENV_SET);
        WD_ADD_ENV("PULSE_PROP", audio_routing.pulse_props, WD_PROCESS_ENV_SET);
        WD_ADD_ENV("PIPEWIRE_NODE", audio_routing.pipewire_target, WD_PROCESS_ENV_SET);
        WD_ADD_ENV("PIPEWIRE_PROPS", audio_routing.pipewire_props, WD_PROCESS_ENV_SET);
    }
    else
    {
        WD_ADD_ENV("PULSE_SINK", NULL, WD_PROCESS_ENV_UNSET);
        WD_ADD_ENV("PULSE_PROP", NULL, WD_PROCESS_ENV_UNSET);
        WD_ADD_ENV("PIPEWIRE_NODE", NULL, WD_PROCESS_ENV_UNSET);
        WD_ADD_ENV("PIPEWIRE_PROPS", NULL, WD_PROCESS_ENV_UNSET);
    }
    WD_ADD_ENV("PIPEWIRE_ALSA", NULL, WD_PROCESS_ENV_UNSET);
#if WAYDISPLAY_ENABLE_XWAYLAND
    WD_ADD_ENV("_JAVA_AWT_WM_NONREPARENTING", "1", WD_PROCESS_ENV_SET_IF_ABSENT);
#endif
#undef WD_ADD_ENV

    int spawn_error = 0;
    if (!wd_spawn_shell_command(&server->startup_process, server->startup_command, environment, environment_count, &spawn_error))
    {
        WD_LOG_ERROR("failed to launch app command: %s", strerror(spawn_error));
        return false;
    }

    WD_LOG_INFO("launched app pid=%d process_group=%d command=%s audio_sink=%s "
                "pipewire_target=%s audio_scope=%s fallback=%s",
                server->startup_process.pid, server->startup_process.process_group, server->startup_command,
                audio_routing.enabled ? audio_routing.pulse_sink : "system-default",
                audio_routing.enabled ? audio_routing.pipewire_target : "system-default",
                audio_routing.enabled ? audio_routing.scope : "none", audio_routing.enabled ? "disabled" : "system-policy");

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

    uint64_t serial             = net->display_resize_request_serial;
    uint32_t width              = net->display_resize_width;
    uint32_t height             = net->display_resize_height;
    uint16_t refresh_hz         = net->display_resize_refresh_hz;
    net->display_resize_pending = false;
    pthread_mutex_unlock(&net->lock);

    bool ok = wd_server_apply_display_mode(server, width, height, refresh_hz);

    pthread_mutex_lock(&net->lock);
    net->display_resize_result           = ok;
    net->display_resize_completed_serial = serial;
    pthread_cond_broadcast(&net->display_resize_cond);
    pthread_mutex_unlock(&net->lock);
}

static void server_apply_pending_compositor_requests(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    if (!wd_stream_frame_worker_idle(server))
    {
        return;
    }

    const uint32_t requests = atomic_exchange_explicit(&server->compositor_requests, 0, memory_order_acq_rel);
    if ((requests & WD_COMPOSITOR_REQUEST_FULL_REFRESH) != 0)
    {
        wd_server_mark_scene_dirty(server);
        server->framebuffer_shadow_valid = false;
    }
}

void wd_server_request_full_refresh(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    atomic_fetch_or_explicit(&server->compositor_requests, WD_COMPOSITOR_REQUEST_FULL_REFRESH, memory_order_release);
    wd_server_wake_input(server);
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

    /* This helper is intentionally lock-free. Compositor requests and input
     * queue publication commonly wake the event loop while server->net.lock is
     * already held. Successful eventfd writes are counted from the drained
     * eventfd value in server_input_wakeup(); only failures need a side-band
     * counter. */
    if (!wd_eventfd_signal(server->input_wakeup_fd))
    {
        atomic_fetch_add_explicit(&server->input_wakeup_write_failures, 1, memory_order_relaxed);
    }
}

static int server_input_wakeup(int fd, uint32_t mask, void* data) {
    struct wd_server* server      = data;
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
            uint64_t value    = 0;
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

    server_apply_pending_compositor_requests(server);
    server_drain_pending_input(server);
    if (server->frame_timer && server->scene_dirty && wd_stream_frame_worker_idle(server))
    {
        wl_event_source_timer_update(server->frame_timer, (int)WD_SERVER_FRAME_SERVICE_MIN_INTERVAL_MS);
    }

    const uint64_t write_failures =
        atomic_exchange_explicit(&server->input_wakeup_write_failures, 0, memory_order_relaxed);

    pthread_mutex_lock(&server->net.lock);
    server->net.stats.input_wakeup_callbacks++;
    server->net.stats.input_wakeup_signals += wake_events;
    server->net.stats.input_wakeup_events += wake_events;
    if (wake_events > 1)
    {
        server->net.stats.input_wakeup_coalesced += wake_events - 1;
    }
    server->net.stats.input_wakeup_failures += write_failures;
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
        uint64_t queued    = wd_async_tcp_sender_queued(server->net.control_tx);
        uint64_t completed = wd_async_tcp_sender_completed(server->net.control_tx);
        uint64_t failed    = wd_async_tcp_sender_failed(server->net.control_tx);
        uint64_t partial   = wd_async_tcp_sender_partial_resubmits(server->net.control_tx);
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
            const uint64_t new_failures      = failed - server->net.video_tx_failed_seen;
            server->net.video_tx_failed_seen = failed;
            server->net.stats.video_tcp_send_failed += new_failures;

            if (server->net.video_tcp_fd >= 0)
            {
                WD_LOG_ERROR("video TCP async completion failed; returning display ownership to tiles");
                wd_stream_video_reset_locked(server, "video async completion failed", false, false);
                wd_stream_invalidate_all_tiles_locked(server);
                wd_server_mark_scene_dirty(server);
                server->framebuffer_shadow_valid = false;
                (void)shutdown(server->net.video_tcp_fd, SHUT_RDWR);
            }
        }
    }

    if (server->net.udp_tx)
    {
        uint64_t queued          = wd_async_udp_sender_queued(server->net.udp_tx);
        uint64_t completed       = wd_async_udp_sender_completed(server->net.udp_tx);
        uint64_t failed          = wd_async_udp_sender_failed(server->net.udp_tx);
        uint64_t sqe_exhaustions       = wd_async_udp_sender_sqe_exhaustions(server->net.udp_tx);
        uint64_t submit_calls    = wd_async_udp_sender_submit_calls(server->net.udp_tx);
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
        if (sqe_exhaustions > server->net.udp_tx_sqe_exhausted_seen)
        {
            server->net.stats.udp_async_sqe_exhausted += sqe_exhaustions - server->net.udp_tx_sqe_exhausted_seen;
            server->net.udp_tx_sqe_exhausted_seen = sqe_exhaustions;
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

    /* wlroots is authoritative about whether a frame is required. The exact
     * output damage is merged from wlr_output_state after the scene state has
     * been built, so this promotion only schedules the render and does not
     * broaden an existing manual damage set. */
    if (!server->scene_dirty)
    {
        server->scene_dirty = true;
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

    server_reap_startup_process(server);

    if (g_terminate_requested)
    {
        wl_display_terminate(server->display);
        return 0;
    }

    const uint64_t timer_start_ns = wd_now_ns();

    /* Rearm before compositor work. The interval follows the effective capture
     * target, capped by the backend refresh and by the queue-service ceiling.
     * If work exceeds the interval, the timer is ready when this callback
     * returns instead of adding another fixed delay. */
    wl_event_source_timer_update(server->frame_timer, (int)wd_stream_frame_service_interval_ms(server));

    const bool stream_worker_idle = wd_stream_frame_worker_idle(server);
    if (stream_worker_idle)
    {
        server_process_pending_display_resize(server);
    }

    /* Periodic fallback in case a wakeup is coalesced or unavailable. Frame
     * and shadow ownership remain with the compositor while the stream worker
     * is idle. */
    server_apply_pending_compositor_requests(server);
    server_drain_pending_input(server);

    uint64_t t = wd_now_ns();

    pthread_mutex_lock(&server->net.lock);
    wd_server_reap_and_sample_async_locked(server);
    wd_clipboard_send_pending_locked(server);
    wd_cursor_flush_pending_locked(server);
    pthread_mutex_unlock(&server->net.lock);

    const bool scene_damage_promoted = server_promote_wlroots_scene_damage(server);

    bool                  should_render      = stream_worker_idle && wd_stream_policy_should_render_now(server, t);
    bool                  render_attempted   = false;
    bool                  tile_stream_pass   = false;
    uint64_t              render_readback_ns = 0;
    enum wd_render_result render_result      = WD_RENDER_RESULT_IDLE;

    if (should_render)
    {
        if (server->output)
        {
            wlr_output_schedule_frame(server->output);
        }

        const uint64_t render_start_ns = wd_now_ns();
        render_result                  = wd_render_scene_and_readback_xrgb8888(server);
        render_readback_ns             = wd_now_ns() - render_start_ns;
        render_attempted               = true;

        if (render_result == WD_RENDER_RESULT_FRAME)
        {
            tile_stream_pass = wd_stream_frame_worker_submit(server);
            if (!tile_stream_pass)
            {
                /* Keep compositor damage live if the worker became busy
                 * between the idle check and publication. */
                server->scene_dirty = true;
            }
        }
        else if (render_result == WD_RENDER_RESULT_IDLE)
        {
            /* wlroots built no output buffer, so the manual damage was a
             * conservative false positive. Clear the complete damage state;
             * leaving only the bitmap populated can strand work because
             * render eligibility is driven by scene_dirty. */
            wd_server_clear_scene_damage(server);
        }
        else
        {
            /* Renderer/backend failures retain damage for a later retry. */
            server->scene_dirty = true;
        }
    }

    /* Dirty and repair queues consume the most recently captured framebuffer;
     * they do not require wlroots to render an unchanged scene. Service queued
     * work every compositor tick, including after a static-scene idle result. */
    if (!tile_stream_pass && stream_worker_idle)
    {
        wd_stream_frame_worker_request_service(server);
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
        delta_interval_ns            = wd_server_delta_summary_interval_locked(server, t);
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

        /* Keep adaptive health and mode transitions on the established
         * one-second cadence. Telemetry observes the controller result but no
         * longer owns correctness-critical state transitions. */
        wd_stream_controller_tick(server);
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

void wd_server_clear_scene_damage(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    server->scene_dirty       = false;
    server->damage_all_tiles  = false;
    server->damage_tile_count = 0;
    if (server->damage_tiles && server->total_base_tiles > 0)
    {
        memset(server->damage_tiles, 0, (size_t)server->total_base_tiles * sizeof(*server->damage_tiles));
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

struct wd_scene_damage_bounds {
    struct wd_server* server;
    int               offset_x;
    int               offset_y;
    bool              have_bounds;
    struct wlr_box    bounds;
};

static void scene_damage_buffer_iterator(struct wlr_scene_buffer* scene_buffer, int sx, int sy, void* data) {
    struct wd_scene_damage_bounds* state = data;
    if (!state || !scene_buffer)
    {
        return;
    }

    int width  = scene_buffer->dst_width;
    int height = scene_buffer->dst_height;

    struct wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (scene_surface && scene_surface->surface)
    {
        if (width <= 0)
        {
            width = scene_surface->surface->current.width;
        }
        if (height <= 0)
        {
            height = scene_surface->surface->current.height;
        }
    }

    if (scene_buffer->buffer)
    {
        if (width <= 0)
        {
            width = scene_buffer->buffer->width;
        }
        if (height <= 0)
        {
            height = scene_buffer->buffer->height;
        }
    }

    if (width <= 0 || height <= 0)
    {
        return;
    }

    sx += state->offset_x;
    sy += state->offset_y;

    if (!state->have_bounds)
    {
        state->bounds      = (struct wlr_box){.x = sx, .y = sy, .width = width, .height = height};
        state->have_bounds = true;
    }
    else
    {
        int x1     = sx < state->bounds.x ? sx : state->bounds.x;
        int y1     = sy < state->bounds.y ? sy : state->bounds.y;
        int x2     = sx + width;
        int y2     = sy + height;
        int old_x2 = state->bounds.x + state->bounds.width;
        int old_y2 = state->bounds.y + state->bounds.height;
        if (old_x2 > x2)
        {
            x2 = old_x2;
        }
        if (old_y2 > y2)
        {
            y2 = old_y2;
        }
        state->bounds = (struct wlr_box){.x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1};
    }

    if (state->server)
    {
        wd_server_mark_rect_dirty(state->server, sx, sy, width, height);
    }
}

static bool scene_node_damage(struct wd_server* server, struct wlr_scene_node* node, int offset_x, int offset_y, struct wlr_box* out_box) {
    if (!node)
    {
        return false;
    }

    struct wd_scene_damage_bounds state = {
        .server   = server,
        .offset_x = offset_x,
        .offset_y = offset_y,
    };
    wlr_scene_node_for_each_buffer(node, scene_damage_buffer_iterator, &state);

    if (state.have_bounds && out_box)
    {
        *out_box = state.bounds;
    }
    return state.have_bounds;
}

bool wd_server_scene_node_bounds(struct wlr_scene_node* node, struct wlr_box* out_box) {
    if (!out_box)
    {
        return false;
    }
    return scene_node_damage(NULL, node, 0, 0, out_box);
}

void wd_server_mark_scene_node_dirty(struct wd_server* server, struct wlr_scene_node* node) {
    if (!server || !node)
    {
        return;
    }
    scene_node_damage(server, node, 0, 0, NULL);
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
    if (view->scene_tree)
    {
        wd_server_mark_scene_node_dirty(view->server, &view->scene_tree->node);
    }
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

    if (view->scene_tree)
    {
        /* The node has already moved. Damage its current buffers and the same
         * buffer rectangles translated back to the previous view origin. */
        scene_node_damage(view->server, &view->scene_tree->node, 0, 0, NULL);
        scene_node_damage(view->server, &view->scene_tree->node, old_x - view->x, old_y - view->y, NULL);
    }
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

bool wd_server_request_display_mode(struct wd_server* server, uint32_t width, uint32_t height, uint16_t refresh_hz) {
    if (!server || width == 0 || height == 0 || width > WD_MAX_RENDER_WIDTH || height > WD_MAX_RENDER_HEIGHT ||
        refresh_hz < WD_SERVER_MIN_REFRESH_HZ || refresh_hz > WD_SERVER_MAX_REFRESH_HZ)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    uint64_t serial                    = ++net->display_resize_request_serial;
    net->display_resize_width          = width;
    net->display_resize_height         = height;
    net->display_resize_refresh_hz     = refresh_hz;
    net->display_resize_pending        = true;

    while (wd_net_run_state_is_running(&net->run_state) && net->display_resize_completed_serial < serial)
    {
        pthread_cond_wait(&net->display_resize_cond, &net->lock);
    }

    bool ok = wd_net_run_state_is_running(&net->run_state) && net->display_resize_completed_serial >= serial && net->display_resize_result;

    pthread_mutex_unlock(&net->lock);

    return ok;
}

bool wd_server_request_display_size(struct wd_server* server, uint32_t width, uint32_t height) {
    if (!server)
    {
        return false;
    }
    uint16_t refresh_hz = (uint16_t)((server->output_refresh_mhz + 500u) / 1000u);
    if (refresh_hz < WD_SERVER_MIN_REFRESH_HZ)
    {
        refresh_hz = WD_SERVER_IDLE_REFRESH_HZ;
    }
    return wd_server_request_display_mode(server, width, height, refresh_hz);
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
    uint32_t*             framebuffer_xrgb8888;
    uint32_t*             framebuffer_shadow_xrgb8888;
    struct wd_tile_state* tiles;
    bool*                 damage_tiles;
    uint16_t*             dirty_regions;
    bool*                 dirty_region_queued;
    uint64_t*             dirty_region_enqueued_ns;
    uint64_t*             dirty_epochs;
    uint16_t*             dirty_queue;
    bool*                 dirty_queued;
    uint64_t*             dirty_queue_enqueued_ns;
    uint16_t*             retransmit_queue;
    bool*                 retransmit_queued;
    uint64_t*             retransmit_queue_enqueued_ns;
    uint64_t*             retransmit_requested_generation;
    bool*                 summary_dirty_tiles;
    uint16_t*             summary_dirty_queue;
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

    allocs->framebuffer_xrgb8888            = calloc(server->framebuffer_pixels, sizeof(*allocs->framebuffer_xrgb8888));
    allocs->framebuffer_shadow_xrgb8888     = calloc(server->framebuffer_pixels, sizeof(*allocs->framebuffer_shadow_xrgb8888));
    allocs->tiles                           = calloc(server->total_tiles, sizeof(*allocs->tiles));
    allocs->damage_tiles                    = calloc(server->total_base_tiles, sizeof(*allocs->damage_tiles));
    allocs->dirty_regions                   = calloc(server->total_tiles, sizeof(*allocs->dirty_regions));
    allocs->dirty_region_queued             = calloc(server->total_tiles, sizeof(*allocs->dirty_region_queued));
    allocs->dirty_region_enqueued_ns        = calloc(server->total_tiles, sizeof(*allocs->dirty_region_enqueued_ns));
    allocs->dirty_epochs                    = calloc(server->total_tiles, sizeof(*allocs->dirty_epochs));
    allocs->dirty_queue                     = calloc(server->total_tiles, sizeof(*allocs->dirty_queue));
    allocs->dirty_queued                    = calloc(server->total_tiles, sizeof(*allocs->dirty_queued));
    allocs->dirty_queue_enqueued_ns         = calloc(server->total_tiles, sizeof(*allocs->dirty_queue_enqueued_ns));
    allocs->retransmit_queue                = calloc(server->total_tiles, sizeof(*allocs->retransmit_queue));
    allocs->retransmit_queued               = calloc(server->total_tiles, sizeof(*allocs->retransmit_queued));
    allocs->retransmit_queue_enqueued_ns    = calloc(server->total_tiles, sizeof(*allocs->retransmit_queue_enqueued_ns));
    allocs->retransmit_requested_generation = calloc(server->total_tiles, sizeof(*allocs->retransmit_requested_generation));
    allocs->summary_dirty_tiles             = calloc(server->total_tiles, sizeof(*allocs->summary_dirty_tiles));
    allocs->summary_dirty_queue             = calloc(server->total_tiles, sizeof(*allocs->summary_dirty_queue));

    if (!allocs->framebuffer_xrgb8888 || !allocs->framebuffer_shadow_xrgb8888 || !allocs->tiles || !allocs->damage_tiles ||
        !allocs->dirty_regions || !allocs->dirty_region_queued || !allocs->dirty_region_enqueued_ns || !allocs->dirty_epochs ||
        !allocs->dirty_queue || !allocs->dirty_queued || !allocs->dirty_queue_enqueued_ns || !allocs->retransmit_queue ||
        !allocs->retransmit_queued || !allocs->retransmit_queue_enqueued_ns || !allocs->retransmit_requested_generation ||
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

    uint32_t* old_framebuffer        = server->framebuffer_xrgb8888;
    uint32_t* old_framebuffer_shadow = server->framebuffer_shadow_xrgb8888;
    wd_server_free_resize_stream_state(server);

    server->framebuffer_xrgb8888                = next_allocs.framebuffer_xrgb8888;
    server->framebuffer_shadow_xrgb8888         = next_allocs.framebuffer_shadow_xrgb8888;
    server->framebuffer_shadow_valid            = false;
    server->net.tiles                           = next_allocs.tiles;
    server->damage_tiles                        = next_allocs.damage_tiles;
    server->net.dirty_regions                   = next_allocs.dirty_regions;
    server->net.dirty_region_queued             = next_allocs.dirty_region_queued;
    server->net.dirty_region_enqueued_ns        = next_allocs.dirty_region_enqueued_ns;
    server->net.dirty_epochs                    = next_allocs.dirty_epochs;
    server->net.dirty_queue                     = next_allocs.dirty_queue;
    server->net.dirty_queued                    = next_allocs.dirty_queued;
    server->net.dirty_queue_enqueued_ns         = next_allocs.dirty_queue_enqueued_ns;
    server->net.retransmit_queue                = next_allocs.retransmit_queue;
    server->net.retransmit_queued               = next_allocs.retransmit_queued;
    server->net.retransmit_queue_enqueued_ns    = next_allocs.retransmit_queue_enqueued_ns;
    server->net.retransmit_requested_generation = next_allocs.retransmit_requested_generation;
    server->net.summary_dirty_tiles             = next_allocs.summary_dirty_tiles;
    server->net.summary_dirty_queue             = next_allocs.summary_dirty_queue;
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
    wd_server_mark_scene_dirty(server);
    server->framebuffer_shadow_valid = false;

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

bool wd_server_apply_display_mode(struct wd_server* server, uint32_t width, uint32_t height, uint16_t refresh_hz) {
    if (!server || width == 0 || height == 0 || width > WD_MAX_RENDER_WIDTH || height > WD_MAX_RENDER_HEIGHT ||
        refresh_hz < WD_SERVER_MIN_REFRESH_HZ || refresh_hz > WD_SERVER_MAX_REFRESH_HZ)
    {
        return false;
    }

    const uint32_t old_refresh_mhz  = server->output_refresh_mhz;
    const uint32_t next_refresh_mhz = (uint32_t)refresh_hz * 1000u;
    const bool     geometry_changed = server->display_width != width || server->display_height != height;
    const bool     refresh_changed  = old_refresh_mhz != next_refresh_mhz;
    if (!geometry_changed && !refresh_changed)
    {
        return true;
    }

    server->output_refresh_mhz = next_refresh_mhz;
    if (geometry_changed)
    {
        if (wd_server_apply_display_size(server, width, height))
        {
            return true;
        }
        server->output_refresh_mhz = old_refresh_mhz;
        (void)wd_wlroots_resize_headless_output(server);
        return false;
    }

    if (!wd_wlroots_resize_headless_output(server))
    {
        server->output_refresh_mhz = old_refresh_mhz;
        (void)wd_wlroots_resize_headless_output(server);
        return false;
    }

    pthread_mutex_lock(&server->net.lock);
    wd_frame_pacing_reset(&server->net.stream_policy.frame_pacing);
    pthread_mutex_unlock(&server->net.lock);
    return true;
}

static bool wd_server_init(struct wd_server* server, const struct wd_server_config* config) {
    memset(server, 0, sizeof(*server));
    wd_spawned_process_init(&server->startup_process);
    server->input_wakeup_fd = -1;

    wl_list_init(&server->views);
    wl_list_init(&server->popup_commit_trackers);
    wl_list_init(&server->keyboard_shortcuts_inhibitors);
    wl_list_init(&server->new_keyboard_shortcuts_inhibitor.link);
    wl_list_init(&server->keyboard_shortcuts_inhibit_manager_destroy.link);
    wl_list_init(&server->pointer_button_grab_surface_destroy.link);

    if (!wd_server_set_tile_size(server, config->tile_width, config->tile_height) ||
        !wd_server_set_geometry(server, config->display_width, config->display_height))
    {
        return false;
    }

    server->scene_dirty = true;

    server->startup_command       = config->app_command;
    server->video_encoder_backend = config->video_encoder_backend;
    server->output_scale          = config->output_scale;
    server->output_refresh_mhz =
        (uint32_t)WD_SERVER_IDLE_REFRESH_HZ * 1000u;
#if WAYDISPLAY_ENABLE_XWAYLAND
    server->enable_xwayland = config->enable_xwayland;
#else
    (void)config->enable_xwayland;
#endif
    server->enable_xdg_dialog = config->enable_xdg_dialog;

    if (server->output_scale <= 0.0)
    {
        server->output_scale = WD_SERVER_DEFAULT_OUTPUT_SCALE;
    }

    server->framebuffer_xrgb8888        = calloc(server->framebuffer_pixels, sizeof(uint32_t));
    server->framebuffer_shadow_xrgb8888 = calloc(server->framebuffer_pixels, sizeof(uint32_t));
    if (server->framebuffer_xrgb8888 && server->framebuffer_shadow_xrgb8888)
    {
        server->framebuffer_generation    = 1;
        server->framebuffer_shadow_valid  = false;
    }

    if (!server->framebuffer_xrgb8888 || !server->framebuffer_shadow_xrgb8888)
    {
        free(server->framebuffer_xrgb8888);
        server->framebuffer_xrgb8888 = NULL;
        free(server->framebuffer_shadow_xrgb8888);
        server->framebuffer_shadow_xrgb8888 = NULL;
        return false;
    }

    if (!wd_net_init(server, config->tcp_port, config->listen_address))
    {
        return false;
    }

    server->tile_compression_benchmark_mode = config->tile_compression_benchmark_mode;
    WD_LOG_DEBUG("tile compression policy: mode=%s",
                 wd_tile_compression_benchmark_mode_name(server->tile_compression_benchmark_mode));

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

    server->input_wakeup_source =
        wl_event_loop_add_fd(server->event_loop, server->input_wakeup_fd, WL_EVENT_READABLE, server_input_wakeup, server);
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

static void wd_server_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    server_terminate_startup_process(server);

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

#if WAYDISPLAY_ENABLE_XWAYLAND
    /* Stop the embedded X server while every compositor subsystem its surface
     * destruction callbacks can touch is still alive. */
    wd_xwayland_destroy(server);
#endif

    /* Destroy client resources before tearing down protocol managers, scene
     * ownership, clipboard state, or the network/stream locks used by view and
     * popup destruction callbacks. Destroying these in the opposite order can
     * turn normal wl_resource teardown into use-after-free during shutdown. */
    if (server->display)
    {
        wl_display_destroy_clients(server->display);
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
    server->framebuffer_shadow_valid    = false;
}

static void server_request_network_stop(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    pthread_mutex_lock(&server->net.lock);
    wd_net_run_state_set(&server->net.run_state, false);
    pthread_cond_broadcast(&server->net.display_resize_cond);
    pthread_cond_broadcast(&server->net.startup_cond);
    pthread_mutex_unlock(&server->net.lock);

    if (server->net.listen_fd >= 0)
    {
        shutdown(server->net.listen_fd, SHUT_RDWR);
    }
    if (server->net.tcp_fd >= 0)
    {
        shutdown(server->net.tcp_fd, SHUT_RDWR);
    }
}

struct wd_server* wd_server_create(const struct wd_server_config* config) {
    if (!config)
    {
        return NULL;
    }

    struct wd_server* server = calloc(1, sizeof(*server));
    if (!server)
    {
        return NULL;
    }

    if (!wd_server_init(server, config))
    {
        wd_server_destroy(server);
        free(server);
        return NULL;
    }

    return server;
}

void wd_server_free(struct wd_server* server) {
    if (!server)
    {
        return;
    }
    wd_server_destroy(server);
    free(server);
}

int wd_server_run(struct wd_server* server) {
#if WAYDISPLAY_LOG_LEVEL >= WD_LOG_LEVEL_VALUE_DEBUG
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

    if (!wd_net_wait_until_ready(server))
    {
        pthread_join(net_thread, NULL);
        return 1;
    }

    if (!launch_startup_command(server))
    {
        server_request_network_stop(server);
        pthread_join(net_thread, NULL);
        return 1;
    }

    wl_display_run(server->display);

    server_request_network_stop(server);
    pthread_join(net_thread, NULL);
    server_terminate_startup_process(server);

    return 0;
}
