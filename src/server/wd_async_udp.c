#include "wd_async_udp.h"
#include "wd_async_udp_accounting.h"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_protocol.h"

#include <liburing.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WD_ASYNC_UDP_DRAIN_LIMIT 250u
#define WD_ASYNC_UDP_DRAIN_SLEEP_US 1000u
#define WD_ASYNC_UDP_PENDING_MULTIPLIER 4u
#define WD_ASYNC_UDP_PENDING_MIN_PACKETS 256u
#define WD_ASYNC_UDP_PENDING_MAX_PACKETS 4096u

struct wd_async_udp_packet {
    struct wd_async_udp_packet* next;
    struct wd_async_udp_packet* prev;
    struct wd_async_udp_packet* free_next;
    struct sockaddr_in          addr;
    struct iovec                iov[2];
    struct msghdr               msg;
    size_t                      packet_size;
    uint8_t                     header[WD_UDP_TILE_HEADER_MAX_SIZE];
    bool                        prepared;
    bool                        submitted;
    wd_async_udp_completion_fn completion;
    void*                       completion_data;
};

struct wd_async_udp_sender {
    struct io_uring ring;
    bool            ring_ready;

    struct wd_async_udp_packet* pending_head;
    struct wd_async_udp_packet* pending_tail;
    struct wd_async_udp_packet* free_packets;
    uint32_t                    free_packet_count;
    uint32_t                    free_packet_limit;
    uint64_t                    pending_packets;
    uint64_t                    pending_bytes;
    uint64_t                    max_pending_packets;
    uint64_t                    max_pending_bytes;

    struct wd_async_udp_accounting accounting;
    uint64_t inflight_max;
    uint64_t local_failures;
    uint64_t fallbacks;
    uint64_t submit_calls;
    uint64_t partial_submits;
    uint64_t saturation_count;
};

static void wd_async_udp_pending_add(struct wd_async_udp_sender* sender, struct wd_async_udp_packet* packet) {
    packet->next = NULL;
    packet->prev = sender->pending_tail;
    if (sender->pending_tail)
    {
        sender->pending_tail->next = packet;
    }
    else
    {
        sender->pending_head = packet;
    }
    sender->pending_tail = packet;
}

static void wd_async_udp_pending_remove(struct wd_async_udp_sender* sender, struct wd_async_udp_packet* packet) {
    if (packet->prev)
    {
        packet->prev->next = packet->next;
    }
    else
    {
        sender->pending_head = packet->next;
    }

    if (packet->next)
    {
        packet->next->prev = packet->prev;
    }
    else
    {
        sender->pending_tail = packet->prev;
    }

    packet->next = NULL;
    packet->prev = NULL;
    if (sender->pending_packets > 0)
    {
        sender->pending_packets--;
    }
    if (sender->pending_bytes >= packet->packet_size)
    {
        sender->pending_bytes -= packet->packet_size;
    }
    else
    {
        sender->pending_bytes = 0;
    }
}

static void wd_async_udp_packet_free(struct wd_async_udp_packet* packet) {
    free(packet);
}

static struct wd_async_udp_packet* wd_async_udp_packet_acquire(struct wd_async_udp_sender* sender) {
    struct wd_async_udp_packet* packet = sender->free_packets;
    if (packet)
    {
        sender->free_packets = packet->free_next;
        packet->free_next = NULL;
        if (sender->free_packet_count > 0)
        {
            sender->free_packet_count--;
        }
        memset(packet, 0, sizeof(*packet));
        return packet;
    }
    return calloc(1, sizeof(*packet));
}

static void wd_async_udp_packet_release(struct wd_async_udp_sender* sender, struct wd_async_udp_packet* packet) {
    if (!packet)
    {
        return;
    }

    packet->next = NULL;
    packet->prev = NULL;
    packet->prepared = false;
    packet->submitted = false;
    if (sender && sender->free_packet_count < sender->free_packet_limit)
    {
        packet->free_next = sender->free_packets;
        sender->free_packets = packet;
        sender->free_packet_count++;
    }
    else
    {
        wd_async_udp_packet_free(packet);
    }
}

