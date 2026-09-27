#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared wire-byte and completion policy for both TCP senders. A zero limit
 * means unlimited; subtraction avoids wrapping the pending-byte counter. */
static inline bool wd_async_tcp_can_enqueue(uint64_t pending, uint64_t next, uint64_t limit) {
    return next <= UINT64_MAX - pending &&
           (limit == 0 || (next <= limit && pending <= limit - next));
}

struct wd_async_tcp_owned_send_plan {
    size_t inline_offset;
    size_t inline_size;
    size_t payload_offset;
    size_t payload_size;
};

/* Build the byte ranges for one owned-message send submission. If any inline
 * bytes remain, include the retained payload in the same plan so crossing the
 * inline/payload boundary never depends on observing an intermediate CQE.
 * Genuine short sends are represented by bytes_sent and may require a later
 * resubmission. */
static inline bool wd_async_tcp_plan_owned_send(size_t inline_size, size_t payload_size,
                                                size_t bytes_sent,
                                                struct wd_async_tcp_owned_send_plan* plan) {
    if (!plan || inline_size > SIZE_MAX - payload_size)
    {
        return false;
    }

    plan->inline_offset  = 0;
    plan->inline_size    = 0;
    plan->payload_offset = 0;
    plan->payload_size   = 0;

    const size_t total_size = inline_size + payload_size;
    if (bytes_sent >= total_size)
    {
        return false;
    }

    if (bytes_sent < inline_size)
    {
        plan->inline_offset = bytes_sent;
        plan->inline_size   = inline_size - bytes_sent;
        plan->payload_size  = payload_size;
        return plan->inline_size != 0 || plan->payload_size != 0;
    }

    plan->payload_offset = bytes_sent - inline_size;
    if (plan->payload_offset >= payload_size)
    {
        return false;
    }
    plan->payload_size = payload_size - plan->payload_offset;
    return plan->payload_size != 0;
}

enum wd_async_tcp_send_progress {
    WD_ASYNC_TCP_SEND_FAILED,
    WD_ASYNC_TCP_SEND_PARTIAL,
    WD_ASYNC_TCP_SEND_COMPLETE,
};

/* Never advance beyond the retained message buffer, even for a bad CQE. */
static inline enum wd_async_tcp_send_progress wd_async_tcp_advance(size_t total, size_t* sent, int result) {
    if (!sent || *sent > total || result <= 0 || (size_t)result > total - *sent) {
        return WD_ASYNC_TCP_SEND_FAILED;
    }
    *sent += (size_t)result;
    return *sent == total ? WD_ASYNC_TCP_SEND_COMPLETE : WD_ASYNC_TCP_SEND_PARTIAL;
}

#ifdef __cplusplus
}
#endif
