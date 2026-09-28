#pragma once

#include <errno.h>
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

/* Coalescible traffic may replace bytes already queued for the same logical
 * update. Evaluate capacity against the post-replacement queue rather than
 * rejecting the newer state merely because the stale copy still occupies
 * the accounting snapshot. */
static inline bool wd_async_tcp_can_enqueue_after_replacing(uint64_t pending, uint64_t replaced,
                                                            uint64_t next, uint64_t limit) {
    return replaced <= pending && wd_async_tcp_can_enqueue(pending - replaced, next, limit);
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

enum wd_async_tcp_submit_progress {
    WD_ASYNC_TCP_SUBMIT_FAILED,
    WD_ASYNC_TCP_SUBMIT_RETRY,
    WD_ASYNC_TCP_SUBMIT_ACCEPTED,
};

/* io_uring_submit() reports submission progress, not socket-send progress.
 * A zero result or a transient enter error leaves the prepared SQE eligible
 * for a later submit attempt and must not poison the TCP connection. */
static inline enum wd_async_tcp_submit_progress wd_async_tcp_submit_result(int result) {
    if (result > 0) {
        return WD_ASYNC_TCP_SUBMIT_ACCEPTED;
    }
    if (result == 0 || result == -EINTR || result == -EAGAIN || result == -EBUSY) {
        return WD_ASYNC_TCP_SUBMIT_RETRY;
    }
    return WD_ASYNC_TCP_SUBMIT_FAILED;
}

enum wd_async_tcp_send_progress {
    WD_ASYNC_TCP_SEND_FAILED,
    WD_ASYNC_TCP_SEND_PARTIAL,
    WD_ASYNC_TCP_SEND_COMPLETE,
};

/* Some kernels/socket combinations can reject an io_uring send operation even
 * though the pinned socket is still valid for a normal nonblocking send.
 * Validate these ring-side errors through the pinned syscall path before
 * declaring the peer dead. */
static inline bool wd_async_tcp_cqe_should_try_syscall(int result) {
    return result == -EOPNOTSUPP || result == -EBADF;
}

/* Never advance beyond the retained message buffer, even for a bad CQE. */
static inline enum wd_async_tcp_send_progress wd_async_tcp_advance(size_t total, size_t* sent, int result) {
    if (!sent || *sent > total) {
        return WD_ASYNC_TCP_SEND_FAILED;
    }
    if (result == -EINTR || result == -EAGAIN) {
        return WD_ASYNC_TCP_SEND_PARTIAL;
    }
    if (result <= 0 || (size_t)result > total - *sent) {
        return WD_ASYNC_TCP_SEND_FAILED;
    }
    *sent += (size_t)result;
    return *sent == total ? WD_ASYNC_TCP_SEND_COMPLETE : WD_ASYNC_TCP_SEND_PARTIAL;
}

#ifdef __cplusplus
}
#endif
