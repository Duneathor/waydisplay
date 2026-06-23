#include "wd_connection_identity.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/random.h>

#define WD_CONNECTION_RANDOM_ATTEMPTS 8u

uint8_t wd_connection_next_session_id(uint8_t current_session_id) {
    if (current_session_id == 0 || current_session_id == UINT8_MAX)
    {
        return 1;
    }

    return (uint8_t)(current_session_id + 1u);
}

static bool wd_connection_random_read_exact(wd_connection_random_read_fn read_random, void* read_random_data, uint8_t* buffer,
                                            size_t size) {
    size_t offset = 0;
    while (offset < size)
    {
        const ssize_t read_size = read_random(buffer + offset, size - offset, read_random_data);
        if (read_size < 0 && errno == EINTR)
        {
            continue;
        }
        if (read_size <= 0 || (size_t)read_size > size - offset)
        {
            return false;
        }
        offset += (size_t)read_size;
    }
    return true;
}

static bool wd_connection_random_nonzero_u64(wd_connection_random_read_fn read_random, void* read_random_data, uint64_t* value) {
    for (unsigned int attempt = 0; attempt < WD_CONNECTION_RANDOM_ATTEMPTS; ++attempt)
    {
        uint64_t candidate = 0;
        if (!wd_connection_random_read_exact(read_random, read_random_data, (uint8_t*)&candidate, sizeof(candidate)))
        {
            return false;
        }
        if (candidate != 0)
        {
            *value = candidate;
            return true;
        }
    }
    return false;
}

bool wd_connection_identity_generate_with(wd_connection_random_read_fn read_random, void* read_random_data, uint64_t* connection_token,
                                          uint64_t* media_clock_id) {
    if (!read_random || !connection_token || !media_clock_id)
    {
        return false;
    }

    uint64_t token = 0;
    if (!wd_connection_random_nonzero_u64(read_random, read_random_data, &token))
    {
        return false;
    }

    for (unsigned int attempt = 0; attempt < WD_CONNECTION_RANDOM_ATTEMPTS; ++attempt)
    {
        uint64_t clock_id = 0;
        if (!wd_connection_random_nonzero_u64(read_random, read_random_data, &clock_id))
        {
            return false;
        }
        if (clock_id != token)
        {
            *connection_token = token;
            *media_clock_id   = clock_id;
            return true;
        }
    }

    return false;
}

static ssize_t wd_connection_getrandom(void* buffer, size_t size, void* user_data) {
    (void)user_data;
    return getrandom(buffer, size, 0);
}

bool wd_connection_identity_generate(uint64_t* connection_token, uint64_t* media_clock_id) {
    return wd_connection_identity_generate_with(wd_connection_getrandom, NULL, connection_token, media_clock_id);
}
