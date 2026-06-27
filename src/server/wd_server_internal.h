#pragma once

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <xkbcommon/xkbcommon.h>
#if WAYDISPLAY_ENABLE_XWAYLAND
#include <wlr/xwayland/xwayland.h>
#endif
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_protocol.h"
#include "wd_net_listener.h"
#include "wd_net_run_state.h"
#include "wd_process.h"
#include "wd_selection_delivery.h"
#include "wd_server.h"
#include "wd_frame_pacing.h"
#include "wd_server_compositor.h"
#include "wd_server_input.h"
#include "wd_server_net.h"
#include "wd_server_stream.h"
#include "wd_tile_policy.h"
#include "wd_bandwidth_plan.h"

enum wd_compositor_request {
    WD_COMPOSITOR_REQUEST_FULL_REFRESH = 1u << 0,
};

struct wd_async_tcp_sender;
struct wd_async_udp_sender;
struct wd_dirty_region_scheduler;
struct wd_audio_stream;

#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_toplevel_icon_v1.h>
#include <wlr/util/log.h>

/*
 * wlroots exposes these as WLR_EDGE_* in some versions, but the resize edge
 * values are the xdg_toplevel_resize_edge bit layout:
 *
 *   none=0, top=1, bottom=2, left=4, right=8
 *
 * Define local compatibility macros when wlroots does not provide them.
 */
#ifndef WLR_EDGE_NONE
#define WLR_EDGE_NONE   0u
#define WLR_EDGE_TOP    1u
#define WLR_EDGE_BOTTOM 2u
#define WLR_EDGE_LEFT   4u
#define WLR_EDGE_RIGHT  8u
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct wd_server;
struct wd_selection_capture;
struct wd_video_encoder;
struct wd_stream_frame_worker;

struct wd_view {
    struct wd_server* server;

    struct wl_list link;

    struct wlr_xdg_surface* xdg_surface;
#if WAYDISPLAY_ENABLE_XWAYLAND
    struct wlr_xwayland_surface* xwayland_surface;
#endif
    struct wlr_scene_tree* scene_tree;
    /* XDG content subtree. scene_tree is the compositor-owned placement wrapper. */
    struct wlr_scene_tree* xdg_surface_tree;
#if WAYDISPLAY_ENABLE_XWAYLAND
    struct wlr_scene_tree* xwayland_surface_tree;
    struct wlr_scene_rect* xwayland_titlebar_rect;
    struct wlr_scene_rect* xwayland_close_rect;
    struct wlr_scene_rect* xwayland_maximize_rect;
    struct wlr_scene_rect* xwayland_minimize_rect;
#endif
    struct wlr_xdg_toplevel_icon_v1* toplevel_icon;

    char*               app_id;
    char*               title;
    struct wl_resource* xdg_dialog_resource;
    struct wd_view*     parent;
    bool                positioned;
#if WAYDISPLAY_ENABLE_XWAYLAND
    bool xwayland_had_map_request;
#endif

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener set_parent;
    struct wl_listener set_app_id;
    struct wl_listener set_title;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener request_minimize;
    struct wl_listener xdg_surface_destroy;
    struct wl_listener xdg_toplevel_destroy;
    struct wl_listener new_popup;
#if WAYDISPLAY_ENABLE_XWAYLAND
    struct wl_listener xwayland_destroy;
    struct wl_listener xwayland_associate;
    struct wl_listener xwayland_dissociate;
    struct wl_listener xwayland_map;
    struct wl_listener xwayland_unmap;
    struct wl_listener xwayland_commit;
    struct wl_listener xwayland_map_request;
    struct wl_listener xwayland_request_configure;
    struct wl_listener xwayland_request_maximize;
    struct wl_listener xwayland_request_fullscreen;
    struct wl_listener xwayland_request_minimize;
    struct wl_listener xwayland_request_close;
#endif

    struct wl_event_source* configure_idle;

    /*
     * Commit listeners for the whole xdg surface tree.  Some clients,
     * especially Firefox, render important content through child/subsurfaces
     * whose commits do not fire the toplevel root surface commit listener.
     */
    struct wl_list surface_commit_trackers;

    int x;
    int y;

    bool mapped;
    bool configured_once;

    bool     activated;
    bool     minimized;
    bool     is_dialog;
    bool     dialog_modal;
    bool     maximized;
    bool     fullscreen;
    uint32_t tiled_edges;

