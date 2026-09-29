#include "wd_async_tcp.h"

#include "waydisplay/wd_async_tcp_policy.h"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_io_uring.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_protocol_codec.h"
#include "waydisplay/wd_protocol_dispatch.h"
#include "waydisplay/wd_socket_pin.h"

#include <errno.h>
#include <liburing.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define WD_ASYNC_TCP_DEFAULT_MAX_PENDING_BYTES WD_SERVER_ASYNC_TCP_DEFAULT_PENDING_BYTES
#ifndef WD_ASYNC_TCP_DRAIN_LIMIT
#define WD_ASYNC_TCP_DRAIN_LIMIT WD_ASYNC_SENDER_DRAIN_LIMIT
#endif
#ifndef WD_ASYNC_TCP_DRAIN_SLEEP_US
#define WD_ASYNC_TCP_DRAIN_SLEEP_US WD_ASYNC_SENDER_DRAIN_SLEEP_US
#endif

struct wd_async_tcp_sender;
static bool wd_async_tcp_sender_bind_socket(struct wd_async_tcp_sender* sender, int fd);

static char wd_async_tcp_cancel_cqe_tag;
#define WD_ASYNC_TCP_CANCEL_CQE ((void*)&wd_async_tcp_cancel_cqe_tag)

struct wd_async_tcp_message {
    struct wd_async_tcp_message* next;
    struct wd_async_tcp_message* prev;
    int                          fd;
    uint16_t                     message_type;
    size_t                       total_size;
    size_t                       bytes_sent;
    size_t                       inline_size;
    struct wd_buffer*            payload_owner;
    size_t                       payload_offset;
    size_t                       payload_size;
    size_t                       submitted_size;
    struct iovec                 submit_iov[2];
    struct msghdr                submit_msg;
    bool                         submitted;
    wd_async_tcp_complete_fn     complete;
    void*                        user_data;
    uint8_t                      bytes[];
};

struct wd_async_tcp_sender {
    struct io_uring ring;
    bool            ring_ready;
    struct wd_socket_pin socket_pin;

    struct wd_async_tcp_message* pending_head;
    struct wd_async_tcp_message* pending_tail;

    uint64_t inflight;
    uint64_t inflight_max;
    uint64_t pending_bytes;
    uint64_t max_pending_bytes;
    uint64_t queued;
    uint64_t completed;
    uint64_t failed;
    uint64_t overflows;
    uint64_t partial_resubmits;
    bool     submit_retry_pending;
    bool     syscall_fallback;
    uint64_t transport_failures;
    int      last_transport_result;
    int      last_transport_fd;
    uint64_t last_transport_cookie;
    uint16_t last_transport_message_type;
};

static bool wd_async_tcp_sender_available(const struct wd_async_tcp_sender* sender) {
    return sender && (sender->ring_ready || sender->syscall_fallback);
}

static void wd_async_tcp_record_transport_failure(struct wd_async_tcp_sender* sender,
                                                  const struct wd_async_tcp_message* msg, int result) {
    if (!sender)
    {
        return;
    }
    sender->transport_failures++;
    sender->last_transport_result       = result;
    sender->last_transport_fd           = msg ? msg->fd : -1;
    sender->last_transport_cookie       = sender->socket_pin.cookie;
    sender->last_transport_message_type = msg ? msg->message_type : 0;
}

static void wd_async_tcp_complete_message(struct wd_async_tcp_message* msg, bool success) {
    if (msg && msg->complete)
    {
        msg->complete(msg->user_data, success);
    }
}

static void wd_async_tcp_message_destroy(struct wd_async_tcp_message* msg) {
    if (!msg)
    {
        return;
    }
    wd_buffer_release(msg->payload_owner);
    msg->payload_owner = NULL;
    free(msg);
}

static void wd_async_tcp_pending_add(struct wd_async_tcp_sender* sender, struct wd_async_tcp_message* msg) {
    msg->next = NULL;
    msg->prev = sender->pending_tail;
    if (sender->pending_tail)
    {
        sender->pending_tail->next = msg;
    }
    else
    {
        sender->pending_head = msg;
    }
    sender->pending_tail = msg;
    sender->pending_bytes += msg->total_size;
}

static void wd_async_tcp_pending_remove(struct wd_async_tcp_sender* sender, struct wd_async_tcp_message* msg) {
    if (msg->prev)
    {
        msg->prev->next = msg->next;
    }
    else
    {
        sender->pending_head = msg->next;
    }

    if (msg->next)
    {
        msg->next->prev = msg->prev;
    }
    else
    {
        sender->pending_tail = msg->prev;
    }

    if (sender->pending_bytes >= msg->total_size)
    {
        sender->pending_bytes -= msg->total_size;
    }
    else
    {
        sender->pending_bytes = 0;
    }

    msg->next = NULL;
    msg->prev = NULL;
}

