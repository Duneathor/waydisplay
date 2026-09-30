#pragma once

#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_io_uring_submit_progress {
    WD_IO_URING_SUBMIT_FAILED = 0,
    WD_IO_URING_SUBMIT_RETRY,
    WD_IO_URING_SUBMIT_ACCEPTED,
};

/* io_uring_submit() reports local submission progress.  A zero result and
 * transient enter errors leave prepared SQEs valid for a later retry. */
static inline enum wd_io_uring_submit_progress wd_io_uring_submit_result(int result) {
    if (result > 0)
    {
        return WD_IO_URING_SUBMIT_ACCEPTED;
    }
    if (result == 0 || result == -EINTR || result == -EAGAIN || result == -EBUSY)
    {
        return WD_IO_URING_SUBMIT_RETRY;
    }
    return WD_IO_URING_SUBMIT_FAILED;
}

#ifdef __cplusplus
}
#endif
