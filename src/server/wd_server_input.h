#pragma once

#include "waydisplay/wd_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_net_state;
struct wd_server;
struct wd_view;
struct wlr_surface;

bool wd_clipboard_init(struct wd_server* server);
void wd_clipboard_destroy(struct wd_server* server);
void wd_clipboard_queue_client_set_locked(struct wd_net_state* net, uint8_t expected_session_id, uint64_t expected_connection_token,
                                          const uint8_t* payload, uint32_t payload_size, bool primary);
void wd_clipboard_drain_and_apply(struct wd_server* server);
void wd_clipboard_queue_client_request_locked(struct wd_net_state* net, bool primary);
void wd_clipboard_send_pending_locked(struct wd_server* server);

bool wd_keyboard_init(struct wd_server* server);
void wd_keyboard_queue_event_locked(struct wd_net_state* net, const struct wd_keyboard_event_payload* event,
                                    uint64_t server_rx_timestamp_ns);
void wd_keyboard_drain_and_inject(struct wd_server* server);
void wd_keyboard_note_key_state(struct wd_server* server, uint32_t evdev_key_code, bool pressed);
void wd_keyboard_notify_enter(struct wd_server* server, struct wlr_surface* surface);
void wd_keyboard_clear_pressed_keys(struct wd_server* server);

void wd_pointer_queue_event_locked(struct wd_net_state* net, const struct wd_pointer_event_payload* event,
                                   uint64_t server_rx_timestamp_ns);
void wd_pointer_drain_and_inject(struct wd_server* server);
void wd_pointer_clear_focus(struct wd_server* server);
void wd_pointer_clear_button_grab(struct wd_server* server);
void wd_pointer_clear_button_grab_for_view(struct wd_server* server, struct wd_view* view);
void wd_pointer_clear_button_grab_for_surface(struct wd_server* server, struct wlr_surface* surface);
void wd_pointer_begin_move(struct wd_server* server, struct wd_view* view);
void wd_pointer_update_move(struct wd_server* server);
void wd_pointer_end_move(struct wd_server* server);
void wd_pointer_begin_resize(struct wd_server* server, struct wd_view* view, uint32_t edges);
void wd_pointer_update_resize(struct wd_server* server);
void wd_pointer_end_resize(struct wd_server* server);

bool     wd_cursor_init(struct wd_server* server);
void     wd_cursor_destroy(struct wd_server* server);
void     wd_cursor_set_shape(struct wd_server* server, uint16_t shape);
bool     wd_cursor_flush_pending_locked(struct wd_server* server);
uint16_t wd_cursor_shape_for_resize_edges(uint32_t edges);
void     wd_cursor_queue_current_locked(struct wd_server* server);

#ifdef __cplusplus
}
#endif