    uint32_t bounds_width;
    uint32_t bounds_height;

    int      saved_x;
    int      saved_y;
    uint32_t saved_width;
    uint32_t saved_height;
};

struct wd_move_grab {
    bool            active;
    struct wd_view* view;

    double grab_x;
    double grab_y;

    int view_x;
    int view_y;
};

struct wd_resize_grab {
    bool            active;
    struct wd_view* view;
    uint32_t        edges;

    double grab_x;
    double grab_y;

    int      view_x;
    int      view_y;
    uint32_t view_width;
    uint32_t view_height;
};

struct wd_tile_state {
    uint64_t generation;
    uint64_t timestamp_ns;
    uint64_t input_sequence;
};

struct wd_udp_tile_send_result {
    bool     any_packet_sent;
    bool     all_packets_sent;
    bool     send_blocked;
    uint32_t packets_sent;
    uint32_t bytes_sent;
};

struct wd_stats {
    uint64_t dirty_tiles;
    uint64_t udp_tiles_sent;
    uint64_t udp_fresh_tiles_sent;
    uint64_t udp_fresh_bytes_sent;
    uint64_t udp_retx_tiles_sent;
    uint64_t udp_retx_bytes_sent;
    uint64_t udp_compressed_tiles_sent;
    uint64_t udp_uncompressed_tiles_sent;
    uint64_t udp_compressed_tile_bytes_sent;
    uint64_t udp_uncompressed_tile_bytes_sent;
    uint64_t udp_packets_sent;
    uint64_t udp_bytes_sent;
    uint64_t udp_send_pressure_drops;

    uint64_t tile_choice_compressed;
    uint64_t tile_choice_uncompressed;
    uint64_t tile_choice_compressed_payload_sum;
    uint64_t tile_choice_uncompressed_payload_sum;
    uint64_t tile_choice_compressed_wire_sum;
    uint64_t tile_choice_uncompressed_wire_sum;
    uint64_t tile_choice_chosen_wire_sum;
    uint64_t tile_choice_covered_base_tiles;
    uint64_t tile_choice_saved_wire_sum;
    uint64_t compression_attempts;
    uint64_t compression_wins;
    uint64_t compression_entropy_skips;
    uint64_t compression_adaptive_skips;
    uint64_t compression_nonwins;
    uint64_t compression_forced_choices;
    uint64_t compression_ns;
    uint64_t compression_saved_wire_bytes;

    uint64_t stream_mode_frame_samples;
    /* Scene frames whose framebuffer diff contained at least one changed
     * base tile. This is the actual tile-render demand; unchanged commits and
     * idle queue-service ticks must not count as client render pressure. */
    uint64_t stream_mode_changed_frame_samples;
    uint64_t stream_mode_dirty_coverage_per_mille_sum;
    uint64_t stream_mode_dirty_coverage_per_mille_peak;
    uint64_t stream_mode_pending_coverage_per_mille_sum;
    uint64_t stream_mode_pending_coverage_per_mille_peak;
    uint64_t stream_mode_budget_pressure_frames;
    uint64_t stream_mode_full_refresh_samples;
    uint64_t stream_mode_full_refresh_budget_pressure_frames;
    uint64_t stream_mode_bootstrap_suppressed_samples;

    uint64_t tile_size_128x64_sent;
    uint64_t tile_size_64x64_sent;
    uint64_t tile_size_32x32_sent;
    uint64_t tile_size_16x16_sent;

    uint64_t tcp_hello_rx;
    uint64_t tcp_config_tx;
    uint64_t tcp_config_applied_ack_rx;
    uint64_t tcp_config_apply_ack_samples;
    uint64_t tcp_config_apply_ack_sum_ns;
    uint64_t tcp_config_apply_ack_max_ns;
    uint64_t tcp_summary_tx;
    uint64_t tcp_input_channel_rx;
    uint64_t tcp_input_channel_accepted;
    uint64_t tcp_input_channel_closed;
    uint64_t tcp_selection_channel_rx;
    uint64_t tcp_selection_channel_accepted;
    uint64_t tcp_selection_channel_closed;
    uint64_t tcp_video_channel_rx;
    uint64_t tcp_video_channel_accepted;
    uint64_t tcp_video_channel_closed;

