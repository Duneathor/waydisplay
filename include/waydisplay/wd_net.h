#pragma once

#include "waydisplay/wd_protocol_codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_tcp_reader_status {
    WD_TCP_READER_NEED_MORE = 0,
    WD_TCP_READER_MESSAGE,
    WD_TCP_READER_PEER_CLOSED,
    WD_TCP_READER_INVALID_FRAME,
    WD_TCP_READER_IO_ERROR,
    WD_TCP_READER_TIMED_OUT,
};

struct wd_tcp_message {
    uint16_t message_type;
    uint8_t* payload;
    uint32_t payload_size;
};

struct wd_tcp_reader {
    uint8_t  header_bytes[WD_TCP_HEADER_WIRE_SIZE];
    size_t   header_size;
    uint8_t* payload;
    uint32_t payload_size;
    uint32_t payload_received;
    uint32_t max_payload_size;
    uint16_t message_type;
    uint64_t deadline_ns;
    bool     header_decoded;
};

void wd_tcp_reader_init(struct wd_tcp_reader* reader, uint32_t max_payload_size);
void wd_tcp_reader_reset(struct wd_tcp_reader* reader);
void wd_tcp_reader_destroy(struct wd_tcp_reader* reader);
bool wd_tcp_reader_has_partial_frame(const struct wd_tcp_reader* reader);
uint64_t wd_tcp_reader_deadline_ns(const struct wd_tcp_reader* reader);
enum wd_tcp_reader_status wd_tcp_reader_receive(struct wd_tcp_reader* reader, int fd, uint64_t now_ns, uint64_t frame_timeout_ns,
                                                struct wd_tcp_message* out_message);
void wd_tcp_message_release(struct wd_tcp_message* message);

bool wd_send_all(int fd, const void* data, size_t size);
bool wd_recv_all(int fd, void* data, size_t size);

bool wd_send_tcp_message(int fd, uint16_t message_type, const void* payload, uint32_t payload_size);

/*
 * Allocates *out_payload with malloc() when payload_size > 0.
 * Caller owns *out_payload and must free() it.
 *
 * On failure:
 *   - returns false
 *   - *out_payload is NULL
 *   - *out_payload_size is 0
 */
bool wd_recv_tcp_message_limited(int fd, uint32_t max_payload_size, uint16_t* out_message_type, uint8_t** out_payload,
                                 uint32_t* out_payload_size);
bool wd_recv_tcp_message(int fd, uint16_t* out_message_type, uint8_t** out_payload, uint32_t* out_payload_size);


#ifdef __cplusplus
}
#endif