static int wd_async_tcp_syscall_send_once(struct wd_async_tcp_sender* sender,
                                          struct wd_async_tcp_message* msg) {
    if (!sender || !msg || sender->socket_pin.io_fd < 0 || msg->bytes_sent >= msg->total_size)
    {
        return -EINVAL;
    }

    ssize_t result = -1;
    const int flags = MSG_NOSIGNAL | MSG_DONTWAIT;
    if (msg->payload_owner)
    {
        struct wd_async_tcp_owned_send_plan plan = {0};
        if (!wd_async_tcp_plan_owned_send(msg->inline_size, msg->payload_size, msg->bytes_sent, &plan))
        {
            return -EINVAL;
        }

        struct iovec iov[2];
        unsigned iov_count = 0;
        if (plan.inline_size != 0)
        {
            iov[iov_count].iov_base = msg->bytes + plan.inline_offset;
            iov[iov_count].iov_len  = plan.inline_size;
            iov_count++;
        }
        if (plan.payload_size != 0)
        {
            iov[iov_count].iov_base =
                (void*)(wd_buffer_const_data(msg->payload_owner) + msg->payload_offset + plan.payload_offset);
            iov[iov_count].iov_len = plan.payload_size;
            iov_count++;
        }
        if (iov_count == 0)
        {
            return -EINVAL;
        }

        struct msghdr send_msg;
        memset(&send_msg, 0, sizeof(send_msg));
        send_msg.msg_iov    = iov;
        send_msg.msg_iovlen = iov_count;
        result = sendmsg(sender->socket_pin.io_fd, &send_msg, flags);
    }
    else
    {
        result = send(sender->socket_pin.io_fd, msg->bytes + msg->bytes_sent,
                      msg->total_size - msg->bytes_sent, flags);
    }

    if (result < 0)
    {
        return -errno;
    }
    if (result > INT_MAX)
    {
        return -EOVERFLOW;
    }
    return (int)result;
}

static bool wd_async_tcp_progress_syscall_fallback(struct wd_async_tcp_sender* sender) {
    if (!sender || !sender->syscall_fallback || sender->inflight != 0)
    {
        return true;
    }

    /* Bound work per reap so a large control backlog cannot monopolize the
     * server loop after the ring has fallen back to nonblocking syscalls. */
    for (uint32_t i = 0; sender->pending_head && i < 16; ++i)
    {
        struct wd_async_tcp_message* msg = sender->pending_head;
        const int result = wd_async_tcp_syscall_send_once(sender, msg);
        if (result == -EINTR || result == -EAGAIN)
        {
            return true;
        }

        const enum wd_async_tcp_send_progress progress =
            wd_async_tcp_advance(msg->total_size, &msg->bytes_sent, result);
        if (progress == WD_ASYNC_TCP_SEND_FAILED)
        {
            sender->failed++;
            wd_async_tcp_record_transport_failure(sender, msg, result);
            wd_async_tcp_pending_remove(sender, msg);
            wd_async_tcp_complete_message(msg, false);
            wd_async_tcp_message_destroy(msg);
            return false;
        }
        if (progress == WD_ASYNC_TCP_SEND_COMPLETE)
        {
            sender->completed++;
            wd_async_tcp_pending_remove(sender, msg);
            wd_async_tcp_complete_message(msg, true);
            wd_async_tcp_message_destroy(msg);
            continue;
        }

        sender->partial_resubmits++;
        return true;
    }
    return true;
}

static enum wd_async_tcp_submit_progress wd_async_tcp_flush_submit(struct wd_async_tcp_sender* sender, int* out_result) {
    if (!sender || !sender->ring_ready)
    {
        return WD_ASYNC_TCP_SUBMIT_FAILED;
    }

    const int rc = io_uring_submit(&sender->ring);
    if (out_result)
    {
        *out_result = rc;
    }
    const enum wd_async_tcp_submit_progress progress = wd_async_tcp_submit_result(rc);
    sender->submit_retry_pending = progress == WD_ASYNC_TCP_SUBMIT_RETRY;
    return progress;
}

static void wd_async_tcp_retire_ring(struct wd_async_tcp_sender* sender) {
    if (!sender || !sender->ring_ready)
    {
        return;
    }
    io_uring_queue_exit(&sender->ring);
    sender->ring_ready = false;
    sender->submit_retry_pending = false;
}

