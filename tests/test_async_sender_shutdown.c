#include "waydisplay/wd_protocol.h"
#include "wd_async_tcp.h"
#include "wd_async_udp.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST_VIDEO_BYTES (512u * 1024u)

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                          \
            exit(1);                                                                                                                       \
        }                                                                                                                                  \
    } while (0)

struct completion_probe {
    unsigned calls;
    bool     success;
};

struct owner_release_probe {
    unsigned calls;
};

static void release_owner(void* user_data, uint8_t* data, size_t size) {
    struct owner_release_probe* probe = user_data;
    (void)size;
    probe->calls++;
    free(data);
}

static void tcp_complete(void* data, bool success) {
    struct completion_probe* probe = data;
    probe->calls++;
    probe->success = success;
}

static void udp_complete(void* data, bool success) {
    struct completion_probe* probe = data;
    probe->calls++;
    probe->success = success;
}

static int test_tcp_forced_teardown(void) {
    struct wd_async_tcp_sender* sender = NULL;
    if (!wd_async_tcp_sender_create(&sender, 8))
    {
        return 77;
    }

    int sockets[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    int send_buffer_size = 4096;
    CHECK(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)) == 0);

    struct completion_probe probe = {0};
    const uint8_t malformed_payload[32] = {0};
    CHECK(!wd_async_tcp_send_message_ex(sender, sockets[0], WD_MSG_SERVER_CONFIG, malformed_payload,
                                        sizeof(malformed_payload), tcp_complete, &probe));
    CHECK(probe.calls == 0);

    const uint32_t payload_size = (uint32_t)sizeof(struct wd_video_frame_payload_header) + TEST_VIDEO_BYTES;
    uint8_t*       payload      = calloc(1, payload_size);
    CHECK(payload != NULL);
    struct wd_video_frame_payload_header* header = (struct wd_video_frame_payload_header*)payload;
    header->data_size                             = TEST_VIDEO_BYTES;

    CHECK(wd_async_tcp_send_message_ex(sender, sockets[0], WD_MSG_VIDEO_FRAME, payload, payload_size, tcp_complete, &probe));
    free(payload);

    /* Prepared video messages own their wire buffer through a pending send,
     * including a sender shutdown with an in-flight message ahead of them. */
    void* prepared_payload = NULL;
    struct wd_async_tcp_message* prepared = wd_async_tcp_prepare_message(WD_MSG_VIDEO_FRAME, payload_size,
                                                                         &prepared_payload);
    CHECK(prepared != NULL && prepared_payload != NULL);
    memset(prepared_payload, 0, payload_size);
    ((struct wd_video_frame_payload_header*)prepared_payload)->data_size = TEST_VIDEO_BYTES;
    CHECK(wd_async_tcp_send_prepared_message(sender, sockets[0], prepared));

    /* An owned payload queued behind the blocked send must retain storage
     * until shutdown removes the message, then release it exactly once. */
    struct owner_release_probe owner_probe = {0};
    struct completion_probe   owned_completion = {0};
    uint8_t*                   owned_bytes = malloc(4096);
    CHECK(owned_bytes != NULL);
    memset(owned_bytes, 0x5a, 4096);
    struct wd_buffer* owned = wd_buffer_wrap(owned_bytes, 4096, release_owner, &owner_probe);
    CHECK(owned != NULL);
    struct wd_video_frame_payload_header owned_header = {0};
    owned_header.data_size = 4096;
    CHECK(wd_async_tcp_send_owned_message_ex(sender, sockets[0], WD_MSG_VIDEO_FRAME,
                                             &owned_header, sizeof(owned_header), owned, 0, 4096,
                                             tcp_complete, &owned_completion));
    wd_buffer_release(owned);
    CHECK(owner_probe.calls == 0);

    void* malformed_prepared_payload = NULL;
    prepared = wd_async_tcp_prepare_message(WD_MSG_SERVER_CONFIG, sizeof(malformed_payload), &malformed_prepared_payload);
    CHECK(prepared != NULL && malformed_prepared_payload != NULL);
    CHECK(!wd_async_tcp_send_prepared_message(sender, sockets[0], prepared));

    wd_async_tcp_sender_destroy(sender);
    CHECK(probe.calls == 1);
    CHECK(!probe.success);
    CHECK(owned_completion.calls == 1);
    CHECK(!owned_completion.success);
    CHECK(owner_probe.calls == 1);

    close(sockets[0]);
    close(sockets[1]);
    return 0;
}

static int test_udp_forced_teardown(void) {
    struct wd_async_udp_sender* sender = NULL;
    if (!wd_async_udp_sender_create(&sender, 8))
    {
        return 77;
    }

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);

    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family      = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port        = htons(9);

    const uint8_t              header[8] = {0};
    struct completion_probe    probe     = {0};
    struct owner_release_probe owner_probe = {0};
    uint8_t* owned_bytes = malloc(32);
    CHECK(owned_bytes != NULL);
    memset(owned_bytes, 0xa5, 32);
    struct wd_buffer* owned = wd_buffer_wrap(owned_bytes, 32, release_owner, &owner_probe);
    CHECK(owned != NULL);
    CHECK(wd_async_udp_send_packet_owned(sender, fd, &destination, header, sizeof(header),
                                         owned, 8, 16, udp_complete, &probe) ==
          WD_ASYNC_UDP_SEND_QUEUED);
    wd_buffer_release(owned);
    CHECK(owner_probe.calls == 0);
    CHECK(wd_async_udp_sender_flush(sender));

    wd_async_udp_sender_destroy(sender);
    CHECK(probe.calls == 1);
    CHECK(!probe.success);
    CHECK(owner_probe.calls == 1);

    close(fd);
    return 0;
}

int main(void) {
    int rc = test_tcp_forced_teardown();
    if (rc != 0)
    {
        return rc;
    }
    return test_udp_forced_teardown();
}
