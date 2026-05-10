#pragma once

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <netinet/in.h>

#include <wayland-server-core.h>

#include <xkbcommon/xkbcommon.h>

#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/util/log.h>

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"

#ifdef __cplusplus
extern "C" {
    #endif

    #define WD_KEY_QUEUE_CAP 4096
    #define WD_POINTER_QUEUE_CAP 4096

    struct wd_server;

    struct wd_view {
        struct wd_server *server;

        struct wl_list link;

        struct wlr_xdg_surface *xdg_surface;
        struct wlr_scene_tree *scene_tree;

        struct wl_listener map;
        struct wl_listener unmap;
        struct wl_listener commit;
        struct wl_listener request_move;
        struct wl_listener xdg_surface_destroy;
        struct wl_listener xdg_toplevel_destroy;
        struct wl_listener new_popup;

        struct wl_event_source *configure_idle;

        int x;
        int y;

        bool mapped;
        bool configured_once;
    };

    struct wd_move_grab {
        bool active;
        struct wd_view *view;

        double grab_x;
        double grab_y;

        int view_x;
        int view_y;
    };

    struct wd_cached_tile {
        uint8_t *compressed;
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
    };

    struct wd_stream_policy {
        uint16_t mode;
        uint16_t target_fps;
        uint32_t max_tiles_per_second;

        uint64_t last_frame_send_ns;

        double tile_tokens;
        uint64_t last_token_refill_ns;
    };

    struct wd_queued_key_event {
        uint16_t evdev_key_code;
        bool pressed;
        uint64_t client_timestamp_ns;
    };

    struct wd_queued_pointer_event {
        struct wd_pointer_event_payload event;
    };

    struct wd_net_state {
        pthread_mutex_t lock;

        bool running;
        bool client_connected;
        bool full_frame_needed;

        int listen_fd;
        int tcp_fd;
        int udp_fd;

        uint16_t tcp_port;
        uint32_t session_id;

        struct sockaddr_in client_udp_addr;
        struct wd_stream_policy stream_policy;

        struct wd_cached_tile tiles[WD_TOTAL_TILES];
        struct wd_stats stats;

        struct wd_queued_key_event key_queue[WD_KEY_QUEUE_CAP];
        size_t key_queue_count;

        struct wd_queued_pointer_event pointer_queue[WD_POINTER_QUEUE_CAP];
        size_t pointer_queue_count;
    };

    struct wd_server {
        struct wl_display *display;
        struct wl_event_loop *event_loop;

        struct wlr_backend *backend;
        struct wlr_renderer *renderer;
        struct wlr_allocator *allocator;

        struct wlr_output *output;
        struct wlr_scene *scene;
        struct wlr_scene_output *scene_output;
        bool scene_dirty;

        struct wlr_xdg_shell *xdg_shell;
        struct wlr_viewporter *viewporter;

        struct wlr_seat *seat;
        struct wlr_keyboard_group *keyboard_group;
        struct wlr_keyboard *keyboard;

        struct wl_list views;

        struct wd_view *focused_view;
        struct wlr_surface *focused_surface;

        double pointer_x;
        double pointer_y;
        uint32_t next_view_offset;

        struct wd_move_grab move_grab;

        struct wl_listener new_xdg_surface;
        struct wl_listener new_xdg_toplevel;
        struct wl_listener output_frame;
        struct wl_listener output_destroy;

        struct wl_event_source *frame_timer;

        uint64_t last_summary_ns;
        uint64_t last_stats_ns;

        uint32_t *framebuffer_xrgb8888;

        const char *socket_name;
        const char *startup_command;

        struct wd_net_state net;
    };

    /* wd_server.c */
    bool wd_server_init(struct wd_server *server,
                        uint16_t tcp_port,
                        const char *app_cmd);

    void wd_server_destroy(struct wd_server *server);

    int wd_server_run(struct wd_server *server);

    /* wd_wlroots_backend.c */
    bool wd_wlroots_init(struct wd_server *server);
    bool wd_wlroots_start(struct wd_server *server);
    bool wd_wlroots_create_headless_output(struct wd_server *server);

    /* wd_scene.c */
    void wd_scene_init_listeners(struct wd_server *server);
    void wd_scene_focus_view(struct wd_view *view);
    void wd_scene_set_view_position(struct wd_view *view);

    /* wd_readback.c */
    bool wd_render_scene_and_readback_xrgb8888(struct wd_server *server);

    /* wd_stream.c */
    bool wd_stream_init(struct wd_server *server);
    void wd_stream_destroy(struct wd_server *server);
    bool wd_stream_send_dirty_tiles(struct wd_server *server);
    bool wd_stream_send_generation_summary_locked(struct wd_server *server);
    bool wd_stream_send_cached_tile_locked(struct wd_server *server, uint16_t tile_id);
    void wd_stream_print_and_reset_stats(struct wd_server *server);

    void wd_stream_policy_set_defaults(struct wd_stream_policy *policy);
    void wd_stream_policy_apply_client_hello(struct wd_stream_policy *policy,
                                             const struct wd_client_hello_payload *hello);
    bool wd_stream_policy_should_render_now(struct wd_server *server, uint64_t now_ns);
    uint32_t wd_stream_policy_tile_budget(struct wd_server *server, uint64_t now_ns);
    void wd_stream_policy_consume_tiles(struct wd_server *server, uint32_t count);
    void wd_server_mark_scene_dirty(struct wd_server *server);

    /* wd_server_net.c */
    bool wd_net_init(struct wd_server *server, uint16_t tcp_port);
    void wd_net_destroy(struct wd_server *server);
    void *wd_net_thread_main(void *arg);

    /* wd_keyboard.c */
    bool wd_keyboard_init(struct wd_server *server);
    void wd_keyboard_queue_event_locked(struct wd_net_state *net,
                                        const struct wd_keyboard_event_payload *event);
    void wd_keyboard_drain_and_inject(struct wd_server *server);

    void wd_pointer_queue_event_locked(struct wd_net_state *net,
                                       const struct wd_pointer_event_payload *event);

    void wd_pointer_drain_and_inject(struct wd_server *server);

    struct wd_view *wd_scene_view_at(struct wd_server *server,
                                     double lx,
                                     double ly,
                                     double *sx,
                                     double *sy);

    void wd_scene_raise_view(struct wd_view *view);

    void wd_pointer_begin_move(struct wd_server *server,
                               struct wd_view *view);

    void wd_pointer_update_move(struct wd_server *server);

    void wd_pointer_end_move(struct wd_server *server);



    #ifdef __cplusplus
}
#endif