    uint64_t video_frames_published;
    uint64_t video_frames_superseded;
    uint64_t video_worker_stale_drops;
    uint64_t video_tile_detection_skipped;
    uint64_t video_publish_copy_samples;
    uint64_t video_publish_copy_ns;
    uint64_t video_worker_queue_samples;
    uint64_t video_worker_queue_ns;
    uint64_t video_frame_attempts;
    uint64_t video_frames_tx;
    uint64_t video_keyframe_attempts;
    uint64_t video_keyframes_tx;
    uint64_t video_tcp_bytes_tx;
    uint64_t video_encode_ns;
    uint64_t video_encode_failed;
    uint64_t video_tcp_send_failed;
    uint64_t video_keyframe_skipped_pending;
    uint64_t video_control_frames_tx;
    uint64_t video_end_of_stream_tx;
    uint64_t video_resize_resets;
    uint64_t video_resets;
    uint64_t audio_captured_frames;
    uint64_t audio_capture_overruns;
    uint64_t audio_encoded_packets;
    uint64_t audio_encoded_bytes;
    uint64_t audio_queue_drops;
    uint64_t audio_discontinuities;
    uint64_t audio_encode_failures;

    uint64_t server_frame_timer_samples;
    uint64_t server_frame_timer_sum_ns;
    uint64_t server_frame_timer_max_ns;
    uint64_t server_render_readback_samples;
    uint64_t server_render_readback_sum_ns;
    uint64_t server_render_readback_max_ns;
    uint64_t server_scene_damage_promotions;
    uint64_t server_render_idle_results;
    uint64_t server_render_failed_results;

    uint64_t client_stats_rx;
    uint64_t client_udp_packets_rx;
    uint64_t client_udp_bytes_rx;
    uint64_t client_tiles_completed;
    uint64_t client_completed_packets;
    uint64_t client_partial_tiles_timed_out;
    uint64_t client_old_generation_tiles;
    uint64_t client_retx_requests_tx;
    uint64_t client_udp_interarrival_samples;
    uint64_t client_udp_interarrival_sum_ns;
    uint64_t client_udp_interarrival_jitter_samples;
    uint64_t client_udp_interarrival_jitter_sum_ns;
    uint64_t client_udp_interarrival_max_ns;
    uint64_t client_render_visible_reports;
    uint64_t client_render_hidden_reports;
    uint64_t client_render_frames;
    uint64_t client_present_samples;
    uint64_t client_present_sum_ns;
    uint64_t client_present_max_ns;
    uint64_t client_input_present_samples;
    uint64_t client_input_present_sum_ns;
    uint64_t client_video_frames_rx;
    uint64_t client_video_bytes_rx;
    uint64_t client_video_frames_decoded;
    uint64_t client_video_frames_presented;
    uint64_t client_video_decode_failed;
    uint64_t client_video_publish_failed;
    uint64_t client_video_control_frames_rx;
    uint64_t client_video_need_keyframe_drops;
    uint64_t client_video_decoder_resets;
    uint64_t client_video_decode_samples;
    uint64_t client_video_decode_sum_ns;
    uint64_t client_video_messages_rx;
    uint64_t client_video_data_frames_rx;
    uint64_t client_video_invalid_frames_rx;
    uint64_t client_video_stale_frames_dropped;
    uint64_t client_video_last_frame_id_rx;
    uint64_t client_video_last_frame_id_presented;
    uint64_t client_video_present_latency_samples;
    uint64_t client_video_present_latency_sum_ns;
    uint64_t client_audio_messages_rx;
    uint64_t client_audio_packets_rx;
    uint64_t client_audio_bytes_rx;
    uint64_t client_audio_decode_failed;
    uint64_t client_audio_discontinuities;
    uint64_t client_audio_late_drops;
    uint64_t client_audio_underflows;
    uint64_t client_audio_video_sync_holds;
    uint64_t client_audio_video_sync_drops;
    uint64_t client_video_decode_queue_drops;
    uint64_t client_audio_video_startup_timeouts;
    uint32_t client_audio_video_startup_hold_ms;
    uint8_t  client_audio_playback_state;
    uint64_t client_video_queue_overflow_drops;
    uint32_t client_video_queue_depth;
    uint32_t client_video_queue_depth_max;
    uint64_t client_video_oldest_pts_usec;
    int64_t  client_audio_video_delta_samples;
    uint64_t client_tile_frames_presented;
    uint64_t client_tile_content_epoch_presented;
    uint64_t client_video_content_epoch_presented;

