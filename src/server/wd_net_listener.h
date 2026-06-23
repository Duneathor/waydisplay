#pragma once

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_net_listener_stage {
    WD_NET_LISTENER_STAGE_NONE = 0,
    WD_NET_LISTENER_STAGE_TCP_SOCKET,
    WD_NET_LISTENER_STAGE_TCP_REUSEADDR,
    WD_NET_LISTENER_STAGE_TCP_BIND,
    WD_NET_LISTENER_STAGE_TCP_LISTEN,
    WD_NET_LISTENER_STAGE_TCP_GETSOCKNAME,
    WD_NET_LISTENER_STAGE_UDP_SOCKET,
    WD_NET_LISTENER_STAGE_UDP_BIND,
    WD_NET_LISTENER_STAGE_UDP_GETSOCKNAME,
    WD_NET_LISTENER_STAGE_UDP_NONBLOCK,
};

struct wd_net_listener {
    int      listen_fd;
    int      udp_fd;
    uint16_t tcp_port;
    uint16_t udp_port;
};

void        wd_net_listener_init(struct wd_net_listener* listener);
bool        wd_net_listener_open(struct wd_net_listener* listener, uint16_t requested_tcp_port, const struct in_addr* bind_address,
                                 enum wd_net_listener_stage* failed_stage, int* error_code);
void        wd_net_listener_close(struct wd_net_listener* listener);
const char* wd_net_listener_stage_name(enum wd_net_listener_stage stage);

#ifdef __cplusplus
}
#endif