static bool wd_async_tcp_submit_message(struct wd_async_tcp_sender* sender, struct wd_async_tcp_message* msg) {
    if (!sender || !sender->ring_ready || !msg || msg->submitted || msg->fd < 0 || msg->bytes_sent >= msg->total_size)
    {
        return false;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&sender->ring);
    if (!sqe)
    {
        WD_LOG_WARN("server io_uring TCP SQ unavailable for fd=%d message_type=%u; switching to nonblocking syscall fallback",
                    msg->fd, msg->message_type);
        wd_async_tcp_retire_ring(sender);
        sender->syscall_fallback = true;
        return true;
    }

    size_t send_size = 0;
    if (msg->payload_owner)
    {
        /*
         * An owned message has two logical segments: the copied protocol
         * header/prefix and the retained payload slice. Submit both in one
         * sendmsg SQE so the segment boundary itself does not require a CQE
         * reap before the payload can make progress.
         *
         * On a genuine partial kernel send, bytes_sent is advanced by the CQE
         * result and this iovec is rebuilt from that global byte offset.
         */
        struct wd_async_tcp_owned_send_plan plan = {0};
        if (!wd_async_tcp_plan_owned_send(msg->inline_size, msg->payload_size,
                                          msg->bytes_sent, &plan))
        {
            return false;
        }

        unsigned iov_count = 0;
        memset(&msg->submit_msg, 0, sizeof(msg->submit_msg));

        if (plan.inline_size != 0)
        {
            msg->submit_iov[iov_count].iov_base = msg->bytes + plan.inline_offset;
            msg->submit_iov[iov_count].iov_len  = plan.inline_size;
            send_size += plan.inline_size;
            iov_count++;
        }
        if (plan.payload_size != 0)
        {
            msg->submit_iov[iov_count].iov_base =
                (void*)(wd_buffer_const_data(msg->payload_owner) + msg->payload_offset +
                        plan.payload_offset);
            msg->submit_iov[iov_count].iov_len = plan.payload_size;
            send_size += plan.payload_size;
            iov_count++;
        }

        if (iov_count == 0 || send_size == 0)
        {
            return false;
        }

        msg->submit_msg.msg_iov    = msg->submit_iov;
        msg->submit_msg.msg_iovlen = iov_count;
        io_uring_prep_sendmsg(sqe, sender->socket_pin.io_fd, &msg->submit_msg, MSG_NOSIGNAL);
    }
    else
    {
        const uint8_t* send_data = msg->bytes + msg->bytes_sent;
        send_size                = msg->total_size - msg->bytes_sent;
        if (send_size == 0)
        {
            return false;
        }
        io_uring_prep_send(sqe, sender->socket_pin.io_fd, send_data, send_size, MSG_NOSIGNAL);
    }
    io_uring_sqe_set_data(sqe, msg);

    msg->submitted      = true;
    msg->submitted_size = send_size;
    sender->inflight++;
    if (sender->inflight > sender->inflight_max)
    {
        sender->inflight_max = sender->inflight;
    }

    int submit_result = 0;
    const enum wd_async_tcp_submit_progress submit_progress = wd_async_tcp_flush_submit(sender, &submit_result);
    if (submit_progress == WD_ASYNC_TCP_SUBMIT_FAILED)
    {
        /* io_uring_submit() is a local backend operation. A fatal submit
         * result does not prove that the socket peer failed, so retire the
         * ring and preserve the queued message for the syscall fallback. */
        WD_LOG_WARN("server io_uring TCP submit failed: result=%d fd=%d message_type=%u; switching to nonblocking syscall fallback",
                    submit_result, msg->fd, msg->message_type);
        wd_async_tcp_retire_ring(sender);
        msg->submitted = false;
        if (sender->inflight > 0)
        {
            sender->inflight--;
        }
        sender->syscall_fallback = true;
        return true;
    }

    return true;
}

static bool wd_async_tcp_try_start_head(struct wd_async_tcp_sender* sender, struct wd_async_tcp_message* suppress_completion,
                                        bool* suppressed_message_failed) {
    if (suppressed_message_failed)
    {
        *suppressed_message_failed = false;
    }

    if (!sender || sender->inflight != 0 || !sender->pending_head)
    {
        return true;
    }

    if (sender->syscall_fallback)
    {
        return wd_async_tcp_progress_syscall_fallback(sender);
    }

    if (!wd_async_tcp_submit_message(sender, sender->pending_head))
    {
        struct wd_async_tcp_message* failed_msg = sender->pending_head;
        bool                         suppressed = failed_msg == suppress_completion;
        sender->failed++;
        wd_async_tcp_pending_remove(sender, failed_msg);
        if (!suppressed)
        {
            wd_async_tcp_complete_message(failed_msg, false);
        }
        wd_async_tcp_message_destroy(failed_msg);
        if (suppressed_message_failed)
        {
            *suppressed_message_failed = suppressed;
        }
        return false;
    }

    return true;
}

