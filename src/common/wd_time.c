#define _POSIX_C_SOURCE 200809L

#include "waydisplay/wd_time.h"

#include <stdint.h>
#include <time.h>
#include <errno.h>

uint64_t wd_now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint32_t wd_now_ms32(void) {
    return (uint32_t)(wd_now_ns() / 1000000ull);
}


void wd_sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        // Retry with remaining time if interrupted by a signal.
    }
}
