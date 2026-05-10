#include "waydisplay/wd_net.h"

#include "waydisplay/wd_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

bool wd_send_all(int fd, const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;

    while (size > 0) {
        ssize_t n = send(fd, p, size, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (n == 0) {
            return false;
        }

        p += (size_t)n;
        size -= (size_t)n;
    }

    return true;
}

bool wd_recv_all(int fd, void *data, size_t size) {
    uint8_t *p = (uint8_t *)data;

    while (size > 0) {
        ssize_t n = recv(fd, p, size, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (n == 0) {
            return false;
        }

        p += (size_t)n;
        size -= (size_t)n;
    }

    return true;
}

bool wd_send_tcp_message(int fd,
                         uint16_t message_type,
                         const void *payload,
                         uint32_t payload_size) {
    struct wd_tcp_header header;

    memset(&header, 0, sizeof(header));
    header.magic = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type = message_type;
    header.payload_size = payload_size;

    if (!wd_send_all(fd, &header, sizeof(header))) {
        return false;
    }

    if (payload_size > 0) {
        if (!payload) {
            return false;
        }

        if (!wd_send_all(fd, payload, payload_size)) {
            return false;
        }
    }

    return true;
}

bool wd_recv_tcp_message(int fd,
                         uint16_t *out_message_type,
                         uint8_t **out_payload,
                         uint32_t *out_payload_size) {
    struct wd_tcp_header header;
    uint8_t *payload = NULL;

    if (out_message_type) {
        *out_message_type = 0;
    }

    if (out_payload) {
        *out_payload = NULL;
    }

    if (out_payload_size) {
        *out_payload_size = 0;
    }

    if (!out_message_type || !out_payload || !out_payload_size) {
        return false;
    }

    if (!wd_recv_all(fd, &header, sizeof(header))) {
        return false;
    }

    if (header.magic != WD_TCP_MAGIC ||
        header.protocol_version != WD_PROTOCOL_VERSION) {
        return false;
    }

    if (header.payload_size > 0) {
        payload = (uint8_t *)malloc(header.payload_size);
        if (!payload) {
            return false;
        }

        if (!wd_recv_all(fd, payload, header.payload_size)) {
            free(payload);
            return false;
        }
    }

    *out_message_type = header.message_type;
    *out_payload = payload;
    *out_payload_size = header.payload_size;

    return true;
}

int wd_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
