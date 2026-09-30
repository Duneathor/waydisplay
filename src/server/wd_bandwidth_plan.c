#include "wd_bandwidth_plan.h"

#include "waydisplay/wd_config.h"

#include <limits.h>
#include <string.h>

static uint64_t wd_percent_of_u64(uint64_t value, uint32_t percent) {
    const uint64_t whole = value / 100u;
    const uint64_t rem   = value % 100u;
    return whole * percent + (rem * percent) / 100u;
}

uint64_t wd_bandwidth_audio_wire_bytes_per_second(uint32_t bitrate_bits_per_second) {
    if (bitrate_bits_per_second == 0)
    {
        return 0;
    }

    const uint64_t payload = ((uint64_t)bitrate_bits_per_second + 7u) / 8u;
    return payload + wd_percent_of_u64(payload, WD_BANDWIDTH_AUDIO_TRANSPORT_ALLOWANCE_PERCENT);
}

uint32_t wd_bandwidth_audio_select_bitrate(uint64_t link_bytes_per_second, uint32_t preferred_bitrate_bits_per_second,
                                           uint32_t minimum_bitrate_bits_per_second) {
    if (link_bytes_per_second == 0 || preferred_bitrate_bits_per_second == 0 || minimum_bitrate_bits_per_second == 0 ||
        minimum_bitrate_bits_per_second > preferred_bitrate_bits_per_second)
    {
        return 0;
    }

    const uint64_t cap = wd_percent_of_u64(link_bytes_per_second, WD_BANDWIDTH_AUDIO_PERCENT);
    if (wd_bandwidth_audio_wire_bytes_per_second(minimum_bitrate_bits_per_second) > cap)
    {
        return 0;
    }
    if (wd_bandwidth_audio_wire_bytes_per_second(preferred_bitrate_bits_per_second) <= cap)
    {
        return preferred_bitrate_bits_per_second;
    }

    const uint64_t allowance = 100u + WD_BANDWIDTH_AUDIO_TRANSPORT_ALLOWANCE_PERCENT;
    const __uint128_t scaled_cap = (__uint128_t)cap * 100u;
    uint64_t payload_bytes_per_second = allowance != 0 ? (uint64_t)(scaled_cap / allowance) : 0;
    uint64_t selected = payload_bytes_per_second > UINT32_MAX / 8u ? UINT32_MAX : payload_bytes_per_second * 8u;
    if (selected > preferred_bitrate_bits_per_second)
    {
        selected = preferred_bitrate_bits_per_second;
    }
    selected -= selected % 1000u;
    while (selected >= minimum_bitrate_bits_per_second && wd_bandwidth_audio_wire_bytes_per_second((uint32_t)selected) > cap)
    {
        selected = selected >= 1000u ? selected - 1000u : 0;
    }
    return selected >= minimum_bitrate_bits_per_second ? (uint32_t)selected : 0;
}

struct wd_bandwidth_plan wd_bandwidth_plan_build(uint64_t link_bytes_per_second, enum wd_bandwidth_mode mode,
                                                  bool audio_enabled, uint32_t audio_bitrate_bits_per_second) {
    struct wd_bandwidth_plan plan;
    memset(&plan, 0, sizeof(plan));

    plan.link_bytes_per_second     = link_bytes_per_second;
    plan.overhead_bytes_per_second = wd_percent_of_u64(link_bytes_per_second, WD_BANDWIDTH_OVERHEAD_PERCENT);
    plan.control_bytes_per_second  = wd_percent_of_u64(link_bytes_per_second, WD_BANDWIDTH_CONTROL_PERCENT);
    plan.audio_cap_bytes_per_second = wd_percent_of_u64(link_bytes_per_second, WD_BANDWIDTH_AUDIO_PERCENT);

    if (audio_enabled)
    {
        const uint64_t required = wd_bandwidth_audio_wire_bytes_per_second(audio_bitrate_bits_per_second);
        plan.audio_required_bytes_per_second = required;
        plan.audio_reserved_bytes_per_second = required < plan.audio_cap_bytes_per_second ? required : plan.audio_cap_bytes_per_second;
    }

    if (mode == WD_BANDWIDTH_MODE_VIDEO)
    {
        plan.video_bytes_per_second = wd_percent_of_u64(link_bytes_per_second, WD_BANDWIDTH_VIDEO_PERCENT);
    }
    else
    {
        plan.fresh_tile_bytes_per_second = wd_percent_of_u64(link_bytes_per_second, WD_BANDWIDTH_TILE_FRESH_PERCENT);
        plan.repair_bytes_per_second     = wd_percent_of_u64(link_bytes_per_second, WD_BANDWIDTH_TILE_REPAIR_PERCENT);
    }

    return plan;
}

