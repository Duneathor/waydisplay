#include "wd_async_udp.h"

#include "waydisplay/wd_config.h"
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

struct wd_async_udp_packet {
    struct wd_async_udp_packet* next;
    struct wd_async_udp_packet* prev;
    struct wd_async_udp_packet* free_next;
    struct sockaddr_in          addr;
    struct iovec                iov;
    struct msghdr               msg;
    size_t                      packet_size;
    size_t                      capacity;
    uint8_t*                    packet;
    bool                        prepared;
    bool                        submitted;
};

struct wd_async_udp_sender {
    struct io_uring ring;
    bool            ring_ready;

    struct wd_async_udp_packet* pending_head;
    struct wd_async_udp_packet* pending_tail;
    struct wd_async_udp_packet* free_packets;
    uint32_t                    free_packet_count;
    uint32_t                    free_packet_limit;

    uint32_t prepared_submit;

    uint64_t inflight;
    uint64_t inflight_max;
    uint64_t queued;
    uint64_t completed;
    uint64_t failed;
    uint64_t fallbacks;
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
}

static void wd_async_udp_packet_free(struct wd_async_udp_packet* packet) {
    if (!packet)
    {
        return;
    }
    free(packet->packet);
    free(packet);
}

static struct wd_async_udp_packet* wd_async_udp_packet_acquire(struct wd_async_udp_sender* sender, size_t packet_size) {
    struct wd_async_udp_packet* packet = sender->free_packets;
    if (packet)
    {
        sender->free_packets = packet->free_next;
        packet->free_next = NULL;
        if (sender->free_packet_count > 0)
        {
            sender->free_packet_count--;
        }
    }
    else
    {
        packet = calloc(1, sizeof(*packet));
        if (!packet)
        {
            return NULL;
        }
    }

    if (packet->capacity < packet_size)
    {
        uint8_t* resized = realloc(packet->packet, packet_size);
        if (!resized)
        {
            wd_async_udp_packet_free(packet);
            return NULL;
        }
        packet->packet = resized;
        packet->capacity = packet_size;
    }

    uint8_t* data = packet->packet;
    size_t capacity = packet->capacity;
    memset(packet, 0, sizeof(*packet));
    packet->packet = data;
    packet->capacity = capacity;
    packet->packet_size = packet_size;
    return packet;
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
    *out_sender = sender;
    return true;
}

void wd_async_udp_sender_reap(struct wd_async_udp_sender* sender) {
    if (!sender || !sender->ring_ready)
    {
        return;
    }

    struct io_uring_cqe* cqe = NULL;
    while (io_uring_peek_cqe(&sender->ring, &cqe) == 0 && cqe)
    {
        struct wd_async_udp_packet* packet = io_uring_cqe_get_data(cqe);
        if (cqe->res < 0)
        {
            sender->failed++;
        }
        else
        {
            sender->completed++;
        }
        if (packet)
        {
            if (packet->submitted && sender->inflight > 0)
            {
                sender->inflight--;
            }
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
            sender->inflight++;
            marked++;
        }
    }
    if (sender && sender->inflight > sender->inflight_max)
    {
        sender->inflight_max = sender->inflight;
    }
    return marked;
}

bool wd_async_udp_sender_flush(struct wd_async_udp_sender* sender) {
    if (!sender || !sender->ring_ready || sender->prepared_submit == 0)
    {
        return true;
    }

    int rc = io_uring_submit(&sender->ring);
    if (rc <= 0)
    {
        if (rc < 0)
        {
            sender->failed++;
        }
        return false;
    }

    uint32_t submitted = (uint32_t)rc;
    if (submitted > sender->prepared_submit)
    {
        submitted = sender->prepared_submit;
    }
    uint32_t marked = wd_async_udp_mark_submitted(sender, submitted);
    if (marked > sender->prepared_submit)
    {
        sender->prepared_submit = 0;
    }
    else
    {
        sender->prepared_submit -= marked;
    }

    return sender->prepared_submit == 0;
}