    uint64_t retx_req_rx;
    uint64_t retx_tiles_req;
    uint64_t retx_req_ignored_live;

    uint64_t key_events_rx;
    uint64_t key_events_injected;
    uint64_t key_events_dropped;
    uint64_t key_state_duplicate_presses;
    uint64_t key_state_release_without_press;
    uint64_t keyboard_enter_events;

    uint64_t pointer_events_rx;
    uint64_t pointer_events_injected;
    uint64_t pointer_events_dropped;
    uint64_t pointer_button_grab_started;
    uint64_t pointer_button_grab_ended;
    uint64_t pointer_button_grab_cleared;
    uint64_t pointer_button_grab_surface_destroyed;

    uint64_t xdg_move_invalid_serial;
    uint64_t xdg_resize_invalid_serial;

    uint64_t popup_explicit_scene_trees;
    uint64_t popup_explicit_scene_tree_failures;

    uint64_t cursor_shape_requests;
    uint64_t cursor_shape_tx;
    uint64_t cursor_shape_coalesced;
    uint64_t cursor_set_cursor_requests;
    uint64_t cursor_set_cursor_rejected;
    uint64_t cursor_set_cursor_hidden;
    uint64_t cursor_set_cursor_fallback;

    uint64_t input_net_latency_samples;
    uint64_t input_net_latency_sum_ns;
    uint64_t input_queue_latency_samples;
    uint64_t input_queue_latency_sum_ns;
    uint64_t input_wakeup_signals;
    uint64_t input_wakeup_callbacks;
    uint64_t input_wakeup_events;
    uint64_t input_wakeup_coalesced;
    uint64_t input_wakeup_failures;
    uint64_t input_to_summary_samples;
    uint64_t input_to_summary_sum_ns;
    uint64_t input_to_first_fresh_tile_samples;
    uint64_t input_to_first_fresh_tile_sum_ns;
    uint64_t input_correlation_delivery_failed;

    uint64_t tcp_summary_full_tx;
    uint64_t tcp_summary_delta_tx;
    uint64_t tcp_summary_delta_tiles;
    uint64_t tcp_summary_coalesced;
    uint64_t tcp_summary_budget_interval_ns;
    uint64_t tcp_summary_repair_backoff;
    uint64_t tcp_control_bytes_sent;
    uint64_t tcp_control_bytes_refunded;
    uint64_t tcp_budget_blocked;
    uint64_t tcp_async_send_failed;
    uint64_t tcp_async_queue_overflow;
    uint64_t tcp_async_queued;
    uint64_t tcp_async_completed;
    uint64_t tcp_async_completion_failed;
    uint64_t tcp_async_partial_resubmits;
    uint64_t tcp_async_inflight_max;
    uint64_t udp_async_send_failed;
    uint64_t udp_async_queued;
    uint64_t udp_async_completed;
    uint64_t udp_async_completion_failed;
    uint64_t udp_async_sqe_exhausted;
    uint64_t udp_async_inflight_max;
    uint64_t udp_async_submit_calls;
    uint64_t udp_async_partial_submits;

    uint64_t rate_decreases;
    uint64_t rate_increases;
    uint64_t frame_rate_downshifts;
    uint64_t frame_rate_upshifts;
    uint64_t dirty_region_probes;
    uint64_t dirty_region_hits;
    uint64_t dirty_budget_blocked;
    uint64_t dirty_budget_blocked_full_refresh;
    uint64_t partial_tile_sends;
    uint64_t partial_tile_packets_sent;
    uint64_t dirty_detect_ns;
    uint64_t framebuffer_diff_ns;
    uint64_t framebuffer_diff_candidates;
    uint64_t framebuffer_diff_changed;
    uint64_t framebuffer_diff_unchanged;
    uint64_t framebuffer_diff_full_refreshes;
    uint64_t dirty_region_select_ns;
    uint64_t tile_encode_ns;
    uint64_t summary_build_ns;
    uint64_t udp_send_ns;
    uint64_t encode_jobs_submitted;
    uint64_t encode_jobs_completed;
    uint64_t encode_jobs_stale;
    uint64_t encode_worker_ns;
    uint64_t encode_wait_ns;
    uint64_t encode_batches;
    uint64_t encode_batch_jobs_peak;
    uint64_t encode_worker_threads;
    uint64_t encode_thread_wakeups;