static struct wd_async_tcp_message* wd_async_tcp_message_create(int fd, uint16_t message_type, const void* payload, uint32_t payload_size,
                                                                wd_async_tcp_complete_fn complete, void* user_data) {
    uint32_t wire_payload_size = 0;
    if (!wd_protocol_payload_wire_size(message_type, payload, payload_size, &wire_payload_size))
    {
        return NULL;
    }

    const size_t                 total_size = WD_TCP_HEADER_WIRE_SIZE + (size_t)wire_payload_size;
    struct wd_async_tcp_message* msg        = calloc(1, sizeof(*msg) + total_size);
    if (!msg)
    {
        return NULL;
    }

    struct wd_tcp_header header;
    memset(&header, 0, sizeof(header));
    header.magic            = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = message_type;
    header.payload_size     = wire_payload_size;

    msg->fd           = fd;
    msg->message_type = message_type;
    msg->total_size   = total_size;
    msg->inline_size  = total_size;
    msg->complete     = complete;
    msg->user_data    = user_data;
    if (!wd_tcp_header_encode(msg->bytes, &header))
    {
        wd_async_tcp_message_destroy(msg);
        return NULL;
    }
    if (wire_payload_size != 0)
    {
        memcpy(msg->bytes + WD_TCP_HEADER_WIRE_SIZE, payload, wire_payload_size);
    }

    return msg;
}

struct wd_async_tcp_message* wd_async_tcp_prepare_message(uint16_t message_type, uint32_t payload_size,
                                                          void** out_payload) {
    if (!out_payload)
    {
        return NULL;
    }
    *out_payload = NULL;
    const uint64_t total_size64 = (uint64_t)WD_TCP_HEADER_WIRE_SIZE + payload_size;
    if (total_size64 > SIZE_MAX - sizeof(struct wd_async_tcp_message))
    {
        return NULL;
    }
    const size_t total_size = (size_t)total_size64;
    struct wd_async_tcp_message* msg = calloc(1, sizeof(*msg) + total_size);
    if (!msg)
    {
        return NULL;
    }
    msg->fd           = -1;
    msg->message_type = message_type;
    msg->total_size   = total_size;
    msg->inline_size  = total_size;
    *out_payload      = msg->bytes + WD_TCP_HEADER_WIRE_SIZE;
    return msg;
}

void wd_async_tcp_discard_prepared_message(struct wd_async_tcp_message* msg) {
    wd_async_tcp_message_destroy(msg);
}

/* The allocated bytes are already in their final io_uring-owned storage.
 * Validate the populated payload before encoding the TCP header and handing
 * the message to the normal completion/cancellation queue. */
bool wd_async_tcp_send_prepared_message(struct wd_async_tcp_sender* sender, int fd,
                                        struct wd_async_tcp_message* msg) {
    if (!msg)
    {
        return false;
    }
    if (!wd_async_tcp_sender_available(sender) || fd < 0)
    {
        wd_async_tcp_message_destroy(msg);
        return false;
    }

    wd_async_tcp_sender_reap(sender);
    if (!wd_async_tcp_sender_bind_socket(sender, fd))
    {
        sender->failed++;
        wd_async_tcp_message_destroy(msg);
        return false;
    }
    const uint32_t payload_size = (uint32_t)(msg->total_size - WD_TCP_HEADER_WIRE_SIZE);
    const void* payload = msg->bytes + WD_TCP_HEADER_WIRE_SIZE;
    uint32_t wire_size = 0;
    if (!wd_protocol_payload_wire_size(msg->message_type, payload, payload_size, &wire_size) || wire_size != payload_size)
    {
        sender->failed++;
        wd_async_tcp_message_destroy(msg);
        return false;
    }
    if (!wd_async_tcp_can_enqueue(sender->pending_bytes, msg->total_size, sender->max_pending_bytes))
    {
        sender->overflows++;
        sender->failed++;
        wd_async_tcp_message_destroy(msg);
        return false;
    }
    struct wd_tcp_header header = {0};
    header.magic            = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = msg->message_type;
    header.payload_size     = payload_size;
    if (!wd_tcp_header_encode(msg->bytes, &header))
    {
        sender->failed++;
        wd_async_tcp_message_destroy(msg);
        return false;
    }
    msg->fd = fd;
    wd_async_tcp_pending_add(sender, msg);
    bool just_enqueued_failed = false;
    if (!wd_async_tcp_try_start_head(sender, msg, &just_enqueued_failed) && just_enqueued_failed)
    {
        return false;
    }
    sender->queued++;
    return true;
}

