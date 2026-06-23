#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_async_udp_accounting {
    uint32_t prepared;
    uint32_t submitted;
    uint64_t queued_total;
    uint64_t submit_calls;
    uint64_t submit_failures;
    uint64_t completed_total;
    uint64_t failed_total;
    uint64_t cancelled_total;
};

void     wd_async_udp_accounting_queue(struct wd_async_udp_accounting* accounting);
uint32_t wd_async_udp_accounting_submit_result(struct wd_async_udp_accounting* accounting, int submit_result);
bool     wd_async_udp_accounting_complete(struct wd_async_udp_accounting* accounting, bool success);
uint32_t wd_async_udp_accounting_cancel_prepared(struct wd_async_udp_accounting* accounting);
uint32_t wd_async_udp_accounting_pending(const struct wd_async_udp_accounting* accounting);
bool     wd_async_udp_pending_within_limits(uint64_t pending_packets, uint64_t pending_bytes, uint64_t next_packet_bytes,
                                            uint64_t max_pending_packets, uint64_t max_pending_bytes);

struct wd_stream_epoch_identity {
    uint64_t connection_epoch;
    uint64_t config_epoch;
    uint64_t content_epoch;
    uint64_t framebuffer_generation;
};

bool wd_stream_epoch_identity_equal(const struct wd_stream_epoch_identity* lhs, const struct wd_stream_epoch_identity* rhs);

#ifdef __cplusplus
}
#endif
