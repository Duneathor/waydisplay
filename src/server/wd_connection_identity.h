#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Session IDs identify one control/UDP transport lifetime. They remain stable
 * across in-session configuration changes, but must advance whenever a new
 * control connection is accepted so packets from the previous client cannot
 * be mistaken for current traffic.
 */
uint8_t wd_connection_next_session_id(uint8_t current_session_id);

typedef ssize_t (*wd_connection_random_read_fn)(void* buffer, size_t size, void* user_data);

/* Generate independent, non-zero transport and media-clock identities. No
 * output is changed unless both values are obtained from secure randomness. */
bool wd_connection_identity_generate(uint64_t* connection_token, uint64_t* media_clock_id);
bool wd_connection_identity_generate_with(wd_connection_random_read_fn read_random, void* read_random_data, uint64_t* connection_token,
                                          uint64_t* media_clock_id);

#ifdef __cplusplus
}
#endif
