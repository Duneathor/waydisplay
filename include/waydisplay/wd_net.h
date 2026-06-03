#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
bool wd_recv_tcp_message(int fd, uint16_t* out_message_type, uint8_t** out_payload, uint32_t* out_payload_size);

int wd_set_nonblocking(int fd);

#ifdef __cplusplus
}
#endif
