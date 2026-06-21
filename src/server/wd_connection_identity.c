#include "wd_connection_identity.h"

#include <stdint.h>

uint8_t wd_connection_next_session_id(uint8_t current_session_id) {
    if (current_session_id == 0 || current_session_id == UINT8_MAX)
    {
        return 1;
    }

    return (uint8_t)(current_session_id + 1u);
}
