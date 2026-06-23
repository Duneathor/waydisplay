#pragma once

#include <cstdint>

namespace waydisplay {

struct ClientAsyncTcpSender;

struct ClientAsyncTcpSenderStats {
    uint64_t queued            = 0;
    uint64_t completed         = 0;
    uint64_t failed            = 0;
    uint64_t overflows         = 0;
    uint64_t partial_resubmits = 0;
    uint64_t coalesced         = 0;
    uint64_t inflight_max      = 0;
    uint64_t inflight          = 0;
    uint64_t pending_bytes     = 0;
};

ClientAsyncTcpSender*     client_async_tcp_sender_create(uint32_t entries, uint64_t max_pending_bytes);
ClientAsyncTcpSenderStats client_async_tcp_sender_destroy(ClientAsyncTcpSender* sender);

void client_async_tcp_sender_reap(ClientAsyncTcpSender* sender);

bool client_async_tcp_send_message(ClientAsyncTcpSender* sender, int fd, uint16_t message_type, const void* payload, uint32_t payload_size);

ClientAsyncTcpSenderStats client_async_tcp_sender_stats(ClientAsyncTcpSender* sender);

} // namespace waydisplay