bool wd_async_udp_sender_create(struct wd_async_udp_sender** out_sender, uint32_t entries) {
    if (!out_sender)
    {
        return false;
    }

    *out_sender = NULL;

    if (entries < 8)
    {
        entries = 8;
    }

    struct wd_async_udp_sender* sender = calloc(1, sizeof(*sender));
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

    sender->ring_ready = true;
    sender->free_packet_limit = entries;
    uint64_t max_pending_packets = (uint64_t)entries * WD_ASYNC_UDP_PENDING_MULTIPLIER;
    if (max_pending_packets < WD_ASYNC_UDP_PENDING_MIN_PACKETS)
    {
        max_pending_packets = WD_ASYNC_UDP_PENDING_MIN_PACKETS;
    }
    if (max_pending_packets > WD_ASYNC_UDP_PENDING_MAX_PACKETS)
    {
        max_pending_packets = WD_ASYNC_UDP_PENDING_MAX_PACKETS;
    }
    sender->max_pending_packets = max_pending_packets;
    sender->max_pending_bytes = max_pending_packets *
        (uint64_t)(WD_UDP_TILE_HEADER_MAX_SIZE + WD_UDP_TILE_PAYLOAD_MAX);
    *out_sender = sender;
    return true;
}

void wd_async_udp_sender_reap(struct wd_async_udp_sender* sender) {
    if (!sender || !sender->ring_ready)
    {
        return;
    }

    /* A partial or temporarily failed submit must not require another tile to
     * arrive before the remaining SQEs are retried. */
    if (sender->accounting.prepared != 0)
    {
        (void)wd_async_udp_sender_flush(sender);
    }

    struct io_uring_cqe* cqe = NULL;
    while (io_uring_peek_cqe(&sender->ring, &cqe) == 0 && cqe)
    {
        struct wd_async_udp_packet* packet = io_uring_cqe_get_data(cqe);
        const bool success = packet && cqe->res == (int)packet->packet_size;
        (void)wd_async_udp_accounting_complete(&sender->accounting, success);
        if (packet)
        {
            if (packet->completion)
            {
                packet->completion(packet->completion_data, success);
            }
            packet->submitted = false;
            wd_async_udp_pending_remove(sender, packet);
            wd_async_udp_packet_release(sender, packet);
        }
        io_uring_cqe_seen(&sender->ring, cqe);
        cqe = NULL;
    }
}

static uint32_t wd_async_udp_mark_submitted(struct wd_async_udp_sender* sender, uint32_t submitted_count) {
    uint32_t marked = 0;
    for (struct wd_async_udp_packet* packet = sender ? sender->pending_head : NULL; packet && marked < submitted_count;
         packet = packet->next)
    {
        if (packet->prepared && !packet->submitted)
        {
            packet->prepared = false;
            packet->submitted = true;
            marked++;
        }
    }
    if (sender && sender->accounting.submitted > sender->inflight_max)
    {
        sender->inflight_max = sender->accounting.submitted;
    }
    return marked;
}

bool wd_async_udp_sender_flush(struct wd_async_udp_sender* sender) {
    if (!sender || !sender->ring_ready || sender->accounting.prepared == 0)
    {
        return true;
    }

    const uint32_t prepared_before = sender->accounting.prepared;
    sender->submit_calls++;
    const int rc = io_uring_submit(&sender->ring);
    const uint32_t submitted = wd_async_udp_accounting_submit_result(&sender->accounting, rc);
    if (submitted < prepared_before)
    {
        sender->partial_submits++;
    }
    if (submitted != 0)
    {
        const uint32_t marked = wd_async_udp_mark_submitted(sender, submitted);
        if (marked != submitted)
        {
            /* The accounting and packet list must move in lockstep. Treat a
             * mismatch as fatal to the sender rather than guessing ownership. */
            sender->local_failures++;
            return false;
        }
    }

    return sender->accounting.prepared == 0;
}

