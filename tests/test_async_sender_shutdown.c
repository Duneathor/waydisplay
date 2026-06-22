#include "wd_async_tcp.h"
#include "wd_async_udp.h"

#include "waydisplay/wd_protocol.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(condition)                                                                            \
    do                                                                                              \
    {                                                                                               \
        if (!(condition))                                                                           \
        {                                                                                           \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                  \
            exit(1);                                                                                \
        }                                                                                           \
    } while (0)

struct completion_probe {
    unsigned calls;
    bool     success;
};

static void tcp_complete(void* data, bool success) {
    struct completion_probe* probe = data;
    probe->calls++;
    probe->success = success;
}

static void udp_complete(void* data, bool success) {
    struct completion_probe* probe = data;
    probe->calls++;
    probe->success = success;
}

static int test_tcp_forced_teardown(void) {
    struct wd_async_tcp_sender* sender = NULL;
    if (!wd_async_tcp_sender_create(&sender, 8))
    {
        return 77;
    }

    int sockets[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    const uint8_t payload[32] = {0};
    struct completion_probe probe = {0};
    CHECK(wd_async_tcp_send_message_ex(sender, sockets[0], WD_MSG_TILE_GENERATION_SUMMARY,
                                       payload, sizeof(payload), tcp_complete, &probe));

    wd_async_tcp_sender_destroy(sender);
    CHECK(probe.calls == 1);
    CHECK(!probe.success);

    close(sockets[0]);
    close(sockets[1]);
    return 0;
}

static int test_udp_forced_teardown(void) {
    struct wd_async_udp_sender* sender = NULL;
    if (!wd_async_udp_sender_create(&sender, 8))
    {
        return 77;
    }

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);

    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(9);

    const uint8_t header[8] = {0};
    const uint8_t payload[16] = {0};
    struct completion_probe probe = {0};
    CHECK(wd_async_udp_send_packet(sender, fd, &destination, header, sizeof(header),
                                   payload, sizeof(payload), udp_complete, &probe) ==
          WD_ASYNC_UDP_SEND_QUEUED);
    CHECK(wd_async_udp_sender_flush(sender));

    wd_async_udp_sender_destroy(sender);
    CHECK(probe.calls == 1);
    CHECK(!probe.success);

    close(fd);
    return 0;
}

int main(void) {
    int rc = test_tcp_forced_teardown();
    if (rc != 0)
    {
        return rc;
    }
    return test_udp_forced_teardown();
}
