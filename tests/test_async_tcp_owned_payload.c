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

static struct wd_buffer* make_hevc_regression_buffer(size_t size, struct release_probe* probe) {
    uint8_t* data = malloc(size);
    if (!data)
    {
        return NULL;
    }

    /*
     * Keep this deterministic but intentionally hostile to framing bugs:
     * the body contains many 0x00/0x01/0x03 values, while the beginning has
     * recognizable HEVC Annex-B VPS/SPS/PPS/IDR-style NAL prefixes.
     */
    uint32_t state = UINT32_C(0x6d2b79f5);
    for (size_t i = 0; i < size; ++i)
    {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        data[i] = (uint8_t)(state >> 24u);
    }

    static const uint8_t hevc_prefix[] = {
        0x00, 0x00, 0x00, 0x01, 0x40, 0x01, /* VPS */
        0x0c, 0x01, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x01, 0x42, 0x01, /* SPS */
        0x01, 0x60, 0x00, 0x00, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x44, 0x01, /* PPS */
        0xc0, 0x73, 0xc0, 0x89,
        0x00, 0x00, 0x00, 0x01, 0x26, 0x01, /* IDR */
    };
    if (size < sizeof(hevc_prefix) + 16u)
    {
        free(data);
        return NULL;
    }
    memcpy(data, hevc_prefix, sizeof(hevc_prefix));

    /* Preserve emulation-prevention-shaped bytes exactly across transport. */
    const size_t marker = size / 2u;
    const uint8_t escaped_sequences[] = {
        0x00, 0x00, 0x03, 0x00,
        0x00, 0x00, 0x03, 0x01,
        0x00, 0x00, 0x03, 0x02,
        0x00, 0x00, 0x03, 0x03,
    };
    memcpy(data + marker, escaped_sequences, sizeof(escaped_sequences));

    struct wd_buffer* buffer = wd_buffer_wrap(data, size, release_external, probe);
    if (!buffer)
    {
        free(data);
    }
    return buffer;
}

static bool recv_exact_with_progress(struct wd_async_tcp_sender* sender, int fd,
                                     void* destination, size_t size) {
    uint8_t* bytes = destination;
    size_t received = 0;
    for (unsigned attempts = 0; received < size && attempts < 2000; ++attempts)
    {
        const ssize_t rc = recv(fd, bytes + received, size - received, MSG_DONTWAIT);
        if (rc > 0)
        {
            received += (size_t)rc;
            continue;
        }
        if (rc < 0 && errno == EINTR)
        {
            continue;
        }
        if (rc < 0 && errno == EAGAIN)
        {
            /*
             * A stream send may complete short even for a small message.
             * Reap only to make progress on a genuine short-send completion;
             * the deterministic policy test separately verifies that the
             * inline prefix and retained payload share the initial submission.
             */
            wd_async_tcp_sender_reap(sender);
            usleep(1000);
            continue;
        }
        return false;
    }
    return received == size;
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
     * The owned-send policy test proves that header/prefix plus retained
     * payload are planned in the same initial submission. This integration
     * test still permits a legitimate short stream send by reaping CQEs only
     * when the receiver would otherwise block.
     */
    uint8_t wire_header[WD_TCP_HEADER_WIRE_SIZE] = {0};
    CHECK(recv_exact_with_progress(sender, sockets[1], wire_header, sizeof(wire_header)));
    struct wd_tcp_header decoded_header = {0};
    CHECK(wd_tcp_header_decode(wire_header, &decoded_header));
    CHECK(decoded_header.message_type == WD_MSG_VIDEO_FRAME);
    CHECK(decoded_header.payload_size == sizeof(prefix) + data_size);

    struct wd_video_frame_payload_header received_prefix = {0};
    CHECK(recv_exact_with_progress(sender, sockets[1], &received_prefix, sizeof(received_prefix)));
    CHECK(memcmp(&received_prefix, &prefix, sizeof(prefix)) == 0);

    uint8_t received_payload[257] = {0};
    CHECK(recv_exact_with_progress(sender, sockets[1], received_payload, sizeof(received_payload)));
    for (size_t i = 0; i < sizeof(received_payload); ++i)
    {
        CHECK(received_payload[i] == (uint8_t)((offset + i) * 37u + 11u));
    }

    reap_until_idle(sender);
    CHECK(wd_async_tcp_sender_pending_bytes(sender) == 0);
    CHECK(wd_async_tcp_sender_completed(sender) == 1);
    CHECK(wd_async_tcp_sender_failed(sender) == 0);
    /*
     * A nonzero partial-resubmit count is valid here: it reflects a genuine
     * short kernel send, not an intentional prefix->payload boundary.
     */
    CHECK(probe.calls == 1);

    wd_async_tcp_sender_destroy(sender);
    close(sockets[0]);
    close(sockets[1]);
    return true;
}

