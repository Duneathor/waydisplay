#include "client_async_tcp.hpp"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_protocol.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <liburing.h>
#include <mutex>
#include <new>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#ifdef MSG_NOSIGNAL
#define WD_CLIENT_ASYNC_SEND_FLAGS MSG_NOSIGNAL
#else
#define WD_CLIENT_ASYNC_SEND_FLAGS 0
#endif

#ifndef WD_CLIENT_ASYNC_TCP_DRAIN_LIMIT
#define WD_CLIENT_ASYNC_TCP_DRAIN_LIMIT WD_ASYNC_SENDER_DRAIN_LIMIT
#endif
#ifndef WD_CLIENT_ASYNC_TCP_DRAIN_SLEEP_US
#define WD_CLIENT_ASYNC_TCP_DRAIN_SLEEP_US WD_ASYNC_SENDER_DRAIN_SLEEP_US
#endif

namespace waydisplay {
namespace {

char        g_cancel_cqe_tag;
void* const CANCEL_CQE = &g_cancel_cqe_tag;

struct Message {
    Message*             next         = nullptr;
    Message*             prev         = nullptr;
    int                  fd           = -1;
    uint16_t             message_type = 0;
    size_t               bytes_sent   = 0;
    bool                 submitted    = false;
    std::vector<uint8_t> bytes;
};

} // namespace

struct ClientAsyncTcpSender {
    std::mutex mutex;
    io_uring   ring{};
    bool       ring_ready = false;

    Message* head = nullptr;
    Message* tail = nullptr;

