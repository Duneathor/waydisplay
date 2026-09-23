#pragma once

/* Reporting-interval rates, not configured rates or /min-normalized counts.
 * Use the monotonic interval between actual statistics log snapshots. */
#include <stdint.h>

static inline double wd_video_rate_per_sec(uint64_t count, uint64_t elapsed_ns) {
    return elapsed_ns ? (double)count * 1000000000.0 / (double)elapsed_ns : 0.0;
}

static inline double wd_video_payload_mbit_per_sec(uint64_t bytes, uint64_t elapsed_ns) {
    return elapsed_ns ? (double)bytes * 8.0 * 1000.0 / (double)elapsed_ns : 0.0;
}
