#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_async_tcp_sender;

typedef void (*wd_async_tcp_complete_fn)(void* user_data, bool success);

bool wd_async_tcp_sender_create(struct wd_async_tcp_sender** out_sender, uint32_t entries);
void wd_async_tcp_sender_destroy(struct wd_async_tcp_sender* sender);

void wd_async_tcp_sender_reap(struct wd_async_tcp_sender* sender);

bool wd_async_tcp_send_message(struct wd_async_tcp_sender* sender, int fd, uint16_t message_type, const void* payload,
                               uint32_t payload_size);
bool wd_async_tcp_send_message_ex(struct wd_async_tcp_sender* sender, int fd, uint16_t message_type, const void* payload,
                                  uint32_t payload_size, wd_async_tcp_complete_fn complete, void* user_data);

bool wd_async_tcp_sender_has_message_type(const struct wd_async_tcp_sender* sender, uint16_t message_type);
bool wd_async_tcp_sender_can_queue(const struct wd_async_tcp_sender* sender, uint32_t payload_size);
uint32_t wd_async_tcp_sender_drop_message_type(struct wd_async_tcp_sender* sender, uint16_t message_type);
void wd_async_tcp_sender_set_max_pending_bytes(struct wd_async_tcp_sender* sender, uint64_t max_pending_bytes);

uint64_t wd_async_tcp_sender_inflight(const struct wd_async_tcp_sender* sender);
uint64_t wd_async_tcp_sender_pending_bytes(const struct wd_async_tcp_sender* sender);
uint64_t wd_async_tcp_sender_queued(const struct wd_async_tcp_sender* sender);
uint64_t wd_async_tcp_sender_completed(const struct wd_async_tcp_sender* sender);
uint64_t wd_async_tcp_sender_failed(const struct wd_async_tcp_sender* sender);
uint64_t wd_async_tcp_sender_overflows(const struct wd_async_tcp_sender* sender);
uint64_t wd_async_tcp_sender_partial_resubmits(const struct wd_async_tcp_sender* sender);
uint64_t wd_async_tcp_sender_inflight_max(const struct wd_async_tcp_sender* sender);

#ifdef __cplusplus
}
#endif