    uint64_t dirty_tiles_stale_skipped;
    uint64_t retx_tiles_superseded_by_fresh;
    uint64_t dirty_queue_age_samples;
    uint64_t dirty_queue_age_sum_ns;
    uint64_t retx_queue_age_samples;
    uint64_t retx_queue_age_sum_ns;
    uint64_t retx_req_stale_generation;
    uint64_t retx_req_upgraded_generation;
};

enum wd_stream_mode {
    WD_STREAM_MODE_TILES           = 0,
    WD_STREAM_MODE_VIDEO_CANDIDATE = 1,
    WD_STREAM_MODE_VIDEO_READY     = 2,
    WD_STREAM_MODE_VIDEO_ACTIVE    = 3,
    WD_STREAM_MODE_TILE_RECOVERY   = 4,
};

struct wd_stats_log_state {
    struct wd_stats     totals;
    bool                have_prev_state;
    uint16_t            prev_requested_capture_fps;
    uint16_t            prev_adaptive_capture_fps;
    uint16_t            prev_capture_pacing_fps;
    uint16_t            prev_compositor_refresh_hz;
    uint16_t            prev_client_present_cap_fps;
    bool                prev_client_render_visible;
    uint64_t            prev_safe_link_kib_per_second;
    uint64_t            prev_recent_link_kib_per_second;
    uint64_t            prev_tile_media_rate_kib_per_second;
    uint64_t            prev_tile_fresh_kib_per_second;
    uint64_t            prev_tile_repair_kib_per_second;
    uint64_t            prev_video_kib_per_second;
    uint64_t            prev_control_kib_per_second;
    uint64_t            prev_audio_reserved_kib_per_second;
    uint64_t            prev_audio_cap_kib_per_second;
    uint64_t            prev_overhead_kib_per_second;
    uint16_t            prev_tile_width;
    uint16_t            prev_tile_height;
    bool                prev_input_channel;
    bool                prev_selection_channel;
    bool                prev_video_channel;
    bool                prev_video_negotiated;
    bool                prev_video_encoder;
    uint8_t             prev_video_mode;
    uint8_t             prev_video_min_dirty_percent;
    uint16_t            prev_video_enter_seconds;
    uint8_t             prev_video_exit_dirty_percent;
    uint16_t            prev_video_exit_seconds;
    uint32_t            prev_video_bitrate_kib;
    enum wd_stream_mode prev_stream_mode;
};

struct wd_stream_policy {
    uint16_t requested_capture_fps;
    uint16_t adaptive_capture_fps;

    enum wd_stream_mode stream_mode;
    uint8_t             video_mode;
    uint8_t             video_min_dirty_percent;
    uint16_t            video_enter_seconds;
    uint8_t             video_exit_dirty_percent;
    uint16_t            video_exit_seconds;
    uint32_t            video_bitrate_kib_per_second;
    uint32_t            video_candidate_seconds;
    uint32_t            tile_recovery_seconds;
    uint32_t            video_client_failure_seconds;
    bool                tile_refresh_pending;
    bool                tile_recovery_refresh_started;
    bool                tile_recovery_refresh_sent;
    uint32_t            tile_recovery_wait_seconds;
    uint32_t            video_retry_cooldown_seconds;
    bool                video_bootstrap_pending;
    bool                video_bootstrap_refresh_started;
    bool                video_bootstrap_refresh_sent;
    uint32_t            video_bootstrap_wait_seconds;
    uint64_t            video_bootstrap_content_epoch;
    uint64_t            tile_recovery_content_epoch;
    uint8_t             video_recovery_class;

    uint32_t frame_rate_good_seconds;

    struct wd_frame_pacing_state frame_pacing;
    uint64_t                     last_video_frame_send_ns;

    uint64_t safe_link_bytes_per_second;
    uint64_t recent_link_bytes_per_second;
    uint64_t tile_fresh_bytes_per_second;
    uint64_t adaptive_tile_fresh_bytes_per_second;
    uint64_t tile_repair_bytes_per_second;
    uint64_t video_bytes_per_second;
    uint64_t control_bytes_per_second;
    uint64_t audio_cap_bytes_per_second;
    uint64_t audio_reserved_bytes_per_second;
    uint64_t overhead_bytes_per_second;
    bool     bandwidth_audio_enabled;
    uint32_t bandwidth_audio_bitrate;

