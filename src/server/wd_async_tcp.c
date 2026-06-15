#include "wd_async_tcp.h"

#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_log.h"

#include <errno.h>
#include <liburing.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define WD_ASYNC_SEND_FLAGS MSG_NOSIGNAL
#else
#define WD_ASYNC_SEND_FLAGS 0
#endif

#define WD_ASYNC_TCP_DEFAULT_MAX_PENDING_BYTES (4ull * 1024ull * 1024ull)
#define WD_ASYNC_TCP_DRAIN_LIMIT 250u
#define WD_ASYNC_TCP_DRAIN_SLEEP_US 1000u

static char wd_async_tcp_cancel_cqe_tag;
#define WD_ASYNC_TCP_CANCEL_CQE ((void*)&wd_async_tcp_cancel_cqe_tag)

struct wd_async_tcp_message {
    struct wd_async_tcp_message* next;
    struct wd_async_tcp_message* prev;
    int                          fd;
    uint16_t                     message_type;
    size_t                       total_size;
    size_t                       bytes_sent;
    bool                         submitted;
    wd_async_tcp_complete_fn     complete;
    void*                        user_data;
    uint8_t                      bytes[];
};

struct wd_async_tcp_sender {
    struct io_uring ring;
    bool            ring_ready;

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
};

static void wd_async_tcp_complete_message(struct wd_async_tcp_message* msg, bool success) {
    if (msg && msg->complete)
    {
        msg->complete(msg->user_data, success);
    }
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

static bool wd_async_tcp_submit_message(struct wd_async_tcp_sender* sender, struct wd_async_tcp_message* msg) {
    if (!sender || !sender->ring_ready || !msg || msg->submitted || msg->fd < 0 || msg->bytes_sent >= msg->total_size)
    {
        return false;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&sender->ring);
    if (!sqe)
    {
        return false;
    }

    io_uring_prep_send(sqe, msg->fd, msg->bytes + msg->bytes_sent, msg->total_size - msg->bytes_sent,
                       WD_ASYNC_SEND_FLAGS);
    io_uring_sqe_set_data(sqe, msg);

    msg->submitted = true;
    sender->inflight++;
    if (sender->inflight > sender->inflight_max)
    {
        sender->inflight_max = sender->inflight;
    }

    int rc = io_uring_submit(&sender->ring);
    if (rc <= 0)
    {
        msg->submitted = false;
        if (sender->inflight > 0)
        {
            sender->inflight--;
        }
        return false;
    }

    return true;
}

static bool wd_async_tcp_try_start_head(struct wd_async_tcp_sender* sender,
                                        struct wd_async_tcp_message* suppress_completion,
                                        bool* suppressed_message_failed) {
    if (suppressed_message_failed)
    {
        *suppressed_message_failed = false;
    }

    if (!sender || sender->inflight != 0 || !sender->pending_head)
    {
        return true;
    }

    if (!wd_async_tcp_submit_message(sender, sender->pending_head))
    {
        struct wd_async_tcp_message* failed_msg = sender->pending_head;
        bool suppressed = failed_msg == suppress_completion;
        sender->failed++;
        wd_async_tcp_pending_remove(sender, failed_msg);
        if (!suppressed)
        {
            wd_async_tcp_complete_message(failed_msg, false);
        }
        free(failed_msg);
        if (suppressed_message_failed)
        {
            *suppressed_message_failed = suppressed;
        }
        return false;
    }

    return true;
}

static struct wd_async_tcp_message* wd_async_tcp_message_create(int fd, uint16_t message_type, const void* payload,
                                                                uint32_t payload_size,
                                                                wd_async_tcp_complete_fn complete, void* user_data) {
    if (payload_size != 0 && !payload)
    {
        return NULL;
    }

    const size_t total_size = sizeof(struct wd_tcp_header) + (size_t)payload_size;
    struct wd_async_tcp_message* msg = calloc(1, sizeof(*msg) + total_size);
    if (!msg)
    {
        return NULL;
    }

    struct wd_tcp_header header;
    memset(&header, 0, sizeof(header));
    header.magic            = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = message_type;
    header.payload_size     = payload_size;

    msg->fd           = fd;
    msg->message_type = message_type;
    msg->total_size   = total_size;
    msg->complete     = complete;
    msg->user_data    = user_data;
    memcpy(msg->bytes, &header, sizeof(header));
    if (payload_size != 0)
    {
        memcpy(msg->bytes + sizeof(header), payload, payload_size);
    }

    return msg;
}

bool wd_async_tcp_sender_create(struct wd_async_tcp_sender** out_sender, uint32_t entries) {
    if (!out_sender)
    {
        return false;
    }

    *out_sender = NULL;

    if (entries < 8)
    {
        entries = 8;
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

    sender->ring_ready        = true;
    sender->max_pending_bytes = WD_ASYNC_TCP_DEFAULT_MAX_PENDING_BYTES;
    *out_sender = sender;
    return true;
}

void wd_async_tcp_sender_reap(struct wd_async_tcp_sender* sender) {
    if (!sender || !sender->ring_ready)
    {
        return;
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

        if (msg && msg->submitted)
        {
            msg->submitted = false;
            if (sender->inflight > 0)
            {
                sender->inflight--;
            }
        }

        if (!msg)
        {
            sender->failed++;
        }
        else if (cqe->res <= 0)
        {
            sender->failed++;
            wd_async_tcp_pending_remove(sender, msg);
            wd_async_tcp_complete_message(msg, false);
            free(msg);
            wd_async_tcp_try_start_head(sender, NULL, NULL);
        }
        else
        {
            msg->bytes_sent += (size_t)cqe->res;
            if (msg->bytes_sent >= msg->total_size)
            {
                sender->completed++;
                wd_async_tcp_pending_remove(sender, msg);
                wd_async_tcp_complete_message(msg, true);
                free(msg);
                wd_async_tcp_try_start_head(sender, NULL, NULL);
            }
            else
            {
                sender->partial_resubmits++;
                if (!wd_async_tcp_submit_message(sender, msg))
                {
                    sender->failed++;
                    wd_async_tcp_pending_remove(sender, msg);
                    wd_async_tcp_complete_message(msg, false);
                    free(msg);
                    wd_async_tcp_try_start_head(sender, NULL, NULL);
                }
            }
        }

        io_uring_cqe_seen(&sender->ring, cqe);
        cqe = NULL;
    }
}

bool wd_async_tcp_send_message_ex(struct wd_async_tcp_sender* sender, int fd, uint16_t message_type, const void* payload,
                                  uint32_t payload_size, wd_async_tcp_complete_fn complete, void* user_data) {
    if (!sender || !sender->ring_ready || fd < 0)
    {
        return false;
    }

    wd_async_tcp_sender_reap(sender);

    const uint64_t total_size = (uint64_t)sizeof(struct wd_tcp_header) + (uint64_t)payload_size;
    if (sender->max_pending_bytes != 0 && sender->pending_bytes + total_size > sender->max_pending_bytes)
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

    uint32_t dropped = 0;
    struct wd_async_tcp_message* msg = sender->pending_head;
    while (msg)
    {
        struct wd_async_tcp_message* next = msg->next;
        if (msg->message_type == message_type && !msg->submitted)
        {
            wd_async_tcp_pending_remove(sender, msg);
            wd_async_tcp_complete_message(msg, false);
            free(msg);
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
            free(msg);
        }
        msg = next;
    }
}

static void wd_async_tcp_sender_shutdown_pending_fds(struct wd_async_tcp_sender* sender) {
    for (struct wd_async_tcp_message* msg = sender ? sender->pending_head : NULL; msg; msg = msg->next)
    {
        if (msg->fd >= 0)
        {
            (void)shutdown(msg->fd, SHUT_RDWR);
        }
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

    for (uint32_t i = 0; sender->pending_head && i < WD_ASYNC_TCP_DRAIN_LIMIT; ++i)
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

void wd_async_tcp_sender_destroy(struct wd_async_tcp_sender* sender) {
    if (!sender)
    {
        return;
    }

    if (!wd_async_tcp_sender_drain(sender))
    {
        WD_LOG_WARN("async TCP sender destroy timed out; leaking pending ring buffers safely");
        return;
    }
    wd_async_tcp_sender_fail_unsubmitted(sender);

    if (sender->ring_ready)
    {
        io_uring_queue_exit(&sender->ring);
        sender->ring_ready = false;
    }
    free(sender);
}
