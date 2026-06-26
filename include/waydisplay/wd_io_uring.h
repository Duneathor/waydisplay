#pragma once

#include <liburing.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_io_uring_operation_mask {
    WD_IO_URING_OPERATION_SEND         = 1u << 0,
    WD_IO_URING_OPERATION_SENDMSG      = 1u << 1,
    WD_IO_URING_OPERATION_RECV         = 1u << 2,
    WD_IO_URING_OPERATION_ASYNC_CANCEL = 1u << 3,
};

bool wd_io_uring_require_operations(struct io_uring* ring, uint32_t required_operations, const char* owner);

#ifdef __cplusplus
}
#endif
