#pragma once

#include "waydisplay/wd_buffer.h"

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_async_udp_sender;
typedef void (*wd_async_udp_completion_fn)(void* user_data, bool success);

enum wd_async_udp_send_status {
    WD_ASYNC_UDP_SEND_FAILED    = 0,
    WD_ASYNC_UDP_SEND_QUEUED    = 1,
    WD_ASYNC_UDP_SEND_SATURATED = 2,
};

/* A sender is externally serialized; its API is not safe for concurrent
 * callers. send_packet() copies the header but borrows payload storage until
 * completion/cancellation; send_packet_owned() instead retains its wd_buffer.
 * flush() returning false means queued work was not fully submitted; callers
 * must retry/service the sender or transition a failed backend rather than
 * treating false as packet completion. */
bool wd_async_udp_sender_create(struct wd_async_udp_sender** out_sender, uint32_t entries);
void wd_async_udp_sender_destroy(struct wd_async_udp_sender* sender);

void wd_async_udp_sender_reap(struct wd_async_udp_sender* sender);
bool wd_async_udp_sender_flush(struct wd_async_udp_sender* sender);
bool wd_async_udp_sender_drain(struct wd_async_udp_sender* sender);

enum wd_async_udp_send_status wd_async_udp_send_packet(struct wd_async_udp_sender* sender, int fd, const struct sockaddr_in* addr,
                                                       const void* header, uint32_t header_size, const void* payload, uint32_t payload_size,
                                                       wd_async_udp_completion_fn completion, void* completion_data);

/* Retains payload until the send completes or is cancelled. The caller may
 * release its reference immediately after a queued result. */
enum wd_async_udp_send_status wd_async_udp_send_packet_owned(
    struct wd_async_udp_sender* sender, int fd, const struct sockaddr_in* addr,
    const void* header, uint32_t header_size, struct wd_buffer* payload,
    size_t payload_offset, uint32_t payload_size,
    wd_async_udp_completion_fn completion, void* completion_data);

uint64_t wd_async_udp_sender_inflight(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_queued(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_completed(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_failed(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_sqe_exhaustions(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_inflight_max(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_take_inflight_max(struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_pending_packets(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_pending_bytes(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_saturation_count(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_submit_calls(const struct wd_async_udp_sender* sender);
uint64_t wd_async_udp_sender_partial_submits(const struct wd_async_udp_sender* sender);

#ifdef __cplusplus
}
#endif
