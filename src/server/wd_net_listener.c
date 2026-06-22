#include "wd_net_listener.h"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool wd_net_listener_fail(struct wd_net_listener* listener,
                                 enum wd_net_listener_stage stage, int error_code,
                                 enum wd_net_listener_stage* failed_stage,
                                 int* out_error_code) {
    if (failed_stage)
    {
        *failed_stage = stage;
    }
    if (out_error_code)
    {
        *out_error_code = error_code;
    }
    wd_net_listener_close(listener);
    return false;
}

void wd_net_listener_init(struct wd_net_listener* listener) {
    if (!listener)
    {
        return;
    }

    listener->listen_fd = -1;
    listener->udp_fd    = -1;
    listener->tcp_port  = 0;
    listener->udp_port  = 0;
}

void wd_net_listener_close(struct wd_net_listener* listener) {
    if (!listener)
    {
        return;
    }

    if (listener->listen_fd >= 0)
    {
        close(listener->listen_fd);
    }
    if (listener->udp_fd >= 0)
    {
        close(listener->udp_fd);
    }

    wd_net_listener_init(listener);
}

bool wd_net_listener_open(struct wd_net_listener* listener, uint16_t requested_tcp_port,
                          const struct in_addr* bind_address,
                          enum wd_net_listener_stage* failed_stage, int* error_code) {
    if (!listener)
    {
        if (failed_stage)
        {
            *failed_stage = WD_NET_LISTENER_STAGE_NONE;
        }
        if (error_code)
        {
            *error_code = EINVAL;
        }
        return false;
    }

    wd_net_listener_init(listener);
    if (failed_stage)
    {
        *failed_stage = WD_NET_LISTENER_STAGE_NONE;
    }
    if (error_code)
    {
        *error_code = 0;
    }

    listener->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener->listen_fd < 0)
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_TCP_SOCKET, errno,
                                    failed_stage, error_code);
    }

    const int yes = 1;
    if (setsockopt(listener->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_TCP_REUSEADDR, errno,
                                    failed_stage, error_code);
    }

    struct in_addr selected_address;
    selected_address.s_addr = htonl(INADDR_LOOPBACK);
    if (bind_address)
    {
        selected_address = *bind_address;
    }

    struct sockaddr_in tcp_address;
    memset(&tcp_address, 0, sizeof(tcp_address));
    tcp_address.sin_family = AF_INET;
    tcp_address.sin_addr   = selected_address;
    tcp_address.sin_port   = htons(requested_tcp_port);

    if (bind(listener->listen_fd, (struct sockaddr*)&tcp_address, sizeof(tcp_address)) < 0)
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_TCP_BIND, errno,
                                    failed_stage, error_code);
    }

    if (listen(listener->listen_fd, WD_NET_LISTEN_BACKLOG) < 0)
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_TCP_LISTEN, errno,
                                    failed_stage, error_code);
    }

    socklen_t tcp_address_size = sizeof(tcp_address);
    if (getsockname(listener->listen_fd, (struct sockaddr*)&tcp_address,
                    &tcp_address_size) < 0)
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_TCP_GETSOCKNAME, errno,
                                    failed_stage, error_code);
    }
    if (tcp_address_size != sizeof(tcp_address))
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_TCP_GETSOCKNAME, EIO,
                                    failed_stage, error_code);
    }
    listener->tcp_port = ntohs(tcp_address.sin_port);
    if (listener->tcp_port == 0)
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_TCP_GETSOCKNAME, EIO,
                                    failed_stage, error_code);
    }

    listener->udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (listener->udp_fd < 0)
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_UDP_SOCKET, errno,
                                    failed_stage, error_code);
    }

    struct sockaddr_in udp_address;
    memset(&udp_address, 0, sizeof(udp_address));
    udp_address.sin_family = AF_INET;
    udp_address.sin_addr   = selected_address;
    udp_address.sin_port   = htons(0);

    if (bind(listener->udp_fd, (struct sockaddr*)&udp_address, sizeof(udp_address)) < 0)
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_UDP_BIND, errno,
                                    failed_stage, error_code);
    }

    socklen_t udp_address_size = sizeof(udp_address);
    if (getsockname(listener->udp_fd, (struct sockaddr*)&udp_address,
                    &udp_address_size) < 0)
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_UDP_GETSOCKNAME, errno,
                                    failed_stage, error_code);
    }
    if (udp_address_size != sizeof(udp_address))
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_UDP_GETSOCKNAME, EIO,
                                    failed_stage, error_code);
    }
    listener->udp_port = ntohs(udp_address.sin_port);
    if (listener->udp_port == 0)
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_UDP_GETSOCKNAME, EIO,
                                    failed_stage, error_code);
    }

    if (wd_set_nonblocking(listener->udp_fd) < 0)
    {
        return wd_net_listener_fail(listener, WD_NET_LISTENER_STAGE_UDP_NONBLOCK, errno,
                                    failed_stage, error_code);
    }

    return true;
}

const char* wd_net_listener_stage_name(enum wd_net_listener_stage stage) {
    switch (stage)
    {
    case WD_NET_LISTENER_STAGE_NONE:
        return "none";
    case WD_NET_LISTENER_STAGE_TCP_SOCKET:
        return "TCP socket";
    case WD_NET_LISTENER_STAGE_TCP_REUSEADDR:
        return "TCP SO_REUSEADDR";
    case WD_NET_LISTENER_STAGE_TCP_BIND:
        return "TCP bind";
    case WD_NET_LISTENER_STAGE_TCP_LISTEN:
        return "TCP listen";
    case WD_NET_LISTENER_STAGE_TCP_GETSOCKNAME:
        return "TCP getsockname";
    case WD_NET_LISTENER_STAGE_UDP_SOCKET:
        return "UDP socket";
    case WD_NET_LISTENER_STAGE_UDP_BIND:
        return "UDP bind";
    case WD_NET_LISTENER_STAGE_UDP_GETSOCKNAME:
        return "UDP getsockname";
    case WD_NET_LISTENER_STAGE_UDP_NONBLOCK:
        return "UDP nonblocking";
    default:
        return "unknown";
    }
}
