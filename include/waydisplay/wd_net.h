#pragma once

#include "waydisplay/wd_protocol_codec.h"
#include "waydisplay/wd_buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_TCP_PAYLOAD_PADDING_BYTES 64u

enum wd_tcp_reader_status {
    WD_TCP_READER_NEED_MORE = 0,
    WD_TCP_READER_MESSAGE,
    WD_TCP_READER_PEER_CLOSED,
    WD_TCP_READER_INVALID_FRAME,
    WD_TCP_READER_IO_ERROR,
    WD_TCP_READER_TIMED_OUT,
    WD_TCP_READER_CANCELLED,
};

typedef bool (*wd_tcp_wait_continue_fn)(void* user_data);

struct wd_tcp_message {
    uint16_t          message_type;
    struct wd_buffer* buffer;
    /* Borrowed view into buffer; valid only while this message owns buffer. */
    uint8_t*          payload;
    uint32_t          payload_size;
};

struct wd_tcp_reader {
    uint8_t  header_bytes[WD_TCP_HEADER_WIRE_SIZE];
    size_t   header_size;
    struct wd_buffer* payload_buffer;
    uint8_t*          payload;
    uint32_t          payload_size;
    uint32_t payload_received;
    uint32_t max_payload_size;
    uint16_t message_type;
    uint64_t idle_deadline_ns;
    uint64_t frame_deadline_ns;
    bool     header_decoded;
};

/* The reader is a single-owner state machine. NEED_MORE may be resumed
 * directly. After any terminal non-message result (peer closed, invalid frame,
 * I/O error, timeout, or cancellation), call reset() before attempting to
 * reuse the reader for another frame. */
void wd_tcp_reader_init(struct wd_tcp_reader* reader, uint32_t max_payload_size);
void wd_tcp_reader_reset(struct wd_tcp_reader* reader);
void wd_tcp_reader_destroy(struct wd_tcp_reader* reader);
bool wd_tcp_reader_has_partial_frame(const struct wd_tcp_reader* reader);
uint64_t wd_tcp_reader_deadline_ns(const struct wd_tcp_reader* reader);
enum wd_tcp_reader_status wd_tcp_reader_receive(struct wd_tcp_reader* reader, int fd, uint64_t now_ns, uint64_t idle_timeout_ns,
                                                uint64_t max_frame_lifetime_ns, struct wd_tcp_message* out_message);
enum wd_tcp_reader_status wd_tcp_reader_wait_for_message(struct wd_tcp_reader* reader, int fd, uint64_t idle_timeout_ns,
                                                         uint64_t max_frame_lifetime_ns, uint64_t absolute_deadline_ns,
                                                         uint32_t poll_slice_ms, wd_tcp_wait_continue_fn keep_waiting,
                                                         void* keep_waiting_data, struct wd_tcp_message* out_message);
/* Releases message->buffer and clears the message. Reader-produced messages
 * always own their payload through buffer; payload is never independently
 * malloc-owned. */
void              wd_tcp_message_release(struct wd_tcp_message* message);
/* Transfer the message payload owner to the caller. The returned buffer owns
 * message->payload bytes; the message is left with no payload. */
struct wd_buffer* wd_tcp_message_take_buffer(struct wd_tcp_message* message);

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