bool wd_async_udp_send_packet(struct wd_async_udp_sender* sender, int fd, const struct sockaddr_in* addr, const void* header,
                              uint32_t header_size, const void* payload, uint32_t payload_size) {
    if (!sender || !sender->ring_ready || fd < 0)
    {
        return false;
    }
    if (!addr || !header || header_size == 0 || header_size > WD_UDP_TILE_HEADER_MAX_SIZE || (payload_size != 0 && !payload))
    {
        sender->failed++;
        return false;
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
            return false;
        }
    }

    size_t packet_size = (size_t)header_size + (size_t)payload_size;
    if (packet_size > UINT32_MAX)
    {
        sender->failed++;
        return false;
    }

    struct wd_async_udp_packet* packet = wd_async_udp_packet_acquire(sender, packet_size);
    if (!packet)
    {
        sender->failed++;
        return false;
    }

    packet->addr = *addr;
    memcpy(packet->packet, header, header_size);
    if (payload_size != 0)
    {
        memcpy(packet->packet + header_size, payload, payload_size);
    }

    packet->iov.iov_base = packet->packet;
    packet->iov.iov_len  = packet_size;

    packet->msg.msg_name    = &packet->addr;
    packet->msg.msg_namelen = sizeof(packet->addr);
    packet->msg.msg_iov     = &packet->iov;
    packet->msg.msg_iovlen  = 1;

    io_uring_prep_sendmsg(sqe, fd, &packet->msg, 0);
    io_uring_sqe_set_data(sqe, packet);

    wd_async_udp_pending_add(sender, packet);
    packet->prepared = true;
    sender->queued++;
    sender->prepared_submit++;

    return true;
}

uint64_t wd_async_udp_sender_inflight(const struct wd_async_udp_sender* sender) {
    return sender ? sender->inflight : 0;
}

uint64_t wd_async_udp_sender_queued(const struct wd_async_udp_sender* sender) {
    return sender ? sender->queued : 0;
}

uint64_t wd_async_udp_sender_completed(const struct wd_async_udp_sender* sender) {
    return sender ? sender->completed : 0;
}

uint64_t wd_async_udp_sender_failed(const struct wd_async_udp_sender* sender) {
    return sender ? sender->failed : 0;
}

uint64_t wd_async_udp_sender_fallbacks(const struct wd_async_udp_sender* sender) {
    return sender ? sender->fallbacks : 0;
}

uint64_t wd_async_udp_sender_inflight_max(const struct wd_async_udp_sender* sender) {
    return sender ? sender->inflight_max : 0;
}

static void wd_async_udp_sender_fail_unsubmitted(struct wd_async_udp_sender* sender) {
    struct wd_async_udp_packet* packet = sender ? sender->pending_head : NULL;
    while (packet)
    {
        struct wd_async_udp_packet* next = packet->next;
        if (!packet->submitted)
        {
            if (packet->prepared && sender->prepared_submit > 0)
            {
                sender->prepared_submit--;
            }
            wd_async_udp_pending_remove(sender, packet);
            wd_async_udp_packet_release(sender, packet);
        }
        packet = next;
    }
}

static bool wd_async_udp_sender_has_submitted(const struct wd_async_udp_sender* sender) {
    for (const struct wd_async_udp_packet* packet = sender ? sender->pending_head : NULL; packet; packet = packet->next)
    {
        if (packet->submitted)
        {
            return true;
        }
    }
    return false;
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
        wd_async_udp_sender_fail_unsubmitted(sender);
        if (!wd_async_udp_sender_has_submitted(sender))
        {
            break;
        }
        usleep(WD_ASYNC_UDP_DRAIN_SLEEP_US);
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
        fprintf(stderr, "WayDisplay [warn]: async UDP sender destroy timed out; leaking pending ring buffers safely\n");
        return;
    }
    wd_async_udp_sender_fail_unsubmitted(sender);

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
