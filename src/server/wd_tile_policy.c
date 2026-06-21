#include "wd_tile_policy.h"

#include <limits.h>
#include <string.h>

uint16_t wd_tile_normalize_udp_payload_target(uint16_t udp_payload_target, uint16_t default_target,
                                              uint16_t maximum_target) {
    if (maximum_target == 0)
    {
        return 0;
    }
    if (default_target == 0 || default_target > maximum_target)
    {
        default_target = maximum_target;
    }
    if (udp_payload_target < 512)
    {
        return default_target;
    }
    return udp_payload_target > maximum_target ? maximum_target : udp_payload_target;
}

uint16_t wd_cap_periodic_capture_fps(uint16_t capture_fps, uint16_t output_refresh_hz) {
    if (capture_fps == 0)
    {
        capture_fps = 1;
    }
    if (output_refresh_hz != 0 && capture_fps > output_refresh_hz)
    {
        capture_fps = output_refresh_hz;
    }
    return capture_fps;
}

uint16_t wd_tile_packet_count_for_payload(uint32_t payload_size, uint16_t udp_payload_target) {
    if (payload_size == 0 || udp_payload_target == 0)
    {
        return 0;
    }
    const uint32_t count = (payload_size + (uint32_t)udp_payload_target - 1u) / (uint32_t)udp_payload_target;
    return count > UINT16_MAX ? 0 : (uint16_t)count;
}

uint32_t wd_tile_wire_bytes_for_payload(uint32_t payload_size, uint16_t udp_payload_target,
                                        uint16_t packet_header_size, uint16_t first_packet_header_size) {
    const uint16_t packet_count = wd_tile_packet_count_for_payload(payload_size, udp_payload_target);
    if (packet_count == 0 || packet_header_size == 0 || first_packet_header_size < packet_header_size)
    {
        return 0;
    }

    const uint64_t wire_bytes = (uint64_t)payload_size + first_packet_header_size +
                                (uint64_t)(packet_count - 1u) * packet_header_size;
    return wire_bytes > UINT32_MAX ? 0 : (uint32_t)wire_bytes;
}

static uint16_t wd_percent_clamped(uint64_t numerator, uint64_t denominator) {
    if (denominator == 0)
    {
        return 0;
    }
    const uint64_t percent = numerator > UINT64_MAX / 100u
                                 ? UINT64_MAX
                                 : (numerator * 100u) / denominator;
    return percent > UINT16_MAX ? UINT16_MAX : (uint16_t)percent;
}

struct wd_video_auto_entry_result wd_video_auto_entry_evaluate(
    const struct wd_video_auto_entry_metrics* metrics) {
    struct wd_video_auto_entry_result result;
    memset(&result, 0, sizeof(result));
    if (!metrics || metrics->frame_samples == 0 || metrics->changed_frame_samples == 0)
    {
        return result;
    }

    result.changed_frame_percent = wd_percent_clamped(metrics->changed_frame_samples, metrics->frame_samples);
    result.changed_dirty_percent = wd_percent_clamped(
        metrics->dirty_coverage_per_mille_sum, metrics->changed_frame_samples * 10u);
    result.tile_budget_percent = wd_percent_clamped(metrics->tile_wire_bytes,
                                                     metrics->tile_budget_bytes_per_second);

    const uint16_t min_dirty = metrics->minimum_dirty_percent != 0
                                   ? metrics->minimum_dirty_percent
                                   : 60u;
    uint16_t motion_dirty_floor = min_dirty / 3u;
    if (motion_dirty_floor < 15u)
    {
        motion_dirty_floor = 15u;
    }
    const uint16_t peak_dirty_percent = (uint16_t)(metrics->dirty_coverage_per_mille_peak / 10u);
    uint16_t peak_floor = min_dirty;
    if (peak_floor < 50u)
    {
        peak_floor = 50u;
    }

    const bool sustained_motion = result.changed_frame_percent >= 25u;
    const bool broad_motion = result.changed_dirty_percent >= min_dirty;
    const bool concentrated_motion = result.changed_dirty_percent >= motion_dirty_floor &&
                                     peak_dirty_percent >= peak_floor;
    const bool wire_pressure = metrics->tile_budget_bytes_per_second != 0 &&
                               result.tile_budget_percent >= 65u;
    const bool fps_suppressed = metrics->requested_capture_fps != 0 &&
                                (uint32_t)metrics->adaptive_capture_fps * 100u <
                                    (uint32_t)metrics->requested_capture_fps * 85u;
    const bool queue_pressure = metrics->send_pressure_events != 0;

    result.candidate = sustained_motion && (broad_motion || concentrated_motion) &&
                       (wire_pressure || fps_suppressed || queue_pressure);
    return result;
}

