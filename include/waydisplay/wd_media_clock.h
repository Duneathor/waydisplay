#pragma once

#include <stdint.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint64_t wd_media_delta_ns(uint64_t now_ns, uint64_t origin_ns) {
    return now_ns > origin_ns ? now_ns - origin_ns : 0;
}

static inline uint64_t wd_media_ns_to_usec(uint64_t now_ns, uint64_t origin_ns) {
    return wd_media_delta_ns(now_ns, origin_ns) / 1000ull;
}

static inline uint64_t wd_media_ns_to_samples(uint64_t now_ns, uint64_t origin_ns,
                                              uint32_t sample_rate) {
    if (sample_rate == 0)
    {
        return 0;
    }
    const uint64_t delta = wd_media_delta_ns(now_ns, origin_ns);
    const uint64_t seconds = delta / 1000000000ull;
    const uint64_t remainder = delta % 1000000000ull;
    if (seconds > UINT64_MAX / sample_rate)
    {
        return UINT64_MAX;
    }
    const uint64_t whole = seconds * sample_rate;
    const uint64_t fraction = (remainder * sample_rate) / 1000000000ull;
    return UINT64_MAX - whole < fraction ? UINT64_MAX : whole + fraction;
}

static inline uint64_t wd_media_usec_to_samples(uint64_t pts_usec, uint32_t sample_rate) {
    if (sample_rate == 0)
    {
        return 0;
    }
    const uint64_t seconds = pts_usec / 1000000ull;
    const uint64_t remainder = pts_usec % 1000000ull;
    if (seconds > UINT64_MAX / sample_rate)
    {
        return UINT64_MAX;
    }
    const uint64_t whole = seconds * sample_rate;
    const uint64_t fraction = (remainder * sample_rate) / 1000000ull;
    return UINT64_MAX - whole < fraction ? UINT64_MAX : whole + fraction;
}

static inline uint64_t wd_media_local_deadline_ns(uint64_t local_origin_ns, uint64_t pts_usec) {
    if (pts_usec > (UINT64_MAX - local_origin_ns) / 1000ull)
    {
        return UINT64_MAX;
    }
    return local_origin_ns + pts_usec * 1000ull;
}

#ifdef __cplusplus
}
#endif
