#include "waydisplay/wd_io_uring.h"

#include "waydisplay/wd_log.h"

#include <stdlib.h>

struct wd_io_uring_operation {
    uint32_t    mask;
    uint8_t     opcode;
    const char* name;
};

static const struct wd_io_uring_operation wd_io_uring_operations[] = {
    {WD_IO_URING_OPERATION_SEND, IORING_OP_SEND, "send"},
    {WD_IO_URING_OPERATION_SENDMSG, IORING_OP_SENDMSG, "sendmsg"},
    {WD_IO_URING_OPERATION_RECV, IORING_OP_RECV, "recv"},
    {WD_IO_URING_OPERATION_ASYNC_CANCEL, IORING_OP_ASYNC_CANCEL, "async_cancel"},
};

bool wd_io_uring_require_operations(struct io_uring* ring, uint32_t required_operations, const char* owner) {
    (void)owner;

    if (!ring || required_operations == 0)
    {
        return false;
    }

    struct io_uring_probe* probe = io_uring_get_probe_ring(ring);
    if (!probe)
    {
        WD_LOG_ERROR("%s: failed to query io_uring opcode support", owner ? owner : "io_uring");
        return false;
    }

    bool supported = true;
    for (size_t i = 0; i < sizeof(wd_io_uring_operations) / sizeof(wd_io_uring_operations[0]); ++i)
    {
        const struct wd_io_uring_operation* operation = &wd_io_uring_operations[i];
        if ((required_operations & operation->mask) != 0 && !io_uring_opcode_supported(probe, operation->opcode))
        {
            WD_LOG_ERROR("%s: required Linux 5.14-era io_uring operation is unavailable: %s", owner ? owner : "io_uring",
                         operation->name);
            supported = false;
        }
    }

    io_uring_free_probe(probe);
    return supported;
}
