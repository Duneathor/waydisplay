#include "waydisplay/wd_buffer.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_protocol_codec.h"
#include "wd_async_tcp.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                        \
            return false;                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

struct release_probe {
    unsigned calls;
};

static void release_external(void* user_data, uint8_t* data, size_t size) {
    struct release_probe* probe = user_data;
    (void)size;
    probe->calls++;
    free(data);
}

static struct wd_buffer* make_external_buffer(size_t size, struct release_probe* probe) {
    uint8_t* data = malloc(size);
    if (!data)
    {
        return NULL;
    }
    for (size_t i = 0; i < size; ++i)
    {
        data[i] = (uint8_t)(i * 37u + 11u);
    }
    struct wd_buffer* buffer = wd_buffer_wrap(data, size, release_external, probe);
    if (!buffer)
    {
        free(data);
    }
    return buffer;
}

static bool recv_exact(int fd, void* destination, size_t size) {
    uint8_t* bytes = destination;
    size_t received = 0;
    while (received < size)
    {
        const ssize_t rc = recv(fd, bytes + received, size - received, 0);
        if (rc > 0)
        {
            received += (size_t)rc;
            continue;
        }
        if (rc < 0 && errno == EINTR)
        {
            continue;
        }
        return false;
    }
    return true;
}

static void reap_until_idle(struct wd_async_tcp_sender* sender) {
    for (unsigned i = 0; i < 1000 && wd_async_tcp_sender_pending_bytes(sender) != 0; ++i)
    {
        wd_async_tcp_sender_reap(sender);
        usleep(1000);
    }
    wd_async_tcp_sender_reap(sender);
}

