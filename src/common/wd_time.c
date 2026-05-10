#define _POSIX_C_SOURCE 200809L

#include "waydisplay/wd_time.h"

#include <time.h>

uint64_t wd_now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint32_t wd_now_ms32(void) {
    return (uint32_t)(wd_now_ns() / 1000000ull);
}