static struct wd_async_tcp_message* wd_async_tcp_owned_message_create(
    int fd, uint16_t message_type, const void* prefix, uint32_t prefix_size,
    struct wd_buffer* payload, size_t payload_offset, uint32_t payload_size,
    wd_async_tcp_complete_fn complete, void* user_data) {
    if ((prefix_size != 0 && !prefix) || (payload_size != 0 && !payload) ||
        (payload && !wd_buffer_range_valid(payload, payload_offset, payload_size)))
    {
        return NULL;
    }

    const uint64_t wire_payload_size64 = (uint64_t)prefix_size + (uint64_t)payload_size;
    if (wire_payload_size64 > UINT32_MAX ||
        !wd_protocol_payload_size_is_valid(message_type, (uint32_t)wire_payload_size64))
    {
        return NULL;
    }
    const size_t inline_size = WD_TCP_HEADER_WIRE_SIZE + (size_t)prefix_size;
    if (inline_size > SIZE_MAX - sizeof(struct wd_async_tcp_message) ||
        inline_size > SIZE_MAX - (size_t)payload_size)
    {
        return NULL;
    }

    struct wd_async_tcp_message* msg = calloc(1, sizeof(*msg) + inline_size);
    if (!msg)
    {
        return NULL;
    }

    struct wd_buffer* retained = payload ? wd_buffer_retain(payload) : NULL;
    if (payload && !retained)
    {
        wd_async_tcp_message_destroy(msg);
        return NULL;
    }

    struct wd_tcp_header header = {0};
    header.magic            = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = message_type;
    header.payload_size     = (uint32_t)wire_payload_size64;
    if (!wd_tcp_header_encode(msg->bytes, &header))
    {
        wd_buffer_release(retained);
        wd_async_tcp_message_destroy(msg);
        return NULL;
    }
    if (prefix_size != 0)
    {
        memcpy(msg->bytes + WD_TCP_HEADER_WIRE_SIZE, prefix, prefix_size);
    }

    msg->fd             = fd;
    msg->message_type   = message_type;
    msg->inline_size    = inline_size;
    msg->payload_owner  = retained;
    msg->payload_offset = payload_offset;
    msg->payload_size   = payload_size;
    msg->total_size     = inline_size + (size_t)payload_size;
    msg->complete       = complete;
    msg->user_data      = user_data;
    return msg;
}

bool wd_async_tcp_send_owned_message_ex(struct wd_async_tcp_sender* sender, int fd, uint16_t message_type,
                                        const void* prefix, uint32_t prefix_size, struct wd_buffer* payload,
                                        size_t payload_offset, uint32_t payload_size,
                                        wd_async_tcp_complete_fn complete, void* user_data) {
    if (!wd_async_tcp_sender_available(sender) || fd < 0)
    {
        return false;
    }
    wd_async_tcp_sender_reap(sender);
    if (!wd_async_tcp_sender_bind_socket(sender, fd))
    {
        sender->failed++;
        return false;
    }

    struct wd_async_tcp_message* msg =
        wd_async_tcp_owned_message_create(fd, message_type, prefix, prefix_size, payload,
                                          payload_offset, payload_size, complete, user_data);
    if (!msg)
    {
        sender->failed++;
        return false;
    }
    if (!wd_async_tcp_can_enqueue(sender->pending_bytes, msg->total_size, sender->max_pending_bytes))
    {
        sender->overflows++;
        sender->failed++;
        wd_async_tcp_message_destroy(msg);
        return false;
    }

    wd_async_tcp_pending_add(sender, msg);
    bool just_enqueued_failed = false;
    if (!wd_async_tcp_try_start_head(sender, msg, &just_enqueued_failed) && just_enqueued_failed)
    {
        return false;
    }
    sender->queued++;
    return true;
}

bool wd_async_tcp_send_owned_message(struct wd_async_tcp_sender* sender, int fd, uint16_t message_type,
                                     const void* prefix, uint32_t prefix_size, struct wd_buffer* payload,
                                     size_t payload_offset, uint32_t payload_size) {
    return wd_async_tcp_send_owned_message_ex(sender, fd, message_type, prefix, prefix_size, payload,
                                              payload_offset, payload_size, NULL, NULL);
}

bool wd_async_tcp_sender_create(struct wd_async_tcp_sender** out_sender, uint32_t entries) {
    if (!out_sender)
    {
        return false;
    }

    *out_sender = NULL;

    if (entries < WD_ASYNC_MIN_RING_ENTRIES)
    {
        entries = WD_ASYNC_MIN_RING_ENTRIES;
    }

    struct wd_async_tcp_sender* sender = calloc(1, sizeof(*sender));
    if (!sender)
    {
        return false;
    }

    int rc = io_uring_queue_init(entries, &sender->ring, 0);
    if (rc < 0)
    {
        free(sender);
        return false;
    }
    if (!wd_io_uring_require_operations(&sender->ring,
                                        WD_IO_URING_OPERATION_SEND | WD_IO_URING_OPERATION_SENDMSG |
                                            WD_IO_URING_OPERATION_ASYNC_CANCEL,
                                        "server TCP sender"))
    {
        io_uring_queue_exit(&sender->ring);
        free(sender);
        return false;
    }

    sender->ring_ready        = true;
    sender->socket_pin        = (struct wd_socket_pin)WD_SOCKET_PIN_INITIALIZER;
    sender->max_pending_bytes = WD_ASYNC_TCP_DEFAULT_MAX_PENDING_BYTES;
    *out_sender               = sender;
    return true;
}

