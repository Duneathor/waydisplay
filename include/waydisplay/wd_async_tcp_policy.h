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
