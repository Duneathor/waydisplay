#include "waydisplay/wd_buffer.h"
#include "wd_async_udp.h"

#include <arpa/inet.h>
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

struct completion_probe {
    unsigned calls;
    bool success;
};

static void release_external(void* user_data, uint8_t* data, size_t size) {
    struct release_probe* probe = user_data;
    (void)size;
    probe->calls++;
    free(data);
}

static void complete(void* user_data, bool success) {
    struct completion_probe* probe = user_data;
    probe->calls++;
    probe->success = success;
}

static struct wd_buffer* make_owner(size_t size, struct release_probe* probe) {
    uint8_t* data = malloc(size);
    if (!data)
    {
        return NULL;
    }
    for (size_t i = 0; i < size; ++i)
    {
        data[i] = (uint8_t)(i * 29u + 3u);
    }
    struct wd_buffer* owner = wd_buffer_wrap(data, size, release_external, probe);
    if (!owner)
    {
        free(data);
    }
    return owner;
}

static bool configure_receiver(int* fd, struct sockaddr_in* address) {
    *fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(*fd >= 0);
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address->sin_port = 0;
    CHECK(bind(*fd, (const struct sockaddr*)address, sizeof(*address)) == 0);
    socklen_t length = sizeof(*address);
    CHECK(getsockname(*fd, (struct sockaddr*)address, &length) == 0);
    const struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    CHECK(setsockopt(*fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    return true;
}

static bool test_owned_udp_slice(void) {
    struct wd_async_udp_sender* sender = NULL;
    if (!wd_async_udp_sender_create(&sender, 8))
    {
        return true;
    }
    int tx = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(tx >= 0);
    int rx = -1;
    struct sockaddr_in destination;
    CHECK(configure_receiver(&rx, &destination));

    const uint8_t header[] = {0x57, 0x44, 0x10, 0x20, 0x30, 0x40, 0x50};
    struct release_probe release = {0};
    struct completion_probe completion = {0};
    struct wd_buffer* owner = make_owner(300, &release);
    CHECK(owner != NULL);

    const size_t offset = 17;
    const uint32_t payload_size = 211;
    CHECK(wd_async_udp_send_packet_owned(sender, tx, &destination, header, sizeof(header),
                                         owner, offset, payload_size, complete, &completion) ==
          WD_ASYNC_UDP_SEND_QUEUED);
    wd_buffer_release(owner);
    CHECK(release.calls == 0);
    CHECK(wd_async_udp_sender_flush(sender));

    uint8_t received[sizeof(header) + 211] = {0};
    const ssize_t received_size = recv(rx, received, sizeof(received), 0);
    CHECK(received_size == (ssize_t)sizeof(received));
    CHECK(memcmp(received, header, sizeof(header)) == 0);
    for (size_t i = 0; i < payload_size; ++i)
    {
        CHECK(received[sizeof(header) + i] == (uint8_t)((offset + i) * 29u + 3u));
    }

    for (unsigned i = 0; i < 1000 && completion.calls == 0; ++i)
    {
        wd_async_udp_sender_reap(sender);
        usleep(1000);
    }
    CHECK(completion.calls == 1);
    CHECK(completion.success);
    CHECK(release.calls == 1);
    CHECK(wd_async_udp_sender_pending_packets(sender) == 0);

    wd_async_udp_sender_destroy(sender);
    close(tx);
    close(rx);
    return true;
}

static bool test_invalid_slice_keeps_caller_owner(void) {
    struct wd_async_udp_sender* sender = NULL;
    if (!wd_async_udp_sender_create(&sender, 8))
    {
        return true;
    }
    int tx = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(tx >= 0);
    struct sockaddr_in destination = {0};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(9);

    struct release_probe release = {0};
    struct wd_buffer* owner = make_owner(32, &release);
    CHECK(owner != NULL);
    const uint8_t header[4] = {1, 2, 3, 4};
    CHECK(wd_async_udp_send_packet_owned(sender, tx, &destination, header, sizeof(header),
                                         owner, 20, 20, NULL, NULL) == WD_ASYNC_UDP_SEND_FAILED);
    CHECK(release.calls == 0);
    wd_buffer_release(owner);
    CHECK(release.calls == 1);

    wd_async_udp_sender_destroy(sender);
    close(tx);
    return true;
}

int main(void) {
    struct wd_async_udp_sender* probe = NULL;
    if (!wd_async_udp_sender_create(&probe, 8))
    {
        return 77;
    }
    wd_async_udp_sender_destroy(probe);

    if (!test_owned_udp_slice() || !test_invalid_slice_keeps_caller_owner())
    {
        return 1;
    }
    return 0;
}
