#pragma once

#include "wd_client.hpp"

#include <cstdint>

namespace waydisplay {

bool client_connect(ClientState& state, const char* server_host, uint16_t tcp_port, uint16_t client_udp_port,
                    const ClientStreamConfig& stream_config, uint16_t desired_width, uint16_t desired_height);

void client_disconnect(ClientState& state);

bool client_start_tcp_reader(ClientState& state);

bool client_send_keyboard_key(ClientState& state, uint16_t evdev_key_code, bool pressed);
bool client_send_pointer_event(ClientState& state, const wd_pointer_event_payload& event);
bool client_send_clipboard_text(ClientState& state, const char* text);
bool client_send_primary_text(ClientState& state, const char* text);
bool client_send_display_resize(ClientState& state, uint16_t width, uint16_t height);

void client_promote_deferred_summary_retransmits(ClientState& state);
bool client_flush_retransmit_requests(ClientState& state);
bool client_send_stats(ClientState& state, const wd_client_stats_payload& stats);

} // namespace waydisplay