uint64_t wd_bandwidth_plan_media_bytes(const struct wd_bandwidth_plan* plan, enum wd_bandwidth_mode mode) {
    if (!plan)
    {
        return 0;
    }
    if (mode == WD_BANDWIDTH_MODE_VIDEO)
    {
        return plan->video_bytes_per_second;
    }
    if (mode == WD_BANDWIDTH_MODE_TILES)
    {
        return plan->fresh_tile_bytes_per_second + plan->repair_bytes_per_second;
    }
    return 0;
}

bool wd_bandwidth_plan_is_valid(const struct wd_bandwidth_plan* plan, enum wd_bandwidth_mode mode) {
    if (!plan || (mode != WD_BANDWIDTH_MODE_TILES && mode != WD_BANDWIDTH_MODE_VIDEO) ||
        plan->audio_reserved_bytes_per_second > plan->audio_cap_bytes_per_second ||
        plan->audio_reserved_bytes_per_second != plan->audio_required_bytes_per_second)
    {
        return false;
    }

    __uint128_t allocated = (__uint128_t)plan->overhead_bytes_per_second + plan->control_bytes_per_second + plan->audio_cap_bytes_per_second;
    if (mode == WD_BANDWIDTH_MODE_VIDEO)
    {
        allocated += plan->video_bytes_per_second;
    }
    else
    {
        allocated += (__uint128_t)plan->fresh_tile_bytes_per_second + plan->repair_bytes_per_second;
    }
    return allocated <= plan->link_bytes_per_second;
}

void wd_bandwidth_bucket_reset(struct wd_bandwidth_bucket* bucket) {
    if (!bucket)
    {
        return;
    }
    bucket->tokens = 0.0;
    bucket->last_refill_ns = 0;
}

uint64_t wd_bandwidth_bucket_available(struct wd_bandwidth_bucket* bucket, uint64_t rate_bytes_per_second,
                                       uint64_t burst_bytes, uint64_t now_ns) {
    if (!bucket)
    {
        return UINT64_MAX;
    }
    if (rate_bytes_per_second == 0 || burst_bytes == 0)
    {
        return 0;
    }
    if (bucket->last_refill_ns == 0 || now_ns < bucket->last_refill_ns)
    {
        bucket->last_refill_ns = now_ns;
        bucket->tokens = 0.0;
        return 0;
    }

    const uint64_t elapsed_ns = now_ns - bucket->last_refill_ns;
    bucket->last_refill_ns = now_ns;
    bucket->tokens += ((double)elapsed_ns / 1000000000.0) * (double)rate_bytes_per_second;
    if (bucket->tokens > (double)burst_bytes)
    {
        bucket->tokens = (double)burst_bytes;
    }
    return bucket->tokens >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)bucket->tokens;
}

uint64_t wd_bandwidth_bucket_consume(struct wd_bandwidth_bucket* bucket, uint64_t bytes) {
    if (!bucket || bytes == 0)
    {
        return 0;
    }
    const uint64_t available = bucket->tokens <= 0.0 ? 0 :
                               (bucket->tokens >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)bucket->tokens);
    const uint64_t consumed = bytes < available ? bytes : available;
    bucket->tokens -= (double)consumed;
    if (bucket->tokens < 0.0)
    {
        bucket->tokens = 0.0;
    }
    return consumed;
}

void wd_bandwidth_bucket_refund(struct wd_bandwidth_bucket* bucket, uint64_t bytes, uint64_t burst_bytes) {
    if (!bucket || bytes == 0 || burst_bytes == 0)
    {
        return;
    }
    bucket->tokens += (double)bytes;
    if (bucket->tokens > (double)burst_bytes)
    {
        bucket->tokens = (double)burst_bytes;
    }
}
