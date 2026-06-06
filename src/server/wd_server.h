#pragma once

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
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
#if __has_include(<wlr/xwayland/xwayland.h>)
#include <wlr/xwayland/xwayland.h>
#elif __has_include(<wlr/xwayland.h>)
#include <wlr/xwayland.h>
#endif
#endif
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_protocol.h"

#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
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

#define WD_KEY_QUEUE_CAP     4096
#define WD_POINTER_QUEUE_CAP 4096

struct wd_server;

struct wd_view {
    struct wd_server* server;

    struct wl_list link;

    struct wlr_xdg_surface* xdg_surface;
#if WAYDISPLAY_ENABLE_XWAYLAND
    struct wlr_xwayland_surface* xwayland_surface;
#endif
    struct wlr_scene_tree* scene_tree;
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
    bool                xwayland_had_map_request;
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

struct wd_cached_tile {
    uint8_t* compressed;
    uint32_t compressed_size;
    uint32_t compressed_capacity;

    uint64_t generation;
    uint64_t timestamp_ns;

    uint32_t last_hash;
};

struct wd_stats {
    uint64_t dirty_tiles;
    uint64_t udp_tiles_sent;
    uint64_t udp_packets_sent;
    uint64_t udp_bytes_sent;

    uint64_t tcp_hello_rx;
    uint64_t tcp_config_tx;
    uint64_t tcp_summary_tx;

    uint64_t retx_req_rx;
    uint64_t retx_tiles_req;

    uint64_t key_events_rx;
    uint64_t key_events_injected;
    uint64_t key_events_dropped;

    uint64_t pointer_events_rx;
    uint64_t pointer_events_injected;
    uint64_t pointer_events_dropped;

    uint64_t input_net_latency_samples;
    uint64_t input_net_latency_sum_ns;
    uint64_t input_queue_latency_samples;
    uint64_t input_queue_latency_sum_ns;
    uint64_t input_to_summary_samples;
    uint64_t input_to_summary_sum_ns;
};

struct wd_stream_policy {
    uint16_t mode;
    uint16_t target_fps;
    uint32_t max_tiles_per_second;

    uint32_t max_retransmit_tiles_per_second;
    double   retransmit_tile_tokens;
    uint64_t last_retransmit_token_refill_ns;

    uint64_t last_frame_send_ns;

    double   tile_tokens;
    uint64_t last_token_refill_ns;
};

struct wd_queued_key_event {
    uint16_t evdev_key_code;
    bool     pressed;
    uint64_t client_timestamp_ns;
    uint64_t server_rx_timestamp_ns;
};

struct wd_queued_pointer_event {
    struct wd_pointer_event_payload event;
    uint64_t                        server_rx_timestamp_ns;
};

struct wd_net_state {
    pthread_mutex_t lock;

    bool running;
    bool client_connected;
    bool full_frame_needed;

    uint16_t udp_payload_target;

    uint16_t full_frame_next_tile;
    uint16_t dirty_scan_next_tile;

    uint16_t* dirty_queue;
    bool*     dirty_queued;
    uint16_t  dirty_queue_read;
    uint16_t  dirty_queue_write;
    uint16_t  dirty_queue_count;

    int listen_fd;
    int tcp_fd;
    int udp_fd;

    uint16_t tcp_port;
    uint32_t session_id;

    struct sockaddr_in      client_udp_addr;
    struct wd_stream_policy stream_policy;

    struct wd_cached_tile* tiles;
    struct wd_stats        stats;
    uint64_t               last_input_inject_ns;
    bool                   input_since_last_summary;

    struct wd_queued_key_event key_queue[WD_KEY_QUEUE_CAP];
    size_t                     key_queue_count;

    struct wd_queued_pointer_event pointer_queue[WD_POINTER_QUEUE_CAP];
    size_t                         pointer_queue_count;

    uint8_t* clipboard_text;
    uint32_t clipboard_text_size;
    bool     clipboard_text_pending;
    bool     clipboard_paste_pending;

    uint8_t* primary_text;
    uint32_t primary_text_size;
    bool     primary_text_pending;
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
    struct wlr_scene_output*          scene_output;
    bool                              scene_dirty;
    bool                              damage_all_tiles;
    bool*                             damage_tiles;
    uint16_t                          damage_tile_count;

