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
uint32_t wd_tile_wire_bytes_for_payload(uint32_t payload_size, uint16_t udp_payload_target,
                                        uint16_t packet_header_size, uint16_t first_packet_header_size);

bool wd_tile_compression_is_worthwhile(uint32_t compressed_size, uint32_t uncompressed_size,
                                       uint16_t udp_payload_target, uint16_t packet_header_size,
                                       uint16_t first_packet_header_size, uint32_t minimum_savings_bytes,
                                       uint8_t minimum_savings_percent);
bool wd_tile_xrgb_payload_may_compress(const uint8_t* payload, uint32_t payload_size);



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