enum wd_async_udp_send_status wd_async_udp_send_packet(
    struct wd_async_udp_sender* sender, int fd, const struct sockaddr_in* addr, const void* header,
    uint32_t header_size, const void* payload, uint32_t payload_size,
    wd_async_udp_completion_fn completion, void* completion_data) {
    if (!sender || !sender->ring_ready || fd < 0)
    {
        return WD_ASYNC_UDP_SEND_FAILED;
    }
    if (!addr || !header || header_size == 0 || header_size > WD_UDP_TILE_HEADER_MAX_SIZE || (payload_size != 0 && !payload))
    {
        sender->local_failures++;
        return WD_ASYNC_UDP_SEND_FAILED;
    }

    const size_t packet_size = (size_t)header_size + (size_t)payload_size;
    if (packet_size > UINT32_MAX)
    {
        sender->local_failures++;
        return WD_ASYNC_UDP_SEND_FAILED;
    }

    if (!wd_async_udp_pending_within_limits(sender->pending_packets, sender->pending_bytes, packet_size,
                                             sender->max_pending_packets, sender->max_pending_bytes))
    {
        wd_async_udp_sender_reap(sender);
        if (!wd_async_udp_pending_within_limits(sender->pending_packets, sender->pending_bytes, packet_size,
                                                 sender->max_pending_packets, sender->max_pending_bytes))
        {
            sender->saturation_count++;
            return WD_ASYNC_UDP_SEND_SATURATED;
        }
    }

    struct wd_async_udp_packet* packet = wd_async_udp_packet_acquire(sender);
    if (!packet)
    {
        sender->local_failures++;
        return WD_ASYNC_UDP_SEND_FAILED;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&sender->ring);
    if (!sqe)
    {
        (void)wd_async_udp_sender_flush(sender);
        wd_async_udp_sender_reap(sender);
        sqe = io_uring_get_sqe(&sender->ring);
        if (!sqe)
        {
            sender->fallbacks++;
            wd_async_udp_packet_release(sender, packet);
            return WD_ASYNC_UDP_SEND_FAILED;
        }
    }

    packet->addr = *addr;
    packet->completion = completion;
    packet->completion_data = completion_data;
    packet->packet_size = packet_size;
    memcpy(packet->header, header, header_size);

    packet->iov[0].iov_base = packet->header;
    packet->iov[0].iov_len = header_size;
    packet->iov[1].iov_base = (void*)payload;
    packet->iov[1].iov_len = payload_size;

    packet->msg.msg_name    = &packet->addr;
    packet->msg.msg_namelen = sizeof(packet->addr);
    packet->msg.msg_iov     = packet->iov;
    packet->msg.msg_iovlen  = payload_size != 0 ? 2u : 1u;

    io_uring_prep_sendmsg(sqe, fd, &packet->msg, 0);
    io_uring_sqe_set_data(sqe, packet);

    wd_async_udp_pending_add(sender, packet);
    sender->pending_packets++;
    sender->pending_bytes += packet_size;
    packet->prepared = true;
    wd_async_udp_accounting_queue(&sender->accounting);

    return WD_ASYNC_UDP_SEND_QUEUED;
}

uint64_t wd_async_udp_sender_inflight(const struct wd_async_udp_sender* sender) {
    return sender ? sender->accounting.submitted : 0;
}

uint64_t wd_async_udp_sender_queued(const struct wd_async_udp_sender* sender) {
    return sender ? sender->accounting.queued_total : 0;
}

uint64_t wd_async_udp_sender_completed(const struct wd_async_udp_sender* sender) {
    return sender ? sender->accounting.completed_total : 0;
}