    uint64_t inflight          = 0;
    uint64_t inflight_max      = 0;
    uint64_t pending_bytes     = 0;
    uint64_t max_pending_bytes = WD_CLIENT_ASYNC_TCP_DEFAULT_PENDING_BYTES;
    uint64_t queued            = 0;
    uint64_t completed         = 0;
    uint64_t failed            = 0;
    uint64_t overflows         = 0;
    uint64_t partial_resubmits = 0;
    uint64_t coalesced         = 0;
    bool     fatal             = false;
};

namespace {

void pending_add(ClientAsyncTcpSender* sender, Message* msg) {
    msg->next = nullptr;
    msg->prev = sender->tail;
    if (sender->tail)
    {
        sender->tail->next = msg;
    }
    else
    {
        sender->head = msg;
    }
    sender->tail = msg;
    sender->pending_bytes += msg->bytes.size();
}

void pending_remove(ClientAsyncTcpSender* sender, Message* msg) {
    if (msg->prev)
    {
        msg->prev->next = msg->next;
    }
    else
    {
        sender->head = msg->next;
    }

    if (msg->next)
    {
        msg->next->prev = msg->prev;
    }
    else
    {
        sender->tail = msg->prev;
    }

    if (sender->pending_bytes >= msg->bytes.size())
    {
        sender->pending_bytes -= msg->bytes.size();
    }
    else
    {
        sender->pending_bytes = 0;
    }

    msg->next = nullptr;
    msg->prev = nullptr;
}

bool is_pointer_motion_message(const Message* msg) {
    if (!msg || msg->message_type != WD_MSG_POINTER_EVENT || msg->bytes.size() < sizeof(wd_tcp_header) + sizeof(wd_pointer_event_payload))
    {
        return false;
    }

    wd_pointer_event_payload pointer{};
    std::memcpy(&pointer, msg->bytes.data() + sizeof(wd_tcp_header), sizeof(pointer));
    return pointer.event_type == WD_POINTER_EVENT_MOTION;
}

uint64_t drop_stale_unsubmitted_pointer_motion_locked(ClientAsyncTcpSender* sender, int fd) {
    if (!sender || fd < 0)
    {
        return 0;
    }

    uint64_t dropped = 0;
    Message* msg     = sender->head;
    while (msg)
    {
        Message* next = msg->next;
        if (!msg->submitted && msg->fd == fd && is_pointer_motion_message(msg))
        {
            pending_remove(sender, msg);
            delete msg;
            dropped++;
        }
        msg = next;
    }
    return dropped;
}

Message* create_message(int fd, uint16_t message_type, const void* payload, uint32_t payload_size) {
    if (payload_size != 0 && !payload)
    {
        return nullptr;
    }

    auto* msg = new (std::nothrow) Message();
    if (!msg)
    {
        return nullptr;
    }

    const size_t total_size = sizeof(wd_tcp_header) + static_cast<size_t>(payload_size);
    msg->bytes.resize(total_size);

    wd_tcp_header header{};
    header.magic            = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = message_type;
    header.payload_size     = payload_size;

    msg->fd           = fd;
    msg->message_type = message_type;
    std::memcpy(msg->bytes.data(), &header, sizeof(header));
    if (payload_size != 0)
    {
        std::memcpy(msg->bytes.data() + sizeof(header), payload, payload_size);
    }

    return msg;
}

bool submit_message_locked(ClientAsyncTcpSender* sender, Message* msg) {
    if (!sender || !sender->ring_ready || !msg || msg->submitted || msg->fd < 0 || msg->bytes_sent >= msg->bytes.size())
    {
        return false;
    }

    io_uring_sqe* sqe = io_uring_get_sqe(&sender->ring);
    if (!sqe)
    {
        return false;
    }

    io_uring_prep_send(sqe, msg->fd, msg->bytes.data() + msg->bytes_sent, msg->bytes.size() - msg->bytes_sent, WD_CLIENT_ASYNC_SEND_FLAGS);
    io_uring_sqe_set_data(sqe, msg);

    msg->submitted = true;
    sender->inflight++;
    if (sender->inflight > sender->inflight_max)
    {
        sender->inflight_max = sender->inflight;
    }

    const int rc = io_uring_submit(&sender->ring);
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

bool try_start_head_locked(ClientAsyncTcpSender* sender) {
    if (!sender || sender->inflight != 0 || !sender->head)
    {
        return true;
    }

    if (!submit_message_locked(sender, sender->head))
    {
        Message* failed = sender->head;
        sender->failed++;
        sender->fatal = true;
        if (failed->fd >= 0)
        {
            (void)::shutdown(failed->fd, SHUT_RDWR);
        }
        pending_remove(sender, failed);
        delete failed;
        return false;
    }

    return true;
}

void reap_locked(ClientAsyncTcpSender* sender) {
    if (!sender || !sender->ring_ready)
    {
        return;
    }

    io_uring_cqe* cqe = nullptr;
    while (io_uring_peek_cqe(&sender->ring, &cqe) == 0 && cqe)
    {
        void* data = io_uring_cqe_get_data(cqe);
        if (data == CANCEL_CQE)
        {
            io_uring_cqe_seen(&sender->ring, cqe);
            cqe = nullptr;
            continue;
        }

        auto* msg = static_cast<Message*>(data);
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
            sender->fatal = true;
            if (msg->fd >= 0)
            {
                (void)::shutdown(msg->fd, SHUT_RDWR);
            }
            pending_remove(sender, msg);
            delete msg;
            try_start_head_locked(sender);
        }
        else
        {
            msg->bytes_sent += static_cast<size_t>(cqe->res);
            if (msg->bytes_sent >= msg->bytes.size())
            {
                sender->completed++;
                pending_remove(sender, msg);
                delete msg;
                try_start_head_locked(sender);
            }
            else
            {
                sender->partial_resubmits++;
                if (!submit_message_locked(sender, msg))
                {
                    sender->failed++;
                    sender->fatal = true;
                    if (msg->fd >= 0)
                    {
                        (void)::shutdown(msg->fd, SHUT_RDWR);
                    }
                    pending_remove(sender, msg);
                    delete msg;
                    try_start_head_locked(sender);
                }
            }
        }

        io_uring_cqe_seen(&sender->ring, cqe);
        cqe = nullptr;
    }
}

void fail_unsubmitted_locked(ClientAsyncTcpSender* sender) {
    Message* msg = sender ? sender->head : nullptr;
    while (msg)
    {
        Message* next = msg->next;
        if (!msg->submitted)
        {
            pending_remove(sender, msg);
            delete msg;
        }
        msg = next;
    }
}

bool has_submitted_locked(const ClientAsyncTcpSender* sender) {
    for (const Message* msg = sender ? sender->head : nullptr; msg; msg = msg->next)
    {
        if (msg->submitted)
        {
            return true;
        }
    }
    return false;
}

void shutdown_pending_fds_locked(ClientAsyncTcpSender* sender) {
    for (Message* msg = sender ? sender->head : nullptr; msg; msg = msg->next)
    {
        if (msg->fd >= 0)
        {
            (void)::shutdown(msg->fd, SHUT_RDWR);
        }
    }
}

void request_cancels_locked(ClientAsyncTcpSender* sender) {
    if (!sender || !sender->ring_ready)
    {
        return;
    }

    for (Message* msg = sender->head; msg; msg = msg->next)
    {
        if (!msg->submitted)
        {
            continue;
        }
        io_uring_sqe* sqe = io_uring_get_sqe(&sender->ring);
        if (!sqe)
        {
            break;
        }
        io_uring_prep_cancel(sqe, msg, 0);
        io_uring_sqe_set_data(sqe, CANCEL_CQE);
    }
    (void)io_uring_submit(&sender->ring);
}

bool drain_locked(ClientAsyncTcpSender* sender) {
    if (!sender || !sender->ring_ready)
    {
        return true;
    }

    shutdown_pending_fds_locked(sender);
    request_cancels_locked(sender);

    const uint32_t drain_limit = WD_CLIENT_ASYNC_TCP_DRAIN_LIMIT;
    for (uint32_t i = 0; sender->head && i < drain_limit; ++i)
    {
        reap_locked(sender);
        fail_unsubmitted_locked(sender);
        if (!has_submitted_locked(sender))
        {
            break;
        }
        request_cancels_locked(sender);
        usleep(WD_CLIENT_ASYNC_TCP_DRAIN_SLEEP_US);
    }

    return sender->head == nullptr;
}

void fail_all_after_ring_exit_locked(ClientAsyncTcpSender* sender) {
    if (!sender || sender->ring_ready)
    {
        return;
    }

    Message* msg = sender->head;
    while (msg)
    {
        Message* next = msg->next;
        sender->failed++;
        msg->submitted = false;
        pending_remove(sender, msg);
        delete msg;
        msg = next;
    }
    sender->inflight = 0;
}

ClientAsyncTcpSenderStats snapshot_stats_locked(const ClientAsyncTcpSender* sender) {
    ClientAsyncTcpSenderStats stats{};
    if (!sender)
    {
        return stats;
    }
    stats.queued            = sender->queued;
    stats.completed         = sender->completed;
    stats.failed            = sender->failed;
    stats.overflows         = sender->overflows;
    stats.partial_resubmits = sender->partial_resubmits;
    stats.coalesced         = sender->coalesced;
    stats.inflight_max      = sender->inflight_max;
    stats.inflight          = sender->inflight;
    stats.pending_bytes     = sender->pending_bytes;
    return stats;
}

} // namespace

ClientAsyncTcpSender* client_async_tcp_sender_create(uint32_t entries, uint64_t max_pending_bytes) {
    if (entries < WD_ASYNC_MIN_RING_ENTRIES)
    {
        entries = WD_ASYNC_MIN_RING_ENTRIES;
    }

    auto* sender = new (std::nothrow) ClientAsyncTcpSender();
    if (!sender)
    {
        return nullptr;
    }

    const int rc = io_uring_queue_init(entries, &sender->ring, 0);
    if (rc < 0)
    {
        delete sender;
        return nullptr;
    }

    sender->ring_ready        = true;
    sender->max_pending_bytes = max_pending_bytes != 0 ? max_pending_bytes : WD_CLIENT_ASYNC_TCP_DEFAULT_PENDING_BYTES;
    return sender;
}

ClientAsyncTcpSenderStats client_async_tcp_sender_destroy(ClientAsyncTcpSender* sender) {
    ClientAsyncTcpSenderStats final_stats{};
    if (!sender)
    {
        return final_stats;
    }

    {
        std::lock_guard<std::mutex> lock(sender->mutex);
        if (!drain_locked(sender))
        {
            WD_LOG_WARN("client async TCP sender destroy timed out; forcing io_uring teardown");
        }
        fail_unsubmitted_locked(sender);
        if (sender->ring_ready)
        {
            /* queue_exit synchronizes cancellation before message buffers are
             * released, matching the server sender ownership rule. */
            io_uring_queue_exit(&sender->ring);
            sender->ring_ready = false;
        }
        fail_all_after_ring_exit_locked(sender);
        final_stats = snapshot_stats_locked(sender);
    }

    delete sender;
    return final_stats;
}

void client_async_tcp_sender_reap(ClientAsyncTcpSender* sender) {
    if (!sender)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(sender->mutex);
    reap_locked(sender);
    try_start_head_locked(sender);
}

bool client_async_tcp_send_message(ClientAsyncTcpSender* sender, int fd, uint16_t message_type, const void* payload,
                                   uint32_t payload_size) {
    if (!sender || fd < 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(sender->mutex);
    reap_locked(sender);
    if (sender->fatal)
    {
        return false;
    }

    const uint64_t total_size = static_cast<uint64_t>(sizeof(wd_tcp_header)) + static_cast<uint64_t>(payload_size);
    if (sender->max_pending_bytes != 0 && sender->pending_bytes + total_size > sender->max_pending_bytes)
    {
        sender->overflows++;
        sender->failed++;
        return false;
    }

    const bool coalesce_pointer_motion = message_type == WD_MSG_POINTER_EVENT && payload_size == sizeof(wd_pointer_event_payload) &&
                                         payload &&
                                         static_cast<const wd_pointer_event_payload*>(payload)->event_type == WD_POINTER_EVENT_MOTION;
    if (coalesce_pointer_motion)
    {
        sender->coalesced += drop_stale_unsubmitted_pointer_motion_locked(sender, fd);
    }

    Message* msg = create_message(fd, message_type, payload, payload_size);
    if (!msg)
    {
        sender->failed++;
        return false;
    }

    pending_add(sender, msg);
    if (!try_start_head_locked(sender) || sender->fatal)
    {
        return false;
    }

    sender->queued++;
    return true;
}

ClientAsyncTcpSenderStats client_async_tcp_sender_stats(ClientAsyncTcpSender* sender) {
    if (!sender)
    {
        return {};
    }

    std::lock_guard<std::mutex> lock(sender->mutex);
    reap_locked(sender);
    return snapshot_stats_locked(sender);
}

} // namespace waydisplay