bool wd_tile_compression_is_worthwhile(uint32_t compressed_size, uint32_t uncompressed_size,
                                       uint16_t udp_payload_target, uint16_t packet_header_size,
                                       uint16_t first_packet_header_size, uint32_t minimum_savings_bytes,
                                       uint8_t minimum_savings_percent) {
    if (compressed_size == 0 || uncompressed_size == 0 || compressed_size >= uncompressed_size)
    {
        return false;
    }

    const uint32_t compressed_wire = wd_tile_wire_bytes_for_payload(
        compressed_size, udp_payload_target, packet_header_size, first_packet_header_size);
    const uint32_t uncompressed_wire = wd_tile_wire_bytes_for_payload(
        uncompressed_size, udp_payload_target, packet_header_size, first_packet_header_size);
    if (compressed_wire == 0 || uncompressed_wire == 0 || compressed_wire >= uncompressed_wire)
    {
        return false;
    }

    const uint32_t saved = uncompressed_wire - compressed_wire;
    if (saved < minimum_savings_bytes)
    {
        return false;
    }
    if (minimum_savings_percent != 0 &&
        (uint64_t)saved * 100u < (uint64_t)uncompressed_wire * minimum_savings_percent)
    {
        return false;
    }
    return true;
}

bool wd_tile_xrgb_payload_may_compress(const uint8_t* payload, uint32_t payload_size) {
    const uint32_t pixel_count = payload_size / 4u;
    if (!payload || pixel_count == 0)
    {
        return false;
    }
    if (pixel_count <= 16u)
    {
        return true;
    }

    enum { SAMPLE_COUNT = 64 };
    uint32_t colors[SAMPLE_COUNT];
    uint32_t unique_count = 0;
    uint32_t adjacent_repeats = 0;
    uint32_t repeated_deltas = 0;
    uint32_t previous_color = 0;
    uint32_t previous_delta = 0;
    bool have_previous = false;
    bool have_delta = false;

    const uint32_t samples = pixel_count < SAMPLE_COUNT ? pixel_count : SAMPLE_COUNT;
    for (uint32_t i = 0; i < samples; ++i)
    {
        const uint32_t pixel_index = samples == 1 ? 0 :
            (uint32_t)(((uint64_t)i * (pixel_count - 1u)) / (samples - 1u));
        uint32_t color = 0;
        memcpy(&color, payload + (size_t)pixel_index * 4u, sizeof(color));
        color &= 0x00ffffffu;

        bool seen = false;
        for (uint32_t j = 0; j < unique_count; ++j)
        {
            if (colors[j] == color)
            {
                seen = true;
                break;
            }
        }
        if (!seen)
        {
            colors[unique_count++] = color;
        }

        if (have_previous)
        {
            if (color == previous_color)
            {
                adjacent_repeats++;
            }
            const uint32_t delta = color - previous_color;
            if (have_delta && delta == previous_delta)
            {
                repeated_deltas++;
            }
            previous_delta = delta;
            have_delta = true;
        }
        previous_color = color;
        have_previous = true;
    }

    return unique_count <= samples * 7u / 8u || adjacent_repeats >= 2u || repeated_deltas >= samples / 8u;
}


bool wd_tile_compression_benchmark_mode_parse(const char* value, uint8_t* out_mode) {
    if (!value || !out_mode)
    {
        return false;
    }

    uint8_t mode = WD_TILE_COMPRESSION_BENCH_AUTO;
    if (strcmp(value, "auto") == 0)
    {
        mode = WD_TILE_COMPRESSION_BENCH_AUTO;
    }
    else if (strcmp(value, "off") == 0)
    {
        mode = WD_TILE_COMPRESSION_BENCH_OFF;
    }
    else if (strcmp(value, "attempt") == 0)
    {
        mode = WD_TILE_COMPRESSION_BENCH_ATTEMPT;
    }
    else if (strcmp(value, "force") == 0)
    {
        mode = WD_TILE_COMPRESSION_BENCH_FORCE;
    }
    else
    {
        return false;
    }
    *out_mode = mode;
    return true;
}

const char* wd_tile_compression_benchmark_mode_name(uint8_t mode) {
    switch (mode)
    {
        case WD_TILE_COMPRESSION_BENCH_OFF:
            return "off";
        case WD_TILE_COMPRESSION_BENCH_ATTEMPT:
            return "attempt";
        case WD_TILE_COMPRESSION_BENCH_FORCE:
            return "force";
        case WD_TILE_COMPRESSION_BENCH_AUTO:
        default:
            return "auto";
    }
}

bool wd_tile_compression_benchmark_should_attempt(uint8_t mode, bool entropy_ok, bool advisor_ok) {
    switch (mode)
    {
        case WD_TILE_COMPRESSION_BENCH_OFF:
            return false;
        case WD_TILE_COMPRESSION_BENCH_ATTEMPT:
        case WD_TILE_COMPRESSION_BENCH_FORCE:
            return true;
        case WD_TILE_COMPRESSION_BENCH_AUTO:
        default:
            return entropy_ok && advisor_ok;
    }
}

