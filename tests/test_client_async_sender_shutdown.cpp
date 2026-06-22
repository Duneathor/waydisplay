#include "client_async_tcp.hpp"

#include "waydisplay/wd_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(condition)                                                                            \
    do                                                                                              \
    {                                                                                               \
        if (!(condition))                                                                           \
        {                                                                                           \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);             \
            std::exit(1);                                                                           \
        }                                                                                           \
    } while (0)

int main() {
    using namespace waydisplay;

    ClientAsyncTcpSender* sender = client_async_tcp_sender_create(8, 4096);
    if (!sender)
    {
        return 77;
    }

    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    const unsigned char payload[32] = {};
    CHECK(client_async_tcp_send_message(sender, sockets[0], WD_MSG_POINTER_EVENT,
                                        payload, sizeof(payload)));

    const ClientAsyncTcpSenderStats stats = client_async_tcp_sender_destroy(sender);
    CHECK(stats.queued == 1);
    CHECK(stats.completed == 0);
    CHECK(stats.failed == 1);
    CHECK(stats.inflight == 0);
    CHECK(stats.pending_bytes == 0);

    ::close(sockets[0]);
    ::close(sockets[1]);
    return 0;
}
