#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_bandwidth_mode {
    WD_BANDWIDTH_MODE_TILES = 0,
    WD_BANDWIDTH_MODE_VIDEO = 1,
};

struct wd_bandwidth_plan {
    uint64_t link_bytes_per_second;
    uint64_t overhead_bytes_per_second;
    uint64_t control_bytes_per_second;
    uint64_t audio_cap_bytes_per_second;
    uint64_t audio_reserved_bytes_per_second;
    uint64_t fresh_tile_bytes_per_second;
    uint64_t repair_bytes_per_second;
    uint64_t video_bytes_per_second;
};

struct wd_bandwidth_plan wd_bandwidth_plan_build(uint64_t link_bytes_per_second, enum wd_bandwidth_mode mode,
                                                  bool audio_enabled, uint32_t audio_bitrate_bits_per_second);
uint64_t wd_bandwidth_plan_media_bytes(const struct wd_bandwidth_plan* plan, enum wd_bandwidth_mode mode);
bool     wd_bandwidth_plan_is_valid(const struct wd_bandwidth_plan* plan, enum wd_bandwidth_mode mode);

struct wd_bandwidth_bucket {
    double   tokens;
    uint64_t last_refill_ns;
};

void     wd_bandwidth_bucket_reset(struct wd_bandwidth_bucket* bucket);
uint64_t wd_bandwidth_bucket_available(struct wd_bandwidth_bucket* bucket, uint64_t rate_bytes_per_second,
                                       uint64_t burst_bytes, uint64_t now_ns);
uint64_t wd_bandwidth_bucket_consume(struct wd_bandwidth_bucket* bucket, uint64_t bytes);
void     wd_bandwidth_bucket_refund(struct wd_bandwidth_bucket* bucket, uint64_t bytes, uint64_t burst_bytes);

#ifdef __cplusplus
}
#endif