bool wd_tile_compression_benchmark_choose_compressed(uint8_t mode, bool compression_ok, bool worthwhile) {
    if (!compression_ok)
    {
        return false;
    }
    return mode == WD_TILE_COMPRESSION_BENCH_FORCE ? true : worthwhile;
}

bool wd_tile_compression_advisor_should_attempt(struct wd_tile_compression_advisor* advisor) {
    if (!advisor || advisor->bypass_remaining == 0)
    {
        return true;
    }
    advisor->bypass_remaining--;
    return (advisor->bypass_remaining % 16u) == 0;
}

void wd_tile_compression_advisor_record(struct wd_tile_compression_advisor* advisor, bool worthwhile) {
    if (!advisor)
    {
        return;
    }
    if (worthwhile)
    {
        advisor->poor_streak = 0;
        advisor->bypass_remaining = 0;
        return;
    }
    advisor->poor_streak++;
    if (advisor->poor_streak >= 8u)
    {
        advisor->poor_streak = 0;
        advisor->bypass_remaining = 64u;
    }
}

void wd_tile_delivery_status_add(struct wd_tile_delivery_status* status) {
    if (status)
    {
        status->pending++;
    }
}

static bool wd_tile_delivery_status_ready(struct wd_tile_delivery_status* status, bool* out_failed) {
    if (!status || !status->sealed || status->pending != 0)
    {
        return false;
    }
    if (out_failed)
    {
        *out_failed = status->failed;
    }
    return true;
}

bool wd_tile_delivery_status_complete(struct wd_tile_delivery_status* status, bool success, bool* out_failed) {
    if (!status || status->pending == 0)
    {
        return false;
    }
    status->pending--;
    status->failed = status->failed || !success;
    return wd_tile_delivery_status_ready(status, out_failed);
}

bool wd_tile_delivery_status_seal(struct wd_tile_delivery_status* status, bool* out_failed) {
    if (!status)
    {
        return false;
    }
    status->sealed = true;
    return wd_tile_delivery_status_ready(status, out_failed);
}

static uint32_t wd_tile_region_distance(uint16_t lhs, uint16_t rhs, uint16_t regions_x) {
    if (regions_x == 0)
    {
        return UINT32_MAX;
    }
    const uint32_t lhs_x = lhs % regions_x;
    const uint32_t lhs_y = lhs / regions_x;
    const uint32_t rhs_x = rhs % regions_x;
    const uint32_t rhs_y = rhs / regions_x;
    const uint32_t dx = lhs_x > rhs_x ? lhs_x - rhs_x : rhs_x - lhs_x;
    const uint32_t dy = lhs_y > rhs_y ? lhs_y - rhs_y : rhs_y - lhs_y;
    return dx + dy;
}

size_t wd_tile_select_local_region_index(const uint16_t* region_ids, size_t region_count,
                                         uint16_t regions_x, uint16_t cursor_region_id,
                                         const uint64_t* region_enqueued_ns, size_t region_capacity,
                                         uint64_t now_ns, uint64_t starvation_ns) {
    if (!region_ids || region_count == 0 || regions_x == 0)
    {
        return SIZE_MAX;
    }

    size_t oldest_index = 0;
    const uint16_t first_id = region_ids[0];
    uint64_t oldest_ns = region_enqueued_ns && first_id < region_capacity ? region_enqueued_ns[first_id] : 0;
    for (size_t i = 1; i < region_count; ++i)
    {
        const uint16_t region_id = region_ids[i];
        const uint64_t queued = region_enqueued_ns && region_id < region_capacity ? region_enqueued_ns[region_id] : 0;
        if (queued != 0 && (oldest_ns == 0 || queued < oldest_ns))
        {
            oldest_index = i;
            oldest_ns = queued;
        }
    }
    if (starvation_ns != 0 && oldest_ns != 0 && now_ns >= oldest_ns && now_ns - oldest_ns >= starvation_ns)
    {
        return oldest_index;
    }

    size_t best_index = 0;
    uint32_t best_distance = wd_tile_region_distance(region_ids[0], cursor_region_id, regions_x);
    for (size_t i = 1; i < region_count; ++i)
    {
        const uint32_t distance = wd_tile_region_distance(region_ids[i], cursor_region_id, regions_x);
        if (distance < best_distance)
        {
            best_index = i;
            best_distance = distance;
            continue;
        }
        if (distance == best_distance)
        {
            const uint16_t best_id = region_ids[best_index];
            const uint16_t region_id = region_ids[i];
            const uint64_t best_queued = region_enqueued_ns && best_id < region_capacity ? region_enqueued_ns[best_id] : 0;
            const uint64_t queued = region_enqueued_ns && region_id < region_capacity ? region_enqueued_ns[region_id] : 0;
            if ((queued != 0 && (best_queued == 0 || queued < best_queued)) ||
                (queued == best_queued && region_id < best_id))
            {
                best_index = i;
            }
        }
    }
    return best_index;
}