    /* Current tile-media rate. Kept separate from the stable link estimate so
     * tile adaptation cannot lower the next video encoder target. */
    uint64_t tile_media_bytes_per_second;
    uint64_t tile_media_floor_bytes_per_second;
    uint64_t tile_media_ceiling_bytes_per_second;
    uint32_t link_good_seconds;
    uint32_t link_loss_seconds;
    uint32_t multipacket_loss_cooldown_seconds;
    uint32_t client_render_pressure_seconds;
    bool     client_render_visible;
    struct wd_bandwidth_bucket fresh_tile_bucket;
    struct wd_bandwidth_bucket repair_bucket;
    struct wd_bandwidth_bucket control_bucket;
};

struct wd_queued_key_event {
    uint16_t evdev_key_code;
    bool     pressed;
    uint64_t client_timestamp_ns;
    uint64_t input_sequence;
    uint64_t server_rx_timestamp_ns;
};

struct wd_queued_pointer_event {
    struct wd_pointer_event_payload event;
    uint64_t                        server_rx_timestamp_ns;
};

enum wd_net_startup_state {
    WD_NET_STARTUP_PENDING = 0,
    WD_NET_STARTUP_READY,
    WD_NET_STARTUP_FAILED,
};

struct wd_net_state {
    /*
     * Primary server runtime lock. The network thread owns transport mutation;
     * compositor callbacks may take this lock only for short state transfers.
     * Never call blocking socket, encoder, PipeWire, or wlroots operations while
     * holding it. video_encoder_lock and worker-private locks must not be taken
     * before this lock. See docs/threading.md.
     */
    pthread_mutex_t lock;
    pthread_cond_t  display_resize_cond;
    pthread_cond_t  startup_cond;
    pthread_cond_t  encoder_idle_cond;

    enum wd_net_startup_state  startup_state;
    enum wd_net_listener_stage startup_failed_stage;
    int                        startup_error;

    bool     display_resize_pending;
    uint64_t display_resize_request_serial;
    uint64_t display_resize_completed_serial;
    uint32_t display_resize_width;
    uint32_t display_resize_height;
    uint16_t display_resize_refresh_hz;
    bool     display_resize_result;

    struct wd_net_run_state run_state;
    bool                    client_connected;
    uint16_t                udp_payload_target;

    uint64_t link_rtt_ns;
    uint64_t link_jitter_ns;
    uint64_t summary_retransmit_grace_ns;
    uint64_t retransmit_request_interval_ns;
    uint64_t retransmit_inflight_grace_ns;
    uint64_t tile_reassembly_timeout_ns;
    uint64_t active_summary_interval_ns;
    uint64_t clean_summary_interval_ns;

    uint16_t                          dirty_region_cursor;
    struct wd_dirty_region_scheduler* dirty_region_scheduler;
    uint16_t*                         dirty_regions;
    bool*                             dirty_region_queued;
    uint64_t*                         dirty_region_enqueued_ns;
    uint16_t                          dirty_region_count;

    uint64_t* dirty_epochs;

    bool                     encoder_batch_active;
    void*                    encoder_pool;
    void*                    encode_workspace;
    void*                    video_worker;
    uint64_t                 video_worker_epoch;
    struct wd_video_encoder* video_encoder;
    pthread_mutex_t          video_encoder_lock;

    uint16_t* dirty_queue;
    bool*     dirty_queued;
    uint64_t* dirty_queue_enqueued_ns;
    uint16_t  dirty_queue_read;
    uint16_t  dirty_queue_write;
    uint16_t  dirty_queue_count;

    uint16_t* retransmit_queue;
    bool*     retransmit_queued;
    uint64_t* retransmit_queue_enqueued_ns;
    uint64_t* retransmit_requested_generation;
    uint16_t  retransmit_queue_count;

    bool*     summary_dirty_tiles;
    uint16_t* summary_dirty_queue;
    uint16_t  summary_dirty_count;
    uint64_t  summary_epoch;
    uint16_t  summary_async_pending_count;
    bool      summary_async_pending_full;

    uint16_t pending_cursor_shape;
    bool     pending_cursor_shape_dirty;