    struct wlr_xdg_shell*                    xdg_shell;
    struct wlr_xdg_decoration_manager_v1*    xdg_decoration_manager;
    struct wlr_xdg_activation_v1*            xdg_activation;
    struct wlr_xdg_foreign_registry*         xdg_foreign_registry;
    struct wl_global*                        xdg_dialog_manager_global;
    struct wlr_xdg_toplevel_icon_manager_v1* xdg_toplevel_icon_manager;
    struct wlr_cursor_shape_manager_v1*      cursor_shape_manager;
    struct wlr_fractional_scale_manager_v1*  fractional_scale_manager;
    struct wlr_viewporter*                   viewporter;
#if WAYDISPLAY_ENABLE_XWAYLAND
    struct wlr_xwayland* xwayland;
    struct wl_listener   xwayland_ready;
    struct wl_listener   new_xwayland_surface;
    bool                 enable_xwayland;
#endif
    double output_scale;

    uint32_t display_width;
    uint32_t display_height;
    uint16_t tiles_x;
    uint16_t tiles_y;
    uint16_t total_tiles;
    uint32_t framebuffer_pixels;
    uint32_t framebuffer_bytes;

    struct wlr_data_device_manager*                 data_device_manager;
    struct wlr_primary_selection_v1_device_manager* primary_selection_manager;

    struct wlr_seat*                                  seat;
    struct wlr_keyboard_shortcuts_inhibit_manager_v1* keyboard_shortcuts_inhibit_manager;
    struct wl_list                                    keyboard_shortcuts_inhibitors;
    struct wlr_keyboard_group*                        keyboard_group;
    struct wlr_keyboard*                              keyboard;

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
    double              pointer_button_grab_surface_lx;
    double              pointer_button_grab_surface_ly;
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
    struct wl_listener new_keyboard_shortcuts_inhibitor;
    struct wl_listener keyboard_shortcuts_inhibit_manager_destroy;
    struct wl_listener output_frame;
    struct wl_listener output_destroy;
    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;

    struct wl_event_source* frame_timer;

    uint8_t*                remote_clipboard_text;
    uint32_t                remote_clipboard_text_size;
    struct wlr_data_source* remote_clipboard_source;

    uint8_t*                             remote_primary_text;
    uint32_t                             remote_primary_text_size;
    struct wlr_primary_selection_source* remote_primary_source;

    uint64_t last_summary_ns;
    uint64_t last_stats_ns;

    uint32_t* framebuffer_xrgb8888;

    const char* socket_name;
    const char* startup_command;

