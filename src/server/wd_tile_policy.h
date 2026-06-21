#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t wd_tile_normalize_udp_payload_target(uint16_t udp_payload_target, uint16_t default_target,
                                              uint16_t maximum_target);
uint16_t wd_tile_packet_count_for_payload(uint32_t payload_size, uint16_t udp_payload_target);
uint16_t wd_cap_periodic_capture_fps(uint16_t capture_fps, uint16_t output_refresh_hz);
uint32_t wd_tile_wire_bytes_for_payload(uint32_t payload_size, uint16_t udp_payload_target,
                                        uint16_t packet_header_size, uint16_t first_packet_header_size);

struct wd_video_auto_entry_metrics {
    uint64_t frame_samples;
    uint64_t changed_frame_samples;
    uint64_t dirty_coverage_per_mille_sum;
    uint64_t dirty_coverage_per_mille_peak;
    uint64_t tile_wire_bytes;
    uint64_t tile_budget_bytes_per_second;
    uint64_t send_pressure_events;
    uint16_t requested_capture_fps;
    uint16_t adaptive_capture_fps;
    uint8_t minimum_dirty_percent;
};

struct wd_video_auto_entry_result {
    bool candidate;
    uint16_t changed_frame_percent;
    uint16_t changed_dirty_percent;
    uint16_t tile_budget_percent;
};

struct wd_video_auto_entry_result wd_video_auto_entry_evaluate(
    const struct wd_video_auto_entry_metrics* metrics);

bool wd_tile_compression_is_worthwhile(uint32_t compressed_size, uint32_t uncompressed_size,
                                       uint16_t udp_payload_target, uint16_t packet_header_size,
                                       uint16_t first_packet_header_size, uint32_t minimum_savings_bytes,
                                       uint8_t minimum_savings_percent);
bool wd_tile_xrgb_payload_may_compress(const uint8_t* payload, uint32_t payload_size);

enum wd_tile_compression_benchmark_mode {
    WD_TILE_COMPRESSION_BENCH_AUTO = 0,
    WD_TILE_COMPRESSION_BENCH_OFF = 1,
    WD_TILE_COMPRESSION_BENCH_ATTEMPT = 2,
    WD_TILE_COMPRESSION_BENCH_FORCE = 3,
};

bool wd_tile_compression_benchmark_mode_parse(const char* value, uint8_t* out_mode);
const char* wd_tile_compression_benchmark_mode_name(uint8_t mode);
bool wd_tile_compression_benchmark_should_attempt(uint8_t mode, bool entropy_ok, bool advisor_ok);
bool wd_tile_compression_benchmark_choose_compressed(uint8_t mode, bool compression_ok, bool worthwhile);

struct wd_tile_compression_advisor {
    uint16_t poor_streak;
    uint16_t bypass_remaining;
};

bool wd_tile_compression_advisor_should_attempt(struct wd_tile_compression_advisor* advisor);
void wd_tile_compression_advisor_record(struct wd_tile_compression_advisor* advisor, bool worthwhile);

struct wd_tile_delivery_status {
    uint32_t pending;
    bool sealed;
    bool failed;
};

void wd_tile_delivery_status_add(struct wd_tile_delivery_status* status);
bool wd_tile_delivery_status_complete(struct wd_tile_delivery_status* status, bool success, bool* out_failed);
bool wd_tile_delivery_status_seal(struct wd_tile_delivery_status* status, bool* out_failed);

size_t wd_tile_select_local_region_index(const uint16_t* region_ids, size_t region_count,
                                         uint16_t regions_x, uint16_t cursor_region_id,
                                         const uint64_t* region_enqueued_ns, size_t region_capacity,
                                         uint64_t now_ns, uint64_t starvation_ns);

#ifdef __cplusplus
}
#endif
