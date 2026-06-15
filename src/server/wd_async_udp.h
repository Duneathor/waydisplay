#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_async_udp_sender;

bool wd_async_udp_sender_create(struct wd_async_udp_sender** out_sender, uint32_t entries);
void wd_async_udp_sender_destroy(struct wd_async_udp_sender* sender);

void wd_async_udp_sender_reap(struct wd_async_udp_sender* sender);
bool wd_async_udp_sender_flush(struct wd_async_udp_sender* sender);
bool wd_async_udp_sender_drain(struct wd_async_udp_sender* sender);

bool wd_async_udp_send_packet(struct wd_async_udp_sender* sender, int fd, const struct sockaddr_in* addr, const void* header,
                              uint32_t header_size, const void* payload, uint32_t payload_size);

uint64_t wd_async_udp_sender_inflight(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_queued(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_completed(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_failed(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_fallbacks(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_inflight_max(const struct wd_async_udp_sender* sender);

#ifdef __cplusplus
}
#endif
