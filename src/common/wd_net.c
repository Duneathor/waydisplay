#include "waydisplay/wd_net.h"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_protocol_codec.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

bool wd_send_all(int fd, const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;

    while (size > 0)
    {
        ssize_t n = send(fd, p, size, MSG_NOSIGNAL);

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


static void wd_tcp_reader_clear_frame(struct wd_tcp_reader* reader) {
    const uint32_t max_payload_size = reader->max_payload_size;
    memset(reader, 0, sizeof(*reader));
    reader->max_payload_size = max_payload_size;
}

void wd_tcp_reader_init(struct wd_tcp_reader* reader, uint32_t max_payload_size) {
    if (!reader)
    {
        return;
    }

    memset(reader, 0, sizeof(*reader));
    reader->max_payload_size = max_payload_size;
}

void wd_tcp_reader_reset(struct wd_tcp_reader* reader) {
    if (!reader)
    {
        return;
    }

    free(reader->payload);
    wd_tcp_reader_clear_frame(reader);
}

void wd_tcp_reader_destroy(struct wd_tcp_reader* reader) {
    wd_tcp_reader_reset(reader);
}

bool wd_tcp_reader_has_partial_frame(const struct wd_tcp_reader* reader) {
    return reader && (reader->header_size != 0 || reader->payload_received != 0 || reader->payload_size != 0);
}

uint64_t wd_tcp_reader_deadline_ns(const struct wd_tcp_reader* reader) {
    return reader ? reader->deadline_ns : 0;
}

void wd_tcp_message_release(struct wd_tcp_message* message) {
    if (!message)
    {
        return;
    }

    free(message->payload);
    memset(message, 0, sizeof(*message));
}

static void wd_tcp_reader_start_deadline(struct wd_tcp_reader* reader, uint64_t now_ns, uint64_t frame_timeout_ns) {
    if (reader->deadline_ns != 0 || frame_timeout_ns == 0)
    {
        return;
    }

    reader->deadline_ns = UINT64_MAX - now_ns < frame_timeout_ns ? UINT64_MAX : now_ns + frame_timeout_ns;
}

static enum wd_tcp_reader_status wd_tcp_reader_recv_bytes(struct wd_tcp_reader* reader, int fd, uint8_t* destination, size_t* received,
                                                           size_t expected, uint64_t now_ns, uint64_t frame_timeout_ns) {
    while (*received < expected)
    {
        ssize_t count = recv(fd, destination + *received, expected - *received, MSG_DONTWAIT);
        if (count > 0)
        {
            wd_tcp_reader_start_deadline(reader, now_ns, frame_timeout_ns);
            *received += (size_t)count;
            continue;
        }

        if (count == 0)
        {
            return WD_TCP_READER_PEER_CLOSED;
        }

        if (errno == EINTR)
        {
            continue;
        }

        if (errno == EAGAIN)
        {
            break;
        }

        return WD_TCP_READER_IO_ERROR;
    }

    if (*received == expected)
    {
        return WD_TCP_READER_MESSAGE;
    }

    if (reader->deadline_ns != 0 && now_ns >= reader->deadline_ns)
    {
        return WD_TCP_READER_TIMED_OUT;
    }

    return WD_TCP_READER_NEED_MORE;
}

enum wd_tcp_reader_status wd_tcp_reader_receive(struct wd_tcp_reader* reader, int fd, uint64_t now_ns, uint64_t frame_timeout_ns,
                                                struct wd_tcp_message* out_message) {
    if (!reader || fd < 0 || !out_message || reader->max_payload_size == 0)
    {
        return WD_TCP_READER_IO_ERROR;
    }

    memset(out_message, 0, sizeof(*out_message));

    enum wd_tcp_reader_status status = wd_tcp_reader_recv_bytes(reader, fd, reader->header_bytes, &reader->header_size,
                                                                WD_TCP_HEADER_WIRE_SIZE, now_ns, frame_timeout_ns);
    if (status != WD_TCP_READER_MESSAGE)
    {
        return status;
    }

    if (!reader->header_decoded)
    {
        struct wd_tcp_header header;
        if (!wd_tcp_header_decode(reader->header_bytes, &header))
        {
            return WD_TCP_READER_INVALID_FRAME;
        }

        if (header.magic != WD_TCP_MAGIC || header.protocol_version != WD_PROTOCOL_VERSION ||
            header.payload_size > reader->max_payload_size)
        {
            return WD_TCP_READER_INVALID_FRAME;
        }

        reader->message_type  = header.message_type;
        reader->payload_size  = header.payload_size;
        reader->header_decoded = true;
        if (reader->payload_size != 0)
        {
            reader->payload = (uint8_t*)malloc(reader->payload_size);
            if (!reader->payload)
            {
                return WD_TCP_READER_IO_ERROR;
            }
        }
    }

    if (reader->payload_size != 0)
    {
        size_t payload_received = reader->payload_received;
        status = wd_tcp_reader_recv_bytes(reader, fd, reader->payload, &payload_received, reader->payload_size, now_ns, frame_timeout_ns);
        reader->payload_received = (uint32_t)payload_received;
        if (status != WD_TCP_READER_MESSAGE)
        {
            return status;
        }
    }

    if (!wd_protocol_payload_validate(reader->message_type, reader->payload, reader->payload_size))
    {
        return WD_TCP_READER_INVALID_FRAME;
    }

    out_message->message_type = reader->message_type;
    out_message->payload      = reader->payload;
    out_message->payload_size = reader->payload_size;
    reader->payload           = NULL;
    wd_tcp_reader_clear_frame(reader);
    return WD_TCP_READER_MESSAGE;
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

        ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);

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
    uint32_t wire_payload_size = 0;
    if (!wd_protocol_payload_wire_size(message_type, payload, payload_size, &wire_payload_size))
    {
        return false;
    }

    struct wd_tcp_header header;
    memset(&header, 0, sizeof(header));
    header.magic            = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = message_type;
    header.payload_size     = wire_payload_size;

    uint8_t wire_header[WD_TCP_HEADER_WIRE_SIZE];
    if (!wd_tcp_header_encode(wire_header, &header))
    {
        return false;
    }

    struct iovec iov[2];
    iov[0].iov_base = wire_header;
    iov[0].iov_len  = sizeof(wire_header);
    iov[1].iov_base = (void*)payload;
    iov[1].iov_len  = wire_payload_size;

    return wd_writev_all(fd, iov, wire_payload_size == 0 ? 1 : 2);
}

bool wd_recv_tcp_message_limited(int fd, uint32_t max_payload_size, uint16_t* out_message_type, uint8_t** out_payload,
                                 uint32_t* out_payload_size) {
    uint8_t  wire_header[WD_TCP_HEADER_WIRE_SIZE];
    uint8_t* wire_payload = NULL;

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
    if (!out_message_type || !out_payload || !out_payload_size || max_payload_size == 0)
    {
        return false;
    }

    if (!wd_recv_all(fd, wire_header, sizeof(wire_header)))
    {
        return false;
    }

    struct wd_tcp_header header;
    if (!wd_tcp_header_decode(wire_header, &header) || header.magic != WD_TCP_MAGIC ||
        header.protocol_version != WD_PROTOCOL_VERSION || header.payload_size > max_payload_size)
    {
        return false;
    }

    if (header.payload_size != 0)
    {
        wire_payload = (uint8_t*)malloc(header.payload_size);
        if (!wire_payload || !wd_recv_all(fd, wire_payload, header.payload_size))
        {
            free(wire_payload);
            return false;
        }
    }

    if (!wd_protocol_payload_validate(header.message_type, wire_payload, header.payload_size))
    {
        free(wire_payload);
        return false;
    }

    *out_message_type = header.message_type;
    *out_payload      = wire_payload;
    *out_payload_size = header.payload_size;
    return true;
}

bool wd_recv_tcp_message(int fd, uint16_t* out_message_type, uint8_t** out_payload, uint32_t* out_payload_size) {
    return wd_recv_tcp_message_limited(fd, WD_TCP_MAX_PAYLOAD_SIZE, out_message_type, out_payload, out_payload_size);
}
