#include "wd_async_udp_accounting.h"

#include <limits.h>

void wd_async_udp_accounting_queue(struct wd_async_udp_accounting* accounting) {
    if (!accounting || accounting->prepared == UINT32_MAX)
    {
        return;
    }
    accounting->prepared++;
    accounting->queued_total++;
}

uint32_t wd_async_udp_accounting_submit_result(struct wd_async_udp_accounting* accounting, int submit_result) {
    if (!accounting)
    {
        return 0;
    }
    accounting->submit_calls++;
    if (submit_result < 0)
    {
        accounting->submit_failures++;
        return 0;
    }
    if (submit_result == 0 || accounting->prepared == 0)
    {
        return 0;
    }

    uint32_t submitted = (uint32_t)submit_result;
    if (submitted > accounting->prepared)
    {
        submitted = accounting->prepared;
    }
    accounting->prepared -= submitted;
    accounting->submitted += submitted;
    return submitted;
}

bool wd_async_udp_accounting_complete(struct wd_async_udp_accounting* accounting, bool success) {
    if (!accounting || accounting->submitted == 0)
    {
        return false;
    }
    accounting->submitted--;
    if (success)
    {
        accounting->completed_total++;
    }
    else
    {
        accounting->failed_total++;
    }
    return true;
}

uint32_t wd_async_udp_accounting_cancel_prepared(struct wd_async_udp_accounting* accounting) {
    if (!accounting)
    {
        return 0;
    }
    const uint32_t cancelled = accounting->prepared;
    accounting->prepared     = 0;
    accounting->cancelled_total += cancelled;
    return cancelled;
}

uint32_t wd_async_udp_accounting_pending(const struct wd_async_udp_accounting* accounting) {
    return accounting ? accounting->prepared + accounting->submitted : 0;
}

bool wd_async_udp_pending_within_limits(uint64_t pending_packets, uint64_t pending_bytes, uint64_t next_packet_bytes,
                                        uint64_t max_pending_packets, uint64_t max_pending_bytes) {
    if (next_packet_bytes == 0 || max_pending_packets == 0 || max_pending_bytes == 0)
    {
        return false;
    }
    if (pending_packets >= max_pending_packets || next_packet_bytes > max_pending_bytes)
    {
        return false;
    }
    return pending_bytes <= max_pending_bytes - next_packet_bytes;
}

bool wd_stream_epoch_identity_equal(const struct wd_stream_epoch_identity* lhs, const struct wd_stream_epoch_identity* rhs) {
    return lhs && rhs && lhs->connection_epoch == rhs->connection_epoch && lhs->config_epoch == rhs->config_epoch &&
           lhs->content_epoch == rhs->content_epoch && lhs->framebuffer_generation == rhs->framebuffer_generation;
}
