#include "client_async_tcp.hpp"
#include "waydisplay/wd_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint32_t TestVideoBytes = 512u * 1024u;
}

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                     \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

int main() {
    using namespace waydisplay;

    ClientAsyncTcpSender* sender = client_async_tcp_sender_create(8, 2u * 1024u * 1024u);
    if (!sender)
    {
        return 77;
    }

    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    int send_buffer_size = 4096;
    CHECK(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)) == 0);

    const unsigned char malformed_payload[32] = {};
    CHECK(!client_async_tcp_send_message(sender, sockets[0], WD_MSG_POINTER_EVENT, malformed_payload,
                                         sizeof(malformed_payload)));

    const uint32_t payload_size = static_cast<uint32_t>(sizeof(wd_video_frame_payload_header)) + TestVideoBytes;
    std::vector<uint8_t> payload(payload_size);
    auto* header      = reinterpret_cast<wd_video_frame_payload_header*>(payload.data());
    header->data_size = TestVideoBytes;
    CHECK(client_async_tcp_send_message(sender, sockets[0], WD_MSG_VIDEO_FRAME, payload.data(), payload_size));

    const ClientAsyncTcpSenderStats stats = client_async_tcp_sender_destroy(sender);
    CHECK(stats.queued == 1);
    CHECK(stats.completed == 0);
    CHECK(stats.failed == 2);
    CHECK(stats.inflight == 0);
    CHECK(stats.pending_bytes == 0);

    ::close(sockets[0]);
    ::close(sockets[1]);
    return 0;
}
