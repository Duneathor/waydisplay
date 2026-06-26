#pragma once

#include "client_async_udp.hpp"
#include "client_state.hpp"

#include <cstdint>

namespace waydisplay {

/* Transport-stage operations. These functions own socket/channel setup and
 * asynchronous sender/receiver bookkeeping, but not protocol message handling. */
ClientAsyncTcpSender* create_client_tcp_sender(const char* label);
void                  destroy_client_tcp_sender(ClientAsyncTcpSender*& sender);
ClientAsyncUdpReceiver* create_client_udp_receiver(ClientState& state, const wd_server_config_payload& config);
ClientAsyncUdpDetachResult destroy_client_udp_receiver(ClientState& state);

bool client_send_tcp_message_queued(ClientState& state, int fd, uint16_t message_type, const void* payload, uint32_t payload_size);
bool update_async_seen(ClientState& state, ClientAsyncTcpSender* sender, ClientAsyncTcpStatsSeen& seen);
void update_async_udp_seen(ClientState& state);

bool open_udp_socket(ClientState& state);
bool connect_udp_socket_to_server(ClientState& state, const wd_server_config_payload& config);
bool open_tcp_socket(ClientState& state);
bool open_input_tcp_socket(ClientState& state);
bool open_selection_tcp_socket(ClientState& state);
bool open_video_tcp_socket(ClientState& state);
bool open_audio_tcp_socket(ClientState& state);

} // namespace waydisplay