    struct wd_async_tcp_sender* control_tx;
    struct wd_async_tcp_sender* video_tx;
    struct wd_async_udp_sender* udp_tx;
    struct wd_audio_stream*     audio_stream;
    uint64_t                    audio_captured_frames_seen;
    uint64_t                    audio_capture_overruns_seen;
    uint64_t                    audio_encoded_packets_seen;
    uint64_t                    audio_encoded_bytes_seen;
    uint64_t                    audio_queue_drops_seen;
    uint64_t                    audio_discontinuities_seen;
    uint64_t                    audio_encode_failures_seen;
    uint64_t                    control_tx_failed_seen;
    uint64_t                    control_tx_queued_seen;
    uint64_t                    control_tx_completed_seen;
    uint64_t                    control_tx_partial_seen;
    uint64_t                    control_tx_overflow_seen;
    uint64_t                    video_tx_failed_seen;
    uint64_t                    udp_tx_failed_seen;
    uint64_t                    udp_tx_queued_seen;
    uint64_t                    udp_tx_completed_seen;
    uint64_t                    udp_tx_sqe_exhausted_seen;
    uint64_t                    udp_tx_submit_calls_seen;
    uint64_t                    udp_tx_partial_submits_seen;

    int listen_fd;
    int tcp_fd;
    int input_tcp_fd;
    int selection_tcp_fd;
    int video_tcp_fd;
    int audio_tcp_fd;
    int udp_fd;

    struct in_addr listen_address;
    uint16_t       tcp_port;
    uint16_t       udp_port;
    uint8_t        session_id;
    uint64_t       connection_token;
    uint64_t       connection_epoch;
    uint64_t       config_epoch;
    uint64_t       content_epoch;
    uint64_t       media_clock_id;
    uint64_t       media_clock_start_ns;
    bool           config_update_pending;
    uint64_t       config_update_sent_ns;

    bool     video_stream_negotiated;
    uint32_t video_codecs;
    uint16_t video_transport;

    bool     audio_stream_negotiated;
    uint32_t audio_codec;
    uint16_t audio_transport;
    uint8_t  audio_channels;
    uint16_t audio_target_latency_ms;
    uint32_t audio_bitrate;
    uint64_t audio_epoch;

    struct sockaddr_in      client_udp_addr;
    struct wd_stream_policy stream_policy;

    struct wd_tile_state* tiles;
    struct wd_stats       stats;
    uint64_t              last_input_inject_ns;
    bool                  input_since_last_summary;
    bool                  input_since_last_fresh_tile;
    uint64_t              last_input_sequence;
    uint64_t              input_correlation_inflight_sequence;
    uint64_t              udp_send_pressure_log_ns;
    uint64_t              udp_send_pressure_drops;

    struct wd_queued_key_event key_queue[WD_SERVER_KEY_QUEUE_CAPACITY];
    size_t                     key_queue_count;
    bool                       key_state_reset_pending;

    struct wd_queued_pointer_event pointer_queue[WD_SERVER_POINTER_QUEUE_CAPACITY];
    size_t                         pointer_queue_count;

    uint8_t* clipboard_text;
    uint32_t clipboard_text_size;
    bool     clipboard_text_pending;
    bool     clipboard_paste_pending;

    uint8_t* primary_text;
    uint32_t primary_text_size;
    bool     primary_text_pending;
    bool     clipboard_request_pending;
    bool     primary_request_pending;
};

struct wd_server {
    struct wl_display*    display;
    struct wl_event_loop* event_loop;

    struct wlr_backend*    backend;
    struct wlr_renderer*   renderer;
    struct wlr_allocator*  allocator;
    struct wlr_compositor* compositor;

    struct wlr_output*                output;
    struct wlr_output_layout*         output_layout;
    struct wlr_xdg_output_manager_v1* xdg_output_manager;
    struct wlr_scene*                 scene;
    struct wlr_scene_tree*            scene_views;
    struct wlr_scene_output_layout*   scene_layout;
    struct wlr_scene_output*          scene_output;
    _Atomic uint32_t                  compositor_requests;
    _Atomic uint64_t                  input_wakeup_write_failures;
    bool                              scene_dirty;
    bool                              damage_all_tiles;
    bool*                             damage_tiles;
    uint32_t                          damage_tile_count;
    struct wd_stream_frame_worker*    stream_frame_worker;