void wd_async_tcp_sender_reap(struct wd_async_tcp_sender* sender) {
    if (!sender)
    {
        return;
    }

    if (sender->syscall_fallback && sender->inflight == 0)
    {
        (void)wd_async_tcp_progress_syscall_fallback(sender);
        return;
    }

    if (!sender->ring_ready)
    {
        return;
    }

    if (sender->submit_retry_pending)
    {
        int submit_result = 0;
        const enum wd_async_tcp_submit_progress submit_progress = wd_async_tcp_flush_submit(sender, &submit_result);
        if (submit_progress == WD_ASYNC_TCP_SUBMIT_RETRY)
        {
            return;
        }
        if (submit_progress == WD_ASYNC_TCP_SUBMIT_FAILED)
        {
            struct wd_async_tcp_message* pending_msg = sender->pending_head;
            WD_LOG_WARN("server io_uring TCP resubmit failed: result=%d fd=%d message_type=%u; switching to nonblocking syscall fallback",
                        submit_result, pending_msg ? pending_msg->fd : -1,
                        pending_msg ? pending_msg->message_type : 0);
            wd_async_tcp_retire_ring(sender);
            if (pending_msg)
            {
                pending_msg->submitted = false;
            }
            sender->inflight = 0;
            sender->syscall_fallback = true;
            (void)wd_async_tcp_progress_syscall_fallback(sender);
            return;
        }
    }

    struct io_uring_cqe* cqe = NULL;
    while (io_uring_peek_cqe(&sender->ring, &cqe) == 0 && cqe)
    {
        void* cqe_data = io_uring_cqe_get_data(cqe);
        if (cqe_data == WD_ASYNC_TCP_CANCEL_CQE)
        {
            io_uring_cqe_seen(&sender->ring, cqe);
            cqe = NULL;
            continue;
        }

        struct wd_async_tcp_message* msg = cqe_data;
        size_t                       submitted_size = 0;

        if (msg && msg->submitted)
        {
            submitted_size  = msg->submitted_size;
            msg->submitted = false;
            msg->submitted_size = 0;
            if (sender->inflight > 0)
            {
                sender->inflight--;
            }
        }

        if (msg && wd_async_tcp_cqe_should_try_syscall(cqe->res))
        {
            if (!sender->syscall_fallback)
            {
                WD_LOG_WARN("io_uring TCP send result=%d for fd=%d message_type=%u; validating with nonblocking syscall fallback",
                            cqe->res, msg->fd, msg->message_type);
            }
            sender->syscall_fallback = true;
            io_uring_cqe_seen(&sender->ring, cqe);
            cqe = NULL;
            (void)wd_async_tcp_progress_syscall_fallback(sender);
            continue;
        }

        if (!msg)
        {
            sender->failed++;
        }
        else if (wd_async_tcp_advance(msg->total_size, &msg->bytes_sent, cqe->res) == WD_ASYNC_TCP_SEND_FAILED)
        {
            sender->failed++;
            wd_async_tcp_record_transport_failure(sender, msg, cqe->res);
            wd_async_tcp_pending_remove(sender, msg);
            wd_async_tcp_complete_message(msg, false);
            wd_async_tcp_message_destroy(msg);
            wd_async_tcp_try_start_head(sender, NULL, NULL);
        }
        else
        {
            if (msg->bytes_sent == msg->total_size)
            {
                sender->completed++;
                wd_async_tcp_pending_remove(sender, msg);
                wd_async_tcp_complete_message(msg, true);
                wd_async_tcp_message_destroy(msg);
                wd_async_tcp_try_start_head(sender, NULL, NULL);
            }
            else
            {
                if (cqe->res >= 0 && (size_t)cqe->res < submitted_size)
                {
                    sender->partial_resubmits++;
                }
                if (!wd_async_tcp_submit_message(sender, msg))
                {
                    sender->failed++;
                    wd_async_tcp_pending_remove(sender, msg);
                    wd_async_tcp_complete_message(msg, false);
                    wd_async_tcp_message_destroy(msg);
                    wd_async_tcp_try_start_head(sender, NULL, NULL);
                }
            }
        }

        /* submit_message may retire the ring while handling this CQE.
         * queue_exit consumes the CQ state, so never touch the retired ring. */
        if (!sender->ring_ready)
        {
            cqe = NULL;
            break;
        }
        io_uring_cqe_seen(&sender->ring, cqe);
        cqe = NULL;
    }
}