    struct wd_net_state net;
};

/* wd_server.c */
bool wd_server_init(struct wd_server* server, uint16_t tcp_port, const char* app_cmd, double output_scale, uint32_t display_width,
                    uint32_t display_height, bool enable_xwayland);

void wd_server_destroy(struct wd_server* server);

int wd_server_run(struct wd_server* server);

/* wd_wlroots_backend.c */
bool wd_wlroots_init(struct wd_server* server);
bool wd_wlroots_start(struct wd_server* server);
bool wd_wlroots_create_headless_output(struct wd_server* server);
bool wd_wlroots_resize_headless_output(struct wd_server* server);

#if WAYDISPLAY_ENABLE_XWAYLAND
/* wd_xwayland.c */
bool wd_xwayland_init(struct wd_server* server);
void wd_xwayland_destroy(struct wd_server* server);
bool wd_xwayland_view_has_decoration(struct wd_view* view);
bool wd_xwayland_view_decoration_at(struct wd_view* view, double sx, double sy);
bool wd_xwayland_view_handle_decoration_press(struct wd_view* view, double sx, double sy);
#endif

/* wd_xdg_decoration.c */
bool wd_xdg_decoration_init(struct wd_server* server);
void wd_xdg_decoration_destroy(struct wd_server* server);

/* wd_xdg_activation.c */
bool wd_xdg_activation_init(struct wd_server* server);
void wd_xdg_activation_destroy(struct wd_server* server);

/* wd_xdg_foreign.c */
bool wd_xdg_foreign_init(struct wd_server* server);
void wd_xdg_foreign_destroy(struct wd_server* server);

/* wd_xdg_dialog.c */
bool wd_xdg_dialog_init(struct wd_server* server);
void wd_xdg_dialog_destroy(struct wd_server* server);

/* wd_xdg_toplevel_icon.c */
bool wd_xdg_toplevel_icon_init(struct wd_server* server);
void wd_xdg_toplevel_icon_destroy(struct wd_server* server);

/* wd_keyboard_shortcuts_inhibit.c */
bool wd_keyboard_shortcuts_inhibit_init(struct wd_server* server);
void wd_keyboard_shortcuts_inhibit_destroy(struct wd_server* server);
void wd_keyboard_shortcuts_inhibit_refresh(struct wd_server* server);
bool wd_keyboard_shortcuts_inhibit_active(struct wd_server* server);

/* wd_cursor.c */
bool     wd_cursor_init(struct wd_server* server);
void     wd_cursor_destroy(struct wd_server* server);
void     wd_cursor_set_shape(struct wd_server* server, uint16_t shape);
uint16_t wd_cursor_shape_for_resize_edges(uint32_t edges);

/* net lock must already be held. */
bool wd_cursor_send_current_locked(struct wd_server* server);

/* wd_scene.c */
void            wd_scene_init_listeners(struct wd_server* server);
void            wd_scene_focus_view(struct wd_view* view);
void            wd_scene_deactivate_view(struct wd_view* view);
void            wd_scene_set_view_position(struct wd_view* view);
void            wd_scene_note_dialog_state(struct wd_view* view);
struct wd_view* wd_scene_view_from_xdg_toplevel_resource(struct wd_server* server, struct wl_resource* toplevel_resource);

/* wd_readback.c */
bool wd_render_scene_and_readback_xrgb8888(struct wd_server* server);

/* wd_stream.c */
bool wd_stream_init(struct wd_server* server);
void wd_stream_destroy(struct wd_server* server);
bool wd_stream_send_dirty_tiles(struct wd_server* server);
bool wd_stream_send_generation_summary_locked(struct wd_server* server);
bool wd_stream_send_cached_tile_locked(struct wd_server* server, uint16_t tile_id);
void wd_stream_print_and_reset_stats(struct wd_server* server);

void     wd_stream_policy_set_defaults(struct wd_stream_policy* policy);
void     wd_stream_policy_apply_client_hello(struct wd_stream_policy* policy, const struct wd_client_hello_payload* hello);
bool     wd_stream_policy_should_render_now(struct wd_server* server, uint64_t now_ns);
uint32_t wd_stream_policy_tile_budget(struct wd_server* server, uint64_t now_ns);
void     wd_stream_policy_consume_tiles(struct wd_server* server, uint32_t count);
uint32_t wd_stream_policy_retransmit_budget(struct wd_server* server, uint64_t now_ns);

void wd_stream_policy_consume_retransmit_tiles(struct wd_server* server, uint32_t count);
void wd_server_mark_scene_dirty(struct wd_server* server);
void wd_server_mark_rect_dirty(struct wd_server* server, int x, int y, int width, int height);
void wd_server_mark_view_dirty(struct wd_view* view);
void wd_server_mark_view_move_dirty(struct wd_view* view, int old_x, int old_y);
bool wd_server_set_geometry(struct wd_server* server, uint32_t width, uint32_t height);
bool wd_server_apply_display_size(struct wd_server* server, uint32_t width, uint32_t height);
void wd_server_set_default_geometry(struct wd_server* server);

/* wd_clipboard.c */
bool wd_clipboard_init(struct wd_server* server);
void wd_clipboard_destroy(struct wd_server* server);
void wd_clipboard_queue_client_set_locked(struct wd_net_state* net, uint32_t expected_session_id, const uint8_t* payload,
                                          uint32_t payload_size, bool primary);
void wd_clipboard_drain_and_apply(struct wd_server* server);

/* wd_server_net.c */
bool  wd_net_init(struct wd_server* server, uint16_t tcp_port);
void  wd_net_destroy(struct wd_server* server);
void* wd_net_thread_main(void* arg);

/* wd_keyboard.c */
bool wd_keyboard_init(struct wd_server* server);
void wd_keyboard_queue_event_locked(struct wd_net_state* net, const struct wd_keyboard_event_payload* event,
                                    uint64_t server_rx_timestamp_ns);
void wd_keyboard_drain_and_inject(struct wd_server* server);

void wd_pointer_queue_event_locked(struct wd_net_state* net, const struct wd_pointer_event_payload* event, uint64_t server_rx_timestamp_ns);

void wd_pointer_drain_and_inject(struct wd_server* server);
void wd_pointer_clear_focus(struct wd_server* server);

struct wd_view* wd_scene_view_at(struct wd_server* server, double lx, double ly, double* sx, double* sy);

void wd_scene_raise_view(struct wd_view* view);

void wd_pointer_begin_move(struct wd_server* server, struct wd_view* view);

void wd_pointer_update_move(struct wd_server* server);

void wd_pointer_end_move(struct wd_server* server);

void wd_pointer_begin_resize(struct wd_server* server, struct wd_view* view, uint32_t edges);

void wd_pointer_update_resize(struct wd_server* server);

void wd_pointer_end_resize(struct wd_server* server);

#ifdef __cplusplus
}
#endif
