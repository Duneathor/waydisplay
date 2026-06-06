#include "waydisplay/wd_net.h"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define WD_SEND_FLAGS MSG_NOSIGNAL
#else
#define WD_SEND_FLAGS 0
#endif

bool wd_send_all(int fd, const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;

    while (size > 0)
    {
        ssize_t n = send(fd, p, size, WD_SEND_FLAGS);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return false;
        }

        if (n == 0)
        {
            return false;
        }

        p += (size_t)n;
        size -= (size_t)n;
    }

    return true;
}

bool wd_recv_all(int fd, void* data, size_t size) {
    uint8_t* p = (uint8_t*)data;

    while (size > 0)
    {
        ssize_t n = recv(fd, p, size, 0);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return false;
        }

        if (n == 0)
        {
            return false;
        }

        p += (size_t)n;
        size -= (size_t)n;
    }

    return true;
}

static bool wd_writev_all(int fd, struct iovec* iov, int iovcnt) {
    while (iovcnt > 0)
    {
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov    = iov;
        msg.msg_iovlen = iovcnt;

        ssize_t n = sendmsg(fd, &msg, WD_SEND_FLAGS);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return false;
        }

        if (n == 0)
        {
            return false;
        }

        size_t written = (size_t)n;
        while (iovcnt > 0 && written >= iov[0].iov_len)
        {
            written -= iov[0].iov_len;
            ++iov;
            --iovcnt;
        }

        if (iovcnt > 0 && written > 0)
        {
            iov[0].iov_base = (uint8_t*)iov[0].iov_base + written;
            iov[0].iov_len -= written;
        }
    }

    return true;
}

bool wd_send_tcp_message(int fd, uint16_t message_type, const void* payload, uint32_t payload_size) {
    struct wd_tcp_header header;

    memset(&header, 0, sizeof(header));
    header.magic            = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = message_type;
    header.payload_size     = payload_size;

    if (payload_size == 0)
    {
        return wd_send_all(fd, &header, sizeof(header));
    }

    if (!payload)
    {
        return false;
    }

    struct iovec iov[2];
    iov[0].iov_base = &header;
    iov[0].iov_len  = sizeof(header);
    iov[1].iov_base = (void*)payload;
    iov[1].iov_len  = payload_size;

    return wd_writev_all(fd, iov, 2);
}

bool wd_recv_tcp_message(int fd, uint16_t* out_message_type, uint8_t** out_payload, uint32_t* out_payload_size) {
    struct wd_tcp_header header;
    uint8_t*             payload = NULL;

    if (out_message_type)
    {
        *out_message_type = 0;
    }

    if (out_payload)
    {
        *out_payload = NULL;
    }

    if (out_payload_size)
    {
        *out_payload_size = 0;
    }

    if (!out_message_type || !out_payload || !out_payload_size)
    {
        return false;
    }

    if (!wd_recv_all(fd, &header, sizeof(header)))
    {
        return false;
    }

    if (header.magic != WD_TCP_MAGIC || header.protocol_version != WD_PROTOCOL_VERSION)
    {
        return false;
    }

    if (header.payload_size > WD_TCP_MAX_PAYLOAD_SIZE)
    {
        return false;
    }

    if (header.payload_size > 0)
    {
        payload = (uint8_t*)malloc(header.payload_size);
        if (!payload)
        {
            return false;
        }

        if (!wd_recv_all(fd, payload, header.payload_size))
        {
            free(payload);
            return false;
        }
    }

    *out_message_type = header.message_type;
    *out_payload      = payload;
    *out_payload_size = header.payload_size;

    return true;
}

int wd_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
    {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