static bool wd_async_tcp_sender_bind_socket(struct wd_async_tcp_sender* sender, int fd) {
    if (!sender || fd < 0)
    {
        return false;
    }
    if (wd_socket_pin_matches(&sender->socket_pin, fd))
    {
        sender->socket_pin.source_fd = fd;
        return true;
    }
    if (sender->pending_head || sender->inflight != 0)
    {
        return false;
    }
    return wd_socket_pin_bind(&sender->socket_pin, fd);
}

bool wd_async_tcp_send_message_ex(struct wd_async_tcp_sender* sender, int fd, uint16_t message_type, const void* payload,
                                  uint32_t payload_size, wd_async_tcp_complete_fn complete, void* user_data) {
    if (!wd_async_tcp_sender_available(sender) || fd < 0)
    {
        return false;
    }

    wd_async_tcp_sender_reap(sender);
    if (!wd_async_tcp_sender_bind_socket(sender, fd))
    {
        sender->failed++;
        return false;
    }

    uint32_t wire_payload_size = 0;
    if (!wd_protocol_payload_wire_size(message_type, payload, payload_size, &wire_payload_size))
    {
        sender->failed++;
        return false;
    }

    const uint64_t total_size = (uint64_t)WD_TCP_HEADER_WIRE_SIZE + (uint64_t)wire_payload_size;
    if (!wd_async_tcp_can_enqueue(sender->pending_bytes, total_size, sender->max_pending_bytes))
    {
        sender->overflows++;
        sender->failed++;
        return false;
    }

    struct wd_async_tcp_message* msg = wd_async_tcp_message_create(fd, message_type, payload, payload_size, complete, user_data);
    if (!msg)
    {
        sender->failed++;
        return false;
    }

    wd_async_tcp_pending_add(sender, msg);
    bool just_enqueued_failed = false;
    if (!wd_async_tcp_try_start_head(sender, msg, &just_enqueued_failed) && just_enqueued_failed)
    {
        return false;
    }

    sender->queued++;
    return true;
}

bool wd_async_tcp_send_message(struct wd_async_tcp_sender* sender, int fd, uint16_t message_type, const void* payload,
                               uint32_t payload_size) {
    return wd_async_tcp_send_message_ex(sender, fd, message_type, payload, payload_size, NULL, NULL);
}

bool wd_async_tcp_sender_can_queue(const struct wd_async_tcp_sender* sender, uint32_t payload_size) {
    if (!wd_async_tcp_sender_available(sender))
    {
        return false;
    }

    const uint64_t total_size = (uint64_t)WD_TCP_HEADER_WIRE_SIZE + (uint64_t)payload_size;
    return wd_async_tcp_can_enqueue(sender->pending_bytes, total_size, sender->max_pending_bytes);
}

bool wd_async_tcp_sender_has_message_type(const struct wd_async_tcp_sender* sender, uint16_t message_type) {
    if (!sender)
    {
        return false;
    }
    for (const struct wd_async_tcp_message* msg = sender->pending_head; msg; msg = msg->next)
    {
        if (msg->message_type == message_type)
        {
            return true;
        }
    }
    return false;
}

uint32_t wd_async_tcp_sender_drop_message_type(struct wd_async_tcp_sender* sender, uint16_t message_type) {
    if (!sender)
    {
        return 0;
    }

    uint32_t                     dropped = 0;
    struct wd_async_tcp_message* msg     = sender->pending_head;
    while (msg)
    {
        struct wd_async_tcp_message* next = msg->next;
        if (msg->message_type == message_type && !msg->submitted)
        {
            wd_async_tcp_pending_remove(sender, msg);
            wd_async_tcp_complete_message(msg, false);
            wd_async_tcp_message_destroy(msg);
            dropped++;
        }
        msg = next;
    }

    if (dropped != 0)
    {
        wd_async_tcp_try_start_head(sender, NULL, NULL);
    }
    return dropped;
}

void wd_async_tcp_sender_set_max_pending_bytes(struct wd_async_tcp_sender* sender, uint64_t max_pending_bytes) {
    if (sender)
    {
        sender->max_pending_bytes = max_pending_bytes;
    }
}

uint64_t wd_async_tcp_sender_inflight(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->inflight : 0;
}

uint64_t wd_async_tcp_sender_pending_bytes(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->pending_bytes : 0;
}

uint64_t wd_async_tcp_sender_queued(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->queued : 0;
}

uint64_t wd_async_tcp_sender_completed(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->completed : 0;
}

uint64_t wd_async_tcp_sender_failed(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->failed : 0;
}

uint64_t wd_async_tcp_sender_overflows(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->overflows : 0;
}

uint64_t wd_async_tcp_sender_transport_failures(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->transport_failures : 0;
}

int wd_async_tcp_sender_last_transport_result(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->last_transport_result : 0;
}