uint64_t wd_async_udp_sender_failed(const struct wd_async_udp_sender* sender) {
    return sender ? sender->accounting.failed_total + sender->accounting.submit_failures + sender->local_failures : 0;
}

uint64_t wd_async_udp_sender_fallbacks(const struct wd_async_udp_sender* sender) {
    return sender ? sender->fallbacks : 0;
}

uint64_t wd_async_udp_sender_inflight_max(const struct wd_async_udp_sender* sender) {
    return sender ? sender->inflight_max : 0;
}

uint64_t wd_async_udp_sender_pending_packets(const struct wd_async_udp_sender* sender) {
    return sender ? sender->pending_packets : 0;
}

uint64_t wd_async_udp_sender_pending_bytes(const struct wd_async_udp_sender* sender) {
    return sender ? sender->pending_bytes : 0;
}

uint64_t wd_async_udp_sender_saturation_count(const struct wd_async_udp_sender* sender) {
    return sender ? sender->saturation_count : 0;
}

static void wd_async_udp_sender_cancel_unsubmitted_after_ring_exit(struct wd_async_udp_sender* sender) {
    if (!sender || sender->ring_ready)
    {
        return;
    }

    (void)wd_async_udp_accounting_cancel_prepared(&sender->accounting);
    struct wd_async_udp_packet* packet = sender->pending_head;
    while (packet)
    {
        struct wd_async_udp_packet* next = packet->next;
        if (!packet->submitted)
        {
            if (packet->completion)
            {
                packet->completion(packet->completion_data, false);
            }
            wd_async_udp_pending_remove(sender, packet);
            wd_async_udp_packet_release(sender, packet);
        }
        packet = next;
    }
}

static bool wd_async_udp_sender_has_submitted(const struct wd_async_udp_sender* sender) {
    return sender && sender->accounting.submitted != 0;
}

bool wd_async_udp_sender_drain(struct wd_async_udp_sender* sender) {
    if (!sender || !sender->ring_ready)
    {
        return true;
    }

    for (uint32_t i = 0; sender->pending_head && i < WD_ASYNC_UDP_DRAIN_LIMIT; ++i)
    {
        (void)wd_async_udp_sender_flush(sender);
        wd_async_udp_sender_reap(sender);
        if (!wd_async_udp_sender_has_submitted(sender) && sender->accounting.prepared == 0)
        {
            break;
        }
        usleep(WD_ASYNC_UDP_DRAIN_SLEEP_US);
    }

    if (!wd_async_udp_sender_has_submitted(sender) && sender->accounting.prepared != 0)
    {
        /* Unsubmitted SQEs still reference packet storage. Drop the ring first,
         * then fail callbacks and recycle buffers. */
        io_uring_queue_exit(&sender->ring);
        sender->ring_ready = false;
        wd_async_udp_sender_cancel_unsubmitted_after_ring_exit(sender);
    }
    return sender->pending_head == NULL;
}

void wd_async_udp_sender_destroy(struct wd_async_udp_sender* sender) {
    if (!sender)
    {
        return;
    }

    if (!wd_async_udp_sender_drain(sender))
    {
        WD_LOG_WARN("async UDP sender destroy timed out; leaking pending ring buffers safely");
        return;
    }
    wd_async_udp_sender_cancel_unsubmitted_after_ring_exit(sender);

    struct wd_async_udp_packet* packet = sender->free_packets;
    while (packet)
    {
        struct wd_async_udp_packet* next = packet->free_next;
        wd_async_udp_packet_free(packet);
        packet = next;
    }

    if (sender->ring_ready)
    {
        io_uring_queue_exit(&sender->ring);
        sender->ring_ready = false;
    }
    free(sender);
}

uint64_t wd_async_udp_sender_submit_calls(const struct wd_async_udp_sender* sender) {
    return sender ? sender->submit_calls : 0;
}

uint64_t wd_async_udp_sender_partial_submits(const struct wd_async_udp_sender* sender) {
    return sender ? sender->partial_submits : 0;
}
