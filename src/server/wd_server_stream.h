#pragma once

#include "waydisplay/wd_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_net_state;
struct wd_server;
struct wd_stream_policy;
struct wd_view;
struct wlr_box;
struct wlr_scene_node;

bool     wd_stream_init(struct wd_server* server);
void     wd_stream_destroy(struct wd_server* server);
void     wd_stream_invalidate_all_tiles_locked(struct wd_server* server);
void     wd_stream_wait_for_encoder_idle_locked(struct wd_server* server);
bool     wd_stream_send_generation_summary_locked(struct wd_server* server);
bool     wd_stream_send_pending_generation_summary_locked(struct wd_server* server);
bool     wd_stream_queue_retransmit_tile_locked(struct wd_server* server, uint16_t tile_id, uint64_t requested_generation);
void     wd_stream_sample_and_maybe_log_stats(struct wd_server* server, bool log_stats);
bool     wd_stream_try_consume_tcp_control_budget_locked(struct wd_net_state* net, uint32_t bytes, uint64_t now_ns);
void     wd_stream_account_tcp_control_bytes_locked(struct wd_net_state* net, uint32_t bytes);

void wd_stream_policy_set_defaults(struct wd_stream_policy* policy);
void wd_stream_policy_apply_client_hello(struct wd_stream_policy* policy, const struct wd_client_hello_payload* hello);
void wd_stream_policy_set_udp_rate(struct wd_stream_policy* policy, uint64_t bytes_per_second);
bool wd_stream_policy_should_render_now(struct wd_server* server, uint64_t now_ns);

void wd_server_mark_scene_dirty(struct wd_server* server);
void wd_server_mark_rect_dirty(struct wd_server* server, int x, int y, int width, int height);
bool wd_server_scene_node_bounds(struct wlr_scene_node* node, struct wlr_box* out_box);
void wd_server_mark_scene_node_dirty(struct wd_server* server, struct wlr_scene_node* node);
void wd_server_mark_view_dirty(struct wd_view* view);
void wd_server_mark_view_move_dirty(struct wd_view* view, int old_x, int old_y);
bool wd_server_set_tile_size(struct wd_server* server, uint16_t tile_width, uint16_t tile_height);
bool wd_server_reconfigure_tile_size_locked(struct wd_server* server, uint16_t tile_width, uint16_t tile_height);
void wd_stream_video_reset_locked(struct wd_server* server, const char* reason, bool notify_client, bool resize);
bool wd_server_send_current_config_locked(struct wd_server* server);
bool wd_server_set_geometry(struct wd_server* server, uint32_t width, uint32_t height);
bool wd_server_apply_display_size(struct wd_server* server, uint32_t width, uint32_t height);
bool wd_server_request_display_size(struct wd_server* server, uint32_t width, uint32_t height);
void wd_server_set_default_geometry(struct wd_server* server);

#ifdef __cplusplus
}
#endif
