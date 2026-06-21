#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline uint64_t wd_audio_reserved_bytes_per_second(uint32_t codec_bitrate) {
    if (codec_bitrate == 0) return 0;
    const uint64_t payload_bytes = ((uint64_t)codec_bitrate + 7u) / 8u;
    return payload_bytes + payload_bytes / 4u;
}
static inline uint64_t wd_audio_reserve_from_tile_budget(uint64_t measured_bytes_per_second,
                                                         uint32_t codec_bitrate) {
    const uint64_t reserve = wd_audio_reserved_bytes_per_second(codec_bitrate);
    if (reserve == 0 || measured_bytes_per_second == 0) return measured_bytes_per_second;
    /* Never turn a measured non-zero budget into the stream-policy sentinel zero. */
    if (measured_bytes_per_second <= reserve + 65536u)
        return measured_bytes_per_second / 2u ? measured_bytes_per_second / 2u : 1u;
    return measured_bytes_per_second - reserve;
}
#ifdef __cplusplus
}
#endif
