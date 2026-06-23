#include "wd_net_listener.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            exit(1);                                                                                                                       \
        }                                                                                                                                  \
    } while (0)

static struct in_addr socket_address(int fd) {
    struct sockaddr_in address;
    socklen_t          address_size = sizeof(address);
    CHECK(getsockname(fd, (struct sockaddr*)&address, &address_size) == 0);
    CHECK(address_size == sizeof(address));
    return address.sin_addr;
}

int main(void) {
    struct in_addr loopback;
    CHECK(inet_pton(AF_INET, "127.0.0.1", &loopback) == 1);

    struct wd_net_listener     first;
    enum wd_net_listener_stage stage      = WD_NET_LISTENER_STAGE_NONE;
    int                        error_code = 0;

    CHECK(wd_net_listener_open(&first, 0, &loopback, &stage, &error_code));
    CHECK(first.listen_fd >= 0);
    CHECK(first.udp_fd >= 0);
    CHECK(first.tcp_port != 0);
    CHECK(first.udp_port != 0);
    CHECK((fcntl(first.listen_fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(first.udp_fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(first.udp_fd, F_GETFL) & O_NONBLOCK) != 0);
    CHECK(socket_address(first.listen_fd).s_addr == loopback.s_addr);
    CHECK(socket_address(first.udp_fd).s_addr == loopback.s_addr);

    struct wd_net_listener conflict;
    stage      = WD_NET_LISTENER_STAGE_NONE;
    error_code = 0;
    CHECK(!wd_net_listener_open(&conflict, first.tcp_port, &loopback, &stage, &error_code));
    CHECK(stage == WD_NET_LISTENER_STAGE_TCP_BIND);
    CHECK(error_code == EADDRINUSE);
    CHECK(conflict.listen_fd == -1);
    CHECK(conflict.udp_fd == -1);
    CHECK(conflict.tcp_port == 0);
    CHECK(conflict.udp_port == 0);

    struct in_addr         any = {.s_addr = htonl(INADDR_ANY)};
    struct wd_net_listener exposed;
    CHECK(wd_net_listener_open(&exposed, 0, &any, &stage, &error_code));
    CHECK(socket_address(exposed.listen_fd).s_addr == any.s_addr);
    CHECK(socket_address(exposed.udp_fd).s_addr == any.s_addr);
    wd_net_listener_close(&exposed);

    wd_net_listener_close(&first);
    CHECK(first.listen_fd == -1);
    CHECK(first.udp_fd == -1);
    return 0;
}
