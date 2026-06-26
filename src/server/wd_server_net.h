#pragma once

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_server;

bool  wd_net_init(struct wd_server* server, uint16_t tcp_port, struct in_addr listen_address);
bool  wd_net_wait_until_ready(struct wd_server* server);
void  wd_net_destroy(struct wd_server* server);
void* wd_net_thread_main(void* arg);

/* Called by network threads after releasing the network-state lock. */
void wd_server_wake_input(struct wd_server* server);

#ifdef __cplusplus
}
#endif