int wd_async_tcp_sender_last_transport_fd(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->last_transport_fd : -1;
}

bool wd_async_tcp_sender_last_transport_matches_fd(const struct wd_async_tcp_sender* sender, int fd) {
    if (!sender || sender->last_transport_cookie == 0 || fd < 0)
    {
        return false;
    }
    uint64_t cookie = 0;
    return wd_socket_cookie(fd, &cookie) && cookie == sender->last_transport_cookie;
}

uint16_t wd_async_tcp_sender_last_transport_message_type(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->last_transport_message_type : 0;
}

uint64_t wd_async_tcp_sender_partial_resubmits(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->partial_resubmits : 0;
}

uint64_t wd_async_tcp_sender_inflight_max(const struct wd_async_tcp_sender* sender) {
    return sender ? sender->inflight_max : 0;
}

static bool wd_async_tcp_sender_has_submitted(const struct wd_async_tcp_sender* sender) {
    for (const struct wd_async_tcp_message* msg = sender ? sender->pending_head : NULL; msg; msg = msg->next)
    {
        if (msg->submitted)
        {
            return true;
        }
    }
    return false;
}

static void wd_async_tcp_sender_fail_unsubmitted(struct wd_async_tcp_sender* sender) {
    struct wd_async_tcp_message* msg = sender ? sender->pending_head : NULL;
    while (msg)
    {
        struct wd_async_tcp_message* next = msg->next;
        if (!msg->submitted)
        {
            wd_async_tcp_pending_remove(sender, msg);
            wd_async_tcp_complete_message(msg, false);
            wd_async_tcp_message_destroy(msg);
        }
        msg = next;
    }
}

static void wd_async_tcp_sender_shutdown_pending_fds(struct wd_async_tcp_sender* sender) {
    if (sender && sender->pending_head)
    {
        (void)wd_socket_pin_shutdown(&sender->socket_pin);
    }
}

static void wd_async_tcp_sender_request_cancels(struct wd_async_tcp_sender* sender) {
    if (!sender || !sender->ring_ready)
    {
        return;
    }

    for (struct wd_async_tcp_message* msg = sender->pending_head; msg; msg = msg->next)
    {
        if (!msg->submitted)
        {
            continue;
        }
        struct io_uring_sqe* sqe = io_uring_get_sqe(&sender->ring);
        if (!sqe)
        {
            break;
        }
        io_uring_prep_cancel(sqe, msg, 0);
        io_uring_sqe_set_data(sqe, WD_ASYNC_TCP_CANCEL_CQE);
    }
    (void)io_uring_submit(&sender->ring);
}

static bool wd_async_tcp_sender_drain(struct wd_async_tcp_sender* sender) {
    if (!sender || !sender->ring_ready)
    {
        return true;
    }

    wd_async_tcp_sender_shutdown_pending_fds(sender);
    wd_async_tcp_sender_request_cancels(sender);

    const uint32_t drain_limit = WD_ASYNC_TCP_DRAIN_LIMIT;
    for (uint32_t i = 0; sender->pending_head && i < drain_limit; ++i)
    {
        wd_async_tcp_sender_reap(sender);
        wd_async_tcp_sender_fail_unsubmitted(sender);
        if (!wd_async_tcp_sender_has_submitted(sender))
        {
            break;
        }
        wd_async_tcp_sender_request_cancels(sender);
        usleep(WD_ASYNC_TCP_DRAIN_SLEEP_US);
    }

    return sender->pending_head == NULL;
}

static void wd_async_tcp_sender_fail_all_after_ring_exit(struct wd_async_tcp_sender* sender) {
    if (!sender || sender->ring_ready)
    {
        return;
    }

    struct wd_async_tcp_message* msg = sender->pending_head;
    while (msg)
    {
        struct wd_async_tcp_message* next = msg->next;
        sender->failed++;
        wd_async_tcp_pending_remove(sender, msg);
        wd_async_tcp_complete_message(msg, false);
        wd_async_tcp_message_destroy(msg);
        msg = next;
    }
    sender->inflight = 0;
}

void wd_async_tcp_sender_destroy(struct wd_async_tcp_sender* sender) {
    if (!sender)
    {
        return;
    }

    if (!wd_async_tcp_sender_drain(sender))
    {
        WD_LOG_WARN("async TCP sender destroy timed out; forcing io_uring teardown");
    }

    if (sender->ring_ready)
    {
        /* Closing the ring cancels its requests and synchronizes kernel teardown.
         * Only after queue_exit returns is it safe to release buffers referenced
         * by submitted send operations. */
        io_uring_queue_exit(&sender->ring);
        sender->ring_ready = false;
    }

    wd_async_tcp_sender_fail_all_after_ring_exit(sender);
    wd_socket_pin_reset(&sender->socket_pin);
    free(sender);
}