static bool test_hevc_owned_payload_wire_integrity(void) {
    struct wd_async_tcp_sender* sender = NULL;
    if (!wd_async_tcp_sender_create(&sender, 8))
    {
        return true;
    }

    int sockets[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    /*
     * Make the sender's socket buffer deliberately small relative to the
     * access unit. Kernels are still free to complete the send in one CQE,
     * but when they complete it short this exercises resumption across every
     * logical segment without changing the correctness requirement.
     */
    int send_buffer_size = 4096;
    CHECK(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size,
                     sizeof(send_buffer_size)) == 0);

    const uint32_t data_size = 512u * 1024u;
    struct release_probe probe = {0};
    struct wd_buffer* owner = make_hevc_regression_buffer(data_size, &probe);
    CHECK(owner != NULL);

    const uint8_t* original = wd_buffer_const_data(owner);
    CHECK(original != NULL);
    uint8_t* expected = malloc(data_size);
    CHECK(expected != NULL);
    memcpy(expected, original, data_size);

    struct wd_video_frame_payload_header prefix = {0};
    prefix.session_id       = 23;
    prefix.connection_token = UINT64_C(0x8877665544332211);
    prefix.content_epoch    = 19;
    prefix.codec            = WD_VIDEO_CODEC_H265;
    prefix.flags            = WD_VIDEO_FRAME_KEYFRAME | WD_VIDEO_FRAME_CONFIG;
    prefix.frame_id         = 901;
    prefix.pts_usec         = UINT64_C(123456789);
    prefix.width            = 3840;
    prefix.height           = 2160;
    prefix.coded_width      = 3840;
    prefix.coded_height     = 2160;
    prefix.data_size        = data_size;

    CHECK(wd_async_tcp_send_owned_message(sender, sockets[0], WD_MSG_VIDEO_FRAME,
                                          &prefix, sizeof(prefix), owner, 0, data_size));

    /* The sender must be the only remaining owner while bytes are in flight. */
    wd_buffer_release(owner);
    owner = NULL;
    CHECK(probe.calls == 0);

    uint8_t wire_header[WD_TCP_HEADER_WIRE_SIZE] = {0};
    CHECK(recv_exact_with_progress(sender, sockets[1], wire_header, sizeof(wire_header)));

    struct wd_tcp_header decoded_header = {0};
    CHECK(wd_tcp_header_decode(wire_header, &decoded_header));
    CHECK(decoded_header.message_type == WD_MSG_VIDEO_FRAME);
    CHECK(decoded_header.payload_size == sizeof(prefix) + data_size);

    struct wd_video_frame_payload_header received_prefix = {0};
    CHECK(recv_exact_with_progress(sender, sockets[1], &received_prefix, sizeof(received_prefix)));
    CHECK(memcmp(&received_prefix, &prefix, sizeof(prefix)) == 0);

    uint8_t* received = malloc(data_size);
    CHECK(received != NULL);
    CHECK(recv_exact_with_progress(sender, sockets[1], received, data_size));
    CHECK(memcmp(received, expected, data_size) == 0);

    free(received);
    free(expected);

    reap_until_idle(sender);
    CHECK(wd_async_tcp_sender_pending_bytes(sender) == 0);
    CHECK(wd_async_tcp_sender_completed(sender) == 1);
    CHECK(wd_async_tcp_sender_failed(sender) == 0);
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

static bool test_sender_rotation_isolates_session_state(void) {
    struct wd_async_tcp_sender* old_sender = NULL;
    if (!wd_async_tcp_sender_create(&old_sender, 8))
    {
        return true;
    }

    int old_sockets[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, old_sockets) == 0);
    int send_buffer = 4096;
    CHECK(setsockopt(old_sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);

    const uint32_t large_size = 512u * 1024u;
    struct release_probe old_probe = {0};
    struct wd_buffer* old_owner = make_external_buffer(large_size, &old_probe);
    CHECK(old_owner != NULL);
    struct wd_video_frame_payload_header old_prefix = {0};
    old_prefix.session_id = 1;
    old_prefix.content_epoch = 1;
    old_prefix.codec = WD_VIDEO_CODEC_H265;
    old_prefix.frame_id = 1;
    old_prefix.width = 1920;
    old_prefix.height = 1080;
    old_prefix.coded_width = 1920;
    old_prefix.coded_height = 1080;
    old_prefix.data_size = large_size;
    CHECK(wd_async_tcp_send_owned_message(old_sender, old_sockets[0], WD_MSG_VIDEO_FRAME,
                                          &old_prefix, sizeof(old_prefix), old_owner, 0, large_size));
    wd_buffer_release(old_owner);
    CHECK(old_probe.calls == 0);

    /* A session boundary must destroy the old sender rather than reuse its
     * pending CQEs/counters for the next connection. */
    wd_async_tcp_sender_destroy(old_sender);
    old_sender = NULL;
    CHECK(old_probe.calls == 1);
    close(old_sockets[0]);
    close(old_sockets[1]);

    struct wd_async_tcp_sender* new_sender = NULL;
    CHECK(wd_async_tcp_sender_create(&new_sender, 8));
    int new_sockets[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, new_sockets) == 0);

    struct release_probe new_probe = {0};
    struct wd_buffer* new_owner = make_external_buffer(64, &new_probe);
    CHECK(new_owner != NULL);
    struct wd_video_frame_payload_header new_prefix = {0};
    new_prefix.session_id = 2;
    new_prefix.content_epoch = 2;
    new_prefix.codec = WD_VIDEO_CODEC_H265;
    new_prefix.frame_id = 1;
    new_prefix.width = 64;
    new_prefix.height = 64;
    new_prefix.coded_width = 64;
    new_prefix.coded_height = 64;
    new_prefix.data_size = 64;
    CHECK(wd_async_tcp_send_owned_message(new_sender, new_sockets[0], WD_MSG_VIDEO_FRAME,
                                          &new_prefix, sizeof(new_prefix), new_owner, 0, 64));
    wd_buffer_release(new_owner);

    uint8_t wire_header[WD_TCP_HEADER_WIRE_SIZE] = {0};
    CHECK(recv_exact_with_progress(new_sender, new_sockets[1], wire_header, sizeof(wire_header)));
    struct wd_tcp_header decoded_header = {0};
    CHECK(wd_tcp_header_decode(wire_header, &decoded_header));
    CHECK(decoded_header.message_type == WD_MSG_VIDEO_FRAME);
    CHECK(decoded_header.payload_size == sizeof(new_prefix) + 64);
    struct wd_video_frame_payload_header received_prefix = {0};
    CHECK(recv_exact_with_progress(new_sender, new_sockets[1], &received_prefix, sizeof(received_prefix)));
    CHECK(received_prefix.session_id == 2);
    uint8_t received_payload[64] = {0};
    CHECK(recv_exact_with_progress(new_sender, new_sockets[1], received_payload, sizeof(received_payload)));

    reap_until_idle(new_sender);
    CHECK(wd_async_tcp_sender_completed(new_sender) == 1);
    CHECK(wd_async_tcp_sender_failed(new_sender) == 0);
    CHECK(new_probe.calls == 1);

    wd_async_tcp_sender_destroy(new_sender);
    close(new_sockets[0]);
    close(new_sockets[1]);
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
        !test_hevc_owned_payload_wire_integrity() ||
        !test_invalid_ranges_and_overflow_release_temporary_refs() ||
        !test_drop_unsubmitted_releases_owner() ||
        !test_sender_rotation_isolates_session_state())
    {
        return 1;
    }
    return 0;
}
