#define _POSIX_C_SOURCE 200809L

#include "waydisplay/wd_time.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

uint64_t wd_now_ns(void) {
    struct timespec ts = {0};

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        abort();
    }

    return (uint64_t)ts.tv_sec * WD_NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

void wd_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
    {
        // Retry with remaining time if interrupted by a signal.
    }
}