    struct wlr_xdg_shell*                    xdg_shell;
    struct wlr_xdg_decoration_manager_v1*    xdg_decoration_manager;
    struct wlr_xdg_activation_v1*            xdg_activation;
    struct wlr_xdg_foreign_registry*         xdg_foreign_registry;
    struct wl_global*                        xdg_dialog_manager_global;
    struct wlr_xdg_toplevel_icon_manager_v1* xdg_toplevel_icon_manager;
    struct wlr_cursor_shape_manager_v1*      cursor_shape_manager;
    struct wlr_pointer_gestures_v1*          pointer_gestures;
    struct wlr_fractional_scale_manager_v1*  fractional_scale_manager;
    struct wlr_viewporter*                   viewporter;
#if WAYDISPLAY_ENABLE_XWAYLAND
    struct wlr_xwayland* xwayland;
    struct wl_listener   xwayland_ready;
    struct wl_listener   new_xwayland_surface;
    bool                 enable_xwayland;
#endif
    bool     enable_xdg_dialog;
    double   output_scale;
    uint32_t output_refresh_mhz;
    uint8_t  tile_compression_benchmark_mode;

    uint32_t display_width;
    uint32_t display_height;
    uint16_t tile_width;
    uint16_t tile_height;
    uint32_t uncompressed_tile_bytes;
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

    struct wlr_data_device_manager*                 data_device_manager;
    struct wlr_primary_selection_v1_device_manager* primary_selection_manager;

    struct wlr_seat*                                  seat;
    struct wlr_keyboard_shortcuts_inhibit_manager_v1* keyboard_shortcuts_inhibit_manager;
    struct wl_list                                    keyboard_shortcuts_inhibitors;
    struct wlr_keyboard_group*                        keyboard_group;
    struct wlr_keyboard*                              keyboard;
    uint32_t                                          pressed_keycodes[WD_SERVER_PRESSED_KEY_CAPACITY];
    size_t                                            pressed_keycode_count;

    struct wl_list views;
    struct wl_list popup_commit_trackers;

    struct wd_view*     focused_view;
    struct wlr_surface* focused_surface;

    double   pointer_x;
    double   pointer_y;
    uint32_t next_view_offset;

    bool                pointer_button_grab_active;
    struct wd_view*     pointer_button_grab_view;
    struct wlr_surface* pointer_button_grab_surface;
    struct wl_listener  pointer_button_grab_surface_destroy;
    double              pointer_button_grab_layout_x;
    double              pointer_button_grab_layout_y;
    double              pointer_button_grab_surface_sx;
    double              pointer_button_grab_surface_sy;
    uint32_t            pointer_button_grab_count;
    uint32_t            pointer_button_grab_buttons;

    struct wd_move_grab   move_grab;
    struct wd_resize_grab resize_grab;

    uint16_t cursor_shape;

    struct wl_listener new_xdg_surface;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_listener new_xdg_toplevel_decoration;
    struct wl_listener request_activate;
    struct wl_listener set_xdg_toplevel_icon;
    struct wl_listener request_cursor_shape;
    struct wl_listener request_set_cursor;
    struct wl_listener new_keyboard_shortcuts_inhibitor;
    struct wl_listener keyboard_shortcuts_inhibit_manager_destroy;
    struct wl_listener output_frame;
    struct wl_listener output_destroy;
    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;

    struct wl_event_source* frame_timer;
    int                     input_wakeup_fd;
    struct wl_event_source* input_wakeup_source;

    uint8_t*                remote_clipboard_text;
    uint32_t                remote_clipboard_text_size;
    struct wlr_data_source* remote_clipboard_source;

    uint8_t*                             remote_primary_text;
    uint32_t                             remote_primary_text_size;
    struct wlr_primary_selection_source* remote_primary_source;

    struct wd_selection_capture* clipboard_capture;
    struct wd_selection_capture* primary_capture;
    struct wd_selection_delivery local_clipboard;
    struct wd_selection_delivery local_primary;

    uint64_t                  last_summary_ns;
    uint64_t                  last_delta_summary_ns;
    uint64_t                  last_stats_ns;
    uint64_t                  last_stats_log_ns;
    struct wd_stats_log_state stats_log;

    uint32_t* framebuffer_xrgb8888;
    uint32_t* framebuffer_shadow_xrgb8888;
    bool      framebuffer_shadow_valid;
    uint64_t  framebuffer_generation;

    const char*               socket_name;
    const char*               startup_command;
    struct wd_spawned_process startup_process;
    const char*               video_encoder_backend;

    struct wd_net_state net;
};

#ifdef __cplusplus
}
#endif