static bool test_owned_slice_wire_and_lifetime(void) {
    struct wd_async_tcp_sender* sender = NULL;
    if (!wd_async_tcp_sender_create(&sender, 8))
    {
        return true; /* caller maps unavailable io_uring to skip */
    }

    int sockets[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    const struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    CHECK(setsockopt(sockets[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    struct release_probe probe = {0};
    struct wd_buffer* owner = make_external_buffer(512, &probe);
    CHECK(owner != NULL);
    const size_t offset = 19;
    const uint32_t data_size = 257;

    struct wd_video_frame_payload_header prefix = {0};
    prefix.session_id       = 9;
    prefix.connection_token = UINT64_C(0x123456789abcdef0);
    prefix.content_epoch    = 4;
    prefix.codec            = WD_VIDEO_CODEC_H264;
    prefix.frame_id         = 77;
    prefix.pts_usec         = 987654;
    prefix.width            = 65;
    prefix.height           = 49;
    prefix.coded_width      = 66;
    prefix.coded_height     = 50;
    prefix.data_size        = data_size;

    CHECK(wd_async_tcp_send_owned_message(sender, sockets[0], WD_MSG_VIDEO_FRAME, &prefix, sizeof(prefix),
                                          owner, offset, data_size));
    /* Sender owns a reference from this point forward. */
    wd_buffer_release(owner);
    owner = NULL;
    CHECK(probe.calls == 0);

    /*
     * Deliberately do not reap the sender before receiving. A small owned
     * message must submit header/prefix plus retained payload in one operation;
     * otherwise the receiver stalls at the segment boundary waiting for the
     * sender to observe the first CQE.
     */
    uint8_t wire_header[WD_TCP_HEADER_WIRE_SIZE] = {0};
    CHECK(recv_exact(sockets[1], wire_header, sizeof(wire_header)));
    struct wd_tcp_header decoded_header = {0};
    CHECK(wd_tcp_header_decode(wire_header, &decoded_header));
    CHECK(decoded_header.message_type == WD_MSG_VIDEO_FRAME);
    CHECK(decoded_header.payload_size == sizeof(prefix) + data_size);

    struct wd_video_frame_payload_header received_prefix = {0};
    CHECK(recv_exact(sockets[1], &received_prefix, sizeof(received_prefix)));
    CHECK(memcmp(&received_prefix, &prefix, sizeof(prefix)) == 0);

    uint8_t received_payload[257] = {0};
    CHECK(recv_exact(sockets[1], received_payload, sizeof(received_payload)));
    for (size_t i = 0; i < sizeof(received_payload); ++i)
    {
        CHECK(received_payload[i] == (uint8_t)((offset + i) * 37u + 11u));
    }

    reap_until_idle(sender);
    CHECK(wd_async_tcp_sender_pending_bytes(sender) == 0);
    CHECK(wd_async_tcp_sender_completed(sender) == 1);
    CHECK(wd_async_tcp_sender_failed(sender) == 0);
    /* The prefix->payload boundary is intentional, not a partial send. */
    CHECK(wd_async_tcp_sender_partial_resubmits(sender) == 0);
    CHECK(probe.calls == 1);

    wd_async_tcp_sender_destroy(sender);
    close(sockets[0]);
    close(sockets[1]);
    return true;
}

static bool test_invalid_ranges_and_overflow_release_temporary_refs(void) {
    struct wd_async_tcp_sender* sender = NULL;
    if (!wd_async_tcp_sender_create(&sender, 8))
    {
        return true;
    }
    int sockets[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    struct wd_video_frame_payload_header prefix = {0};
    prefix.data_size = 32;

    struct release_probe invalid_probe = {0};
    struct wd_buffer* invalid_owner = make_external_buffer(32, &invalid_probe);
    CHECK(invalid_owner != NULL);
    CHECK(!wd_async_tcp_send_owned_message(sender, sockets[0], WD_MSG_VIDEO_FRAME, &prefix, sizeof(prefix),
                                           invalid_owner, 16, 32));
    CHECK(invalid_probe.calls == 0);
    wd_buffer_release(invalid_owner);
    CHECK(invalid_probe.calls == 1);

    struct release_probe overflow_probe = {0};
    struct wd_buffer* overflow_owner = make_external_buffer(64, &overflow_probe);
    CHECK(overflow_owner != NULL);
    wd_async_tcp_sender_set_max_pending_bytes(sender, 1);
    CHECK(!wd_async_tcp_send_owned_message(sender, sockets[0], WD_MSG_VIDEO_FRAME, &prefix, sizeof(prefix),
                                           overflow_owner, 0, 32));
    /* Failed enqueue dropped only the sender's temporary retain. */
    CHECK(overflow_probe.calls == 0);
    wd_buffer_release(overflow_owner);
    CHECK(overflow_probe.calls == 1);
    CHECK(wd_async_tcp_sender_overflows(sender) == 1);

    wd_async_tcp_sender_destroy(sender);
    close(sockets[0]);
    close(sockets[1]);
    return true;
}

static bool test_drop_unsubmitted_releases_owner(void) {
    struct wd_async_tcp_sender* sender = NULL;
    if (!wd_async_tcp_sender_create(&sender, 8))
    {
        return true;
    }
    int sockets[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    int send_buffer = 4096;
    CHECK(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);

    const uint32_t large_size = 512u * 1024u;
    const uint32_t first_size = (uint32_t)sizeof(struct wd_video_frame_payload_header) + large_size;
    uint8_t* first = calloc(1, first_size);
    CHECK(first != NULL);
    ((struct wd_video_frame_payload_header*)first)->data_size = large_size;
    CHECK(wd_async_tcp_send_message(sender, sockets[0], WD_MSG_VIDEO_FRAME, first, first_size));
    free(first);

    struct release_probe probe = {0};
    struct wd_buffer* owner = make_external_buffer(64, &probe);
    CHECK(owner != NULL);
    struct wd_video_frame_payload_header prefix = {0};
    prefix.data_size = 64;
    CHECK(wd_async_tcp_send_owned_message(sender, sockets[0], WD_MSG_VIDEO_FRAME, &prefix, sizeof(prefix),
                                          owner, 0, 64));
    wd_buffer_release(owner);
    CHECK(probe.calls == 0);

    CHECK(wd_async_tcp_sender_drop_message_type(sender, WD_MSG_VIDEO_FRAME) == 1);
    CHECK(probe.calls == 1);

    wd_async_tcp_sender_destroy(sender);
    close(sockets[0]);
    close(sockets[1]);
    return true;
}

int main(void) {
    struct wd_async_tcp_sender* probe = NULL;
    if (!wd_async_tcp_sender_create(&probe, 8))
    {
        return 77;
    }
    wd_async_tcp_sender_destroy(probe);

    if (!test_owned_slice_wire_and_lifetime() ||
        !test_invalid_ranges_and_overflow_release_temporary_refs() ||
        !test_drop_unsubmitted_releases_owner())
    {
        return 1;
    }
    return 0;
}
