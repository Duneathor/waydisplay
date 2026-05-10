#pragma once

#include <cstdint>

#include "wd_client.hpp"

namespace waydisplay {

bool client_connect(ClientState& state,
                    const char* server_host,
                    uint16_t tcp_port,
                    uint16_t client_udp_port);

void client_disconnect(ClientState& state);

bool client_start_tcp_reader(ClientState& state);

bool client_send_keyboard_key(ClientState& state,
                              uint16_t evdev_key_code,
                              bool pressed);

bool client_flush_retransmit_requests(ClientState& state);

} // namespace waydisplay
