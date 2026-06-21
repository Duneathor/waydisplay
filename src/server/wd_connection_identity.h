#pragma once

#include <stdint.h>

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

#ifdef __cplusplus
}
#endif
