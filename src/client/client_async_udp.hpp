#pragma once

#include <cstddef>
#include <cstdint>

namespace waydisplay {

struct ClientAsyncUdpReceiver;

struct ClientAsyncUdpReceiverStats {
    uint64_t posted            = 0;
    uint64_t retired           = 0;
    uint64_t completed         = 0;
    uint64_t failed            = 0;
    uint64_t submit_failed     = 0;
    uint64_t cancels           = 0;
    uint64_t inflight          = 0;
    uint64_t prepared          = 0;
    uint64_t inflight_max      = 0;
    uint64_t accounting_errors = 0;
    bool     fatal             = false;
};

using ClientAsyncUdpPacketHandler = bool (*)(void* userdata, const uint8_t* data, size_t size);

ClientAsyncUdpReceiver*    client_async_udp_receiver_create(int fd, uint32_t entries, size_t packet_size);
void client_async_udp_receiver_destroy(ClientAsyncUdpReceiver* receiver, ClientAsyncUdpReceiverStats* final_stats = nullptr);
bool                       client_async_udp_receiver_ready(ClientAsyncUdpReceiver* receiver);
int                        client_async_udp_receiver_poll_fd(ClientAsyncUdpReceiver* receiver);

bool client_async_udp_receiver_drain(ClientAsyncUdpReceiver* receiver, void* userdata, ClientAsyncUdpPacketHandler handler,
                                     uint32_t max_packets);

ClientAsyncUdpReceiverStats client_async_udp_receiver_stats(ClientAsyncUdpReceiver* receiver);

} // namespace waydisplay
