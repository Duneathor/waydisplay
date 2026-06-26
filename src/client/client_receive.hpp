#pragma once

#include <cstdint>

namespace waydisplay {

struct ClientState;
struct ClientReceiveState;

ClientReceiveState* client_receive_state_create(ClientState& state);
void                client_receive_state_destroy(ClientReceiveState* receive_state);
bool                client_receive_udp_paused(ClientState& state);
bool                client_receive_udp_service(ClientState& state, ClientReceiveState& receive_state);
uint64_t            client_receive_udp_deadline_ns(ClientState& state, const ClientReceiveState& receive_state, uint64_t now_ns);

} // namespace waydisplay
