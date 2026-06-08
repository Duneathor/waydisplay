#include "waydisplay/wd_net.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "waydisplay/wd_zstd.h"
#include "wd_server.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#define WD_NSEC_PER_SEC 1000000000ull

#define WD_UDP_SEND_PRESSURE_LOG_INTERVAL_NS 1000000000ull

static void wd_stream_note_input_to_first_fresh_tile_locked(struct wd_net_state* net, uint64_t tile_send_ns) {
    if (!net || !net->input_since_last_fresh_tile || net->last_input_inject_ns == 0 || tile_send_ns < net->last_input_inject_ns)
    {
        return;
    }

    net->stats.input_to_first_fresh_tile_samples++;
    net->stats.input_to_first_fresh_tile_sum_ns += tile_send_ns - net->last_input_inject_ns;
    net->input_since_last_fresh_tile = false;
}


static void wd_stream_note_full_frame_start_locked(struct wd_net_state* net, uint64_t now_ns) {
    if (!net || net->full_frame_start_ns != 0)
    {
        return;
    }

    net->full_frame_start_ns = now_ns;
    net->full_frame_tiles_sent = 0;
    net->stats.full_frame_catchup_started++;
}

static void wd_stream_note_full_frame_tile_sent_locked(struct wd_net_state* net) {
    if (!net || net->full_frame_start_ns == 0)
    {
        return;
    }

    net->full_frame_tiles_sent++;
    net->stats.full_frame_catchup_tiles_sent++;
}

static void wd_stream_note_full_frame_complete_locked(struct wd_net_state* net, uint64_t now_ns) {
    if (!net || net->full_frame_start_ns == 0)
    {
        return;
    }

    net->stats.full_frame_catchup_completed++;
    if (now_ns >= net->full_frame_start_ns)
    {
        net->stats.full_frame_catchup_duration_sum_ns += now_ns - net->full_frame_start_ns;
    }

    net->full_frame_start_ns = 0;
    net->full_frame_tiles_sent = 0;
}

static uint64_t wd_stream_byte_burst_cap_for_rate(uint64_t bytes_per_second) {
    if (bytes_per_second == 0)
    {
        return 0;
    }

    uint64_t cap = bytes_per_second / WD_STREAM_TOKEN_BURST_DIVISOR;
    if (cap < (uint64_t)WD_UNCOMPRESSED_TILE_BYTES * 2ull)
    {
        cap = (uint64_t)WD_UNCOMPRESSED_TILE_BYTES * 2ull;
    }
    return cap ? cap : bytes_per_second;
}

static uint64_t wd_stream_clamp_limited_udp_byte_rate(uint64_t bytes_per_second) {
    if (bytes_per_second == 0)
    {
        bytes_per_second = WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    }

    if (bytes_per_second < WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND)
    {
        bytes_per_second = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
    }

    if (bytes_per_second > WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND)
    {
        bytes_per_second = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
    }

    return bytes_per_second;
}

static uint64_t wd_stream_limited_rate_from_kib(uint32_t kib_per_second) {
    if (kib_per_second == 0)
    {
        return 0;
    }

    uint64_t bytes_per_second = (uint64_t)kib_per_second * 1024ull;
    if (bytes_per_second / 1024ull != (uint64_t)kib_per_second)
    {
        bytes_per_second = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
    }

    return wd_stream_clamp_limited_udp_byte_rate(bytes_per_second);
}

static void wd_stream_policy_reset_tokens(struct wd_stream_policy* policy) {
    if (!policy)
    {
        return;
    }

    policy->last_frame_send_ns              = 0;
    policy->limited_udp_byte_tokens         = 0.0;
    policy->last_limited_udp_byte_refill_ns = 0;
}

void wd_stream_policy_set_defaults(struct wd_stream_policy* policy) {
    if (!policy)
    {
        return;
    }

    memset(policy, 0, sizeof(*policy));

    policy->target_fps                   = WD_DEFAULT_PARTIAL_FPS;
    policy->effective_target_fps         = WD_DEFAULT_PARTIAL_FPS;
    policy->throttle_bad_windows         = 0;
    policy->frame_rate_good_windows      = 0;
    policy->throttle_good_windows        = 0;
    policy->limited_udp_bytes_per_second = WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    policy->limited_udp_rate_floor        = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
    policy->limited_udp_rate_ceiling      = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
    policy->limited_rate_good_windows      = 0;
    policy->client_completion_low_windows  = 0;
    policy->tile_size_good_windows         = 0;
    policy->tile_size_bad_windows          = 0;
    policy->tile_size_change_cooldown_windows = 0;
    wd_stream_policy_reset_tokens(policy);
}

void wd_stream_policy_apply_client_hello(struct wd_stream_policy* policy, const struct wd_client_hello_payload* hello) {
    if (!policy || !hello)
    {
        return;
    }

    uint16_t fps = hello->target_fps;
    if (fps == 0)
    {
        fps = WD_DEFAULT_PARTIAL_FPS;
    }

    if (fps > WD_MAX_REASONABLE_FPS)
    {
        fps = WD_MAX_REASONABLE_FPS;
    }

    policy->target_fps           = fps;
    policy->effective_target_fps = fps;
    policy->throttle_bad_windows     = 0;
    policy->frame_rate_good_windows  = 0;
    policy->throttle_good_windows    = 0;
    policy->limited_rate_good_windows = 0;
    policy->client_completion_low_windows = 0;
    policy->tile_size_good_windows = 0;
    policy->tile_size_bad_windows = 0;
    policy->tile_size_change_cooldown_windows = 0;
    if (policy->limited_udp_bytes_per_second == 0)
    {
        policy->limited_udp_bytes_per_second = WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    }
    if (policy->limited_udp_rate_floor == 0)
    {
        policy->limited_udp_rate_floor = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
    }
    if (policy->limited_udp_rate_ceiling == 0)
    {
        policy->limited_udp_rate_ceiling = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
    }

    uint64_t requested_limited_rate = wd_stream_limited_rate_from_kib(hello->limited_udp_kib_per_second);
    if (requested_limited_rate != 0)
    {
        uint64_t ceiling = wd_stream_clamp_limited_udp_byte_rate(policy->limited_udp_rate_ceiling);
        if (requested_limited_rate > ceiling)
        {
            requested_limited_rate = ceiling;
        }

        policy->limited_udp_bytes_per_second = requested_limited_rate;
        policy->limited_udp_rate_ceiling     = requested_limited_rate;
        if (policy->limited_udp_rate_floor > requested_limited_rate)
        {
            policy->limited_udp_rate_floor = requested_limited_rate;
        }
        policy->limited_rate_good_windows = 0;
    }

    policy->tile_size_good_windows = 0;
    policy->tile_size_bad_windows = 0;
    policy->tile_size_change_cooldown_windows = 0;

    wd_stream_policy_reset_tokens(policy);
}


void wd_stream_policy_set_limited_udp_byte_rate(struct wd_stream_policy* policy, uint64_t bytes_per_second) {
    if (!policy)
    {
        return;
    }

    uint64_t rate = wd_stream_clamp_limited_udp_byte_rate(bytes_per_second);

    policy->limited_udp_bytes_per_second    = rate;
    policy->limited_udp_rate_floor          = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
    policy->limited_udp_rate_ceiling        = rate;
    policy->limited_rate_good_windows       = 0;
    policy->limited_udp_byte_tokens         = 0.0;
    policy->last_limited_udp_byte_refill_ns = 0;
}

static uint64_t wd_stream_policy_limited_floor(const struct wd_stream_policy* policy) {
    uint64_t floor = policy ? policy->limited_udp_rate_floor : 0;
    if (floor == 0)
    {
        floor = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
    }
    return wd_stream_clamp_limited_udp_byte_rate(floor);
}

static uint64_t wd_stream_policy_limited_ceiling(const struct wd_stream_policy* policy) {
    uint64_t ceiling = policy ? policy->limited_udp_rate_ceiling : 0;
    if (ceiling == 0)
    {
        ceiling = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
    }
    return wd_stream_clamp_limited_udp_byte_rate(ceiling);
}

static void wd_stream_policy_set_limited_rate_adaptive_locked(struct wd_stream_policy* policy, uint64_t rate) {
    if (!policy)
    {
        return;
    }

    uint64_t floor   = wd_stream_policy_limited_floor(policy);
    uint64_t ceiling = wd_stream_policy_limited_ceiling(policy);

    rate = wd_stream_clamp_limited_udp_byte_rate(rate);
    if (rate < floor)
    {
        rate = floor;
    }
    if (rate > ceiling)
    {
        rate = ceiling;
    }

    if (rate == policy->limited_udp_bytes_per_second)
    {
        return;
    }

    policy->limited_udp_bytes_per_second    = rate;
    policy->limited_udp_byte_tokens         = 0.0;
    policy->last_limited_udp_byte_refill_ns = 0;
}


static uint16_t wd_stream_policy_effective_fps_locked(const struct wd_stream_policy* policy) {
    uint16_t fps = policy ? policy->effective_target_fps : 0;
    if (fps == 0 && policy)
    {
        fps = policy->target_fps;
    }
    if (fps == 0)
    {
        fps = WD_DEFAULT_PARTIAL_FPS;
    }
    if (fps < WD_ADAPTIVE_FPS_MIN)
    {
        fps = WD_ADAPTIVE_FPS_MIN;
    }
    if (fps > WD_MAX_REASONABLE_FPS)
    {
        fps = WD_MAX_REASONABLE_FPS;
    }
    return fps;
}


static uint32_t wd_stream_tile_area(uint16_t width, uint16_t height) {
    return (uint32_t)width * (uint32_t)height;
}

static bool wd_stream_next_larger_tile_size(uint16_t width, uint16_t height, uint16_t* out_width, uint16_t* out_height) {
    if (!out_width || !out_height)
    {
        return false;
    }

    if (width <= 16 && height <= 16)
    {
        *out_width = 32;
        *out_height = 32;
        return true;
    }
    if (width <= 32 && height <= 32)
    {
        *out_width = 64;
        *out_height = 64;
        return true;
    }
    if (width <= 64 && height <= 64)
    {
        *out_width = 128;
        *out_height = 64;
        return true;
    }

    return false;
}

static bool wd_stream_next_smaller_tile_size(uint16_t width, uint16_t height, uint16_t* out_width, uint16_t* out_height) {
    if (!out_width || !out_height)
    {
        return false;
    }

    if (width >= 128 && height >= 64)
    {
        *out_width = 64;
        *out_height = 64;
        return true;
    }
    if (width >= 64 && height >= 64)
    {
        *out_width = 32;
        *out_height = 32;
        return true;
    }
    if (width >= 32 && height >= 32)
    {
        *out_width = 16;
        *out_height = 16;
        return true;
    }

    return false;
}

static bool wd_stream_tile_payloads_fit_one_packet(const struct wd_stats* stats, uint16_t udp_payload_target) {
    if (!stats || stats->udp_tiles_sent == 0 || udp_payload_target == 0)
    {
        return false;
    }

    if (stats->udp_packets_sent <= stats->udp_tiles_sent)
    {
        return true;
    }

    /* The choice stats use payload+header wire bytes.  A one-packet tile can
     * still exceed udp_payload_target slightly after accounting for the header,
     * so allow the largest current extensible header as slack. */
    const uint64_t choices = stats->tile_choice_compressed + stats->tile_choice_uncompressed;
    if (choices == 0)
    {
        return false;
    }

    const uint32_t avg_chosen_wire = (uint32_t)(stats->tile_choice_chosen_wire_sum / choices);
    return avg_chosen_wire <= (uint32_t)udp_payload_target + WD_UDP_TILE_HEADER_MAX_SIZE;
}

static bool wd_stream_observed_wire_bytes_per_pixel(const struct wd_stats* stats, uint16_t tile_width, uint16_t tile_height,
                                                       uint64_t* out_wire_bytes, uint64_t* out_pixels) {
    if (!stats || !out_wire_bytes || !out_pixels || tile_width == 0 || tile_height == 0)
    {
        return false;
    }

    const uint64_t choices = stats->tile_choice_compressed + stats->tile_choice_uncompressed;
    if (choices == 0 || stats->tile_choice_chosen_wire_sum == 0)
    {
        return false;
    }

    *out_wire_bytes = stats->tile_choice_chosen_wire_sum;
    *out_pixels = choices * (uint64_t)wd_stream_tile_area(tile_width, tile_height);
    return *out_pixels != 0;
}

static bool wd_stream_estimated_tile_wire_fits_packet(const struct wd_stats* stats, uint16_t current_width, uint16_t current_height,
                                                       uint16_t candidate_width, uint16_t candidate_height,
                                                       uint16_t udp_payload_target) {
    if (!stats || udp_payload_target == 0)
    {
        return false;
    }

    uint64_t observed_wire_bytes = 0;
    uint64_t observed_pixels = 0;
    if (!wd_stream_observed_wire_bytes_per_pixel(stats, current_width, current_height, &observed_wire_bytes, &observed_pixels))
    {
        return false;
    }

    const uint64_t candidate_pixels = (uint64_t)wd_stream_tile_area(candidate_width, candidate_height);
    if (candidate_pixels == 0)
    {
        return false;
    }

    /*
     * The observed wire cost already includes the chosen compressed/raw
     * representation and current extensible header overhead.  Scale it by
     * tile area as a conservative candidate estimate, then require it to use
     * only a configured fraction of the probed packet payload.  Larger tiles
     * often compress better than this estimate, but the slack avoids
     * upshifting into packet fragmentation when content changes slightly.
     */
    const uint64_t estimated_wire = (observed_wire_bytes * candidate_pixels + observed_pixels - 1ull) / observed_pixels;
    const uint64_t packet_budget = ((uint64_t)udp_payload_target + WD_UDP_TILE_HEADER_MAX_SIZE) *
                                   (uint64_t)WD_ADAPTIVE_TILE_FIT_SLACK_PERCENT / 100ull;
    return estimated_wire <= packet_budget;
}

static bool wd_stream_largest_fitting_tile_size(const struct wd_stats* stats, uint16_t current_width, uint16_t current_height,
                                                 uint16_t udp_payload_target, uint16_t* out_width, uint16_t* out_height) {
    if (!out_width || !out_height)
    {
        return false;
    }

    static const struct {
        uint16_t width;
        uint16_t height;
    } candidates[] = {
        {16, 16},
        {32, 32},
        {64, 64},
        {128, 64},
    };

    uint16_t best_width = current_width;
    uint16_t best_height = current_height;
    bool found = false;

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
    {
        const uint16_t candidate_width = candidates[i].width;
        const uint16_t candidate_height = candidates[i].height;

        if (wd_stream_tile_area(candidate_width, candidate_height) < wd_stream_tile_area(current_width, current_height))
        {
            continue;
        }
        if (wd_stream_tile_area(candidate_width, candidate_height) > wd_stream_tile_area(WD_ADAPTIVE_TILE_MAX_WIDTH, WD_ADAPTIVE_TILE_MAX_HEIGHT))
        {
            continue;
        }
        if (!wd_stream_estimated_tile_wire_fits_packet(stats, current_width, current_height, candidate_width, candidate_height,
                                                       udp_payload_target))
        {
            continue;
        }

        best_width = candidate_width;
        best_height = candidate_height;
        found = true;
    }

    if (!found || (best_width == current_width && best_height == current_height))
    {
        return false;
    }

    *out_width = best_width;
    *out_height = best_height;
    return true;
}

static uint64_t wd_stream_client_completion_percent(const struct wd_stats* stats) {
    if (!stats || stats->client_stats_rx == 0 || stats->client_udp_packets_rx < WD_ADAPTIVE_TILE_MIN_CLIENT_PACKETS)
    {
        return 0;
    }

    return (stats->client_completed_packets * 100ull) / stats->client_udp_packets_rx;
}

static bool wd_stream_client_completion_bad(const struct wd_stats* stats) {
    if (!stats || stats->client_stats_rx == 0 || stats->client_udp_packets_rx < WD_ADAPTIVE_TILE_MIN_CLIENT_PACKETS)
    {
        return false;
    }

    return stats->client_completed_packets * 100ull <
           stats->client_udp_packets_rx * (uint64_t)WD_ADAPTIVE_TILE_COMPLETION_BAD_PERCENT;
}

static void wd_stream_policy_update_tile_size_locked(struct wd_server* server, struct wd_stream_policy* policy,
                                                     struct wd_stats* stats, bool bad_window, bool client_completion_low) {
    if (!server || !policy || !stats)
    {
        return;
    }

    if (!server->net.client_connected)
    {
        policy->tile_size_good_windows = 0;
        policy->tile_size_bad_windows  = 0;
        policy->tile_size_change_cooldown_windows = 0;
        return;
    }

    bool tile_change_cooling_down = false;
    if (policy->tile_size_change_cooldown_windows != 0)
    {
        policy->tile_size_change_cooldown_windows--;
        tile_change_cooling_down = true;
    }

    const bool one_packet_tiles = wd_stream_tile_payloads_fit_one_packet(stats, server->net.udp_payload_target);
    const uint64_t completion_percent = wd_stream_client_completion_percent(stats);
    const bool completion_good  = completion_percent >= WD_ADAPTIVE_TILE_COMPLETION_GOOD_PERCENT;
    const bool completion_excellent = completion_percent >= WD_ADAPTIVE_TILE_DIRECT_JUMP_MIN_COMPLETION_PERCENT;
    const bool completion_bad   = wd_stream_client_completion_bad(stats) || client_completion_low;
    const bool multi_packet_avg = stats->udp_tiles_sent != 0 && stats->udp_packets_sent > stats->udp_tiles_sent;

    const bool should_downscale = (bad_window || completion_bad) && multi_packet_avg;
    const bool should_upscale   = !bad_window && !completion_bad && (one_packet_tiles || completion_good) &&
                                  (stats->udp_tiles_sent != 0 || stats->client_tiles_completed != 0);

    uint16_t new_width  = 0;
    uint16_t new_height = 0;
    const char* upscale_reason = NULL;

    if (should_downscale && wd_stream_next_smaller_tile_size(server->tile_width, server->tile_height, &new_width, &new_height))
    {
        policy->tile_size_good_windows = 0;
        if (policy->tile_size_bad_windows < UINT32_MAX)
        {
            policy->tile_size_bad_windows++;
        }
        if (policy->tile_size_bad_windows < WD_ADAPTIVE_TILE_BAD_WINDOWS_TO_DOWNSCALE)
        {
            return;
        }
        policy->tile_size_bad_windows = 0;

        const uint16_t old_width  = server->tile_width;
        const uint16_t old_height = server->tile_height;
        if (wd_server_reconfigure_tile_size_locked(server, new_width, new_height))
        {
            policy->tile_size_change_cooldown_windows = WD_ADAPTIVE_TILE_CHANGE_COOLDOWN_WINDOWS;
            stats->tile_size_downshifts++;
            WD_LOG_INFO("WayDisplay: adaptive tile size down: %ux%u -> %ux%u%s",
                        old_width, old_height, new_width, new_height,
                        completion_bad ? " due to low tile completion" : " due to send/repair pressure");
            (void)wd_server_send_current_config_locked(server);
        }
        return;
    }

    policy->tile_size_bad_windows = 0;

    if (!should_upscale || tile_change_cooling_down)
    {
        policy->tile_size_good_windows = 0;
        return;
    }

    if (policy->tile_size_good_windows < UINT32_MAX)
    {
        policy->tile_size_good_windows++;
    }

    const bool sustained_excellent_completion = completion_excellent &&
                                               policy->tile_size_good_windows >=
                                                   WD_ADAPTIVE_TILE_DIRECT_JUMP_GOOD_WINDOWS;

    if (sustained_excellent_completion)
    {
        new_width = WD_ADAPTIVE_TILE_MAX_WIDTH;
        new_height = WD_ADAPTIVE_TILE_MAX_HEIGHT;
        upscale_reason = " because client completion stayed lossless";
    }
    else if (wd_stream_largest_fitting_tile_size(stats, server->tile_width, server->tile_height, server->net.udp_payload_target,
                                                 &new_width, &new_height))
    {
        upscale_reason = " because estimated tile wire size fits one UDP packet";
    }
    else if (wd_stream_next_larger_tile_size(server->tile_width, server->tile_height, &new_width, &new_height))
    {
        upscale_reason = one_packet_tiles ? " because current tiles fit one UDP packet" : " because client completion is healthy";
    }
    else
    {
        policy->tile_size_good_windows = 0;
        return;
    }

    if ((new_width == server->tile_width && new_height == server->tile_height) ||
        wd_stream_tile_area(new_width, new_height) > wd_stream_tile_area(WD_ADAPTIVE_TILE_MAX_WIDTH, WD_ADAPTIVE_TILE_MAX_HEIGHT))
    {
        policy->tile_size_good_windows = 0;
        return;
    }

    if (policy->tile_size_good_windows < WD_ADAPTIVE_TILE_GOOD_WINDOWS_TO_UPSCALE)
    {
        return;
    }
    policy->tile_size_good_windows = 0;

    const uint16_t old_width  = server->tile_width;
    const uint16_t old_height = server->tile_height;
    if (wd_server_reconfigure_tile_size_locked(server, new_width, new_height))
    {
        policy->tile_size_change_cooldown_windows = WD_ADAPTIVE_TILE_CHANGE_COOLDOWN_WINDOWS;
        stats->tile_size_upshifts++;
        WD_LOG_INFO("WayDisplay: adaptive tile size up: %ux%u -> %ux%u%s",
                    old_width, old_height, new_width, new_height,
                    upscale_reason ? upscale_reason : " because link health allows larger tiles");
        (void)wd_server_send_current_config_locked(server);
    }
}

static void wd_stream_policy_update_frame_rate_locked(struct wd_stream_policy* policy, struct wd_stats* stats, bool bad_window,
                                                       bool send_pressure, bool client_completion_low) {
    if (!policy || !stats)
    {
        return;
    }

    if (policy->target_fps == 0)
    {
        policy->target_fps = WD_DEFAULT_PARTIAL_FPS;
    }
    if (policy->effective_target_fps == 0)
    {
        policy->effective_target_fps = policy->target_fps;
    }

    uint16_t old_fps = wd_stream_policy_effective_fps_locked(policy);

    if (bad_window)
    {
        policy->frame_rate_good_windows = 0;

        uint32_t decrease_percent = send_pressure ? WD_ADAPTIVE_FPS_PRESSURE_DECREASE_PERCENT : WD_ADAPTIVE_FPS_DECREASE_PERCENT;
        uint32_t new_fps = ((uint32_t)old_fps * decrease_percent) / 100u;
        if (new_fps >= old_fps && old_fps > WD_ADAPTIVE_FPS_MIN)
        {
            new_fps = old_fps - 1u;
        }
        if (new_fps < WD_ADAPTIVE_FPS_MIN)
        {
            new_fps = WD_ADAPTIVE_FPS_MIN;
        }

        if ((uint16_t)new_fps != old_fps)
        {
            policy->effective_target_fps = (uint16_t)new_fps;
            policy->last_frame_send_ns = 0;
            stats->frame_rate_downshifts++;
            WD_LOG_INFO("WayDisplay: adaptive frame rate down: %u -> %u fps%s", old_fps, (unsigned)new_fps,
                        send_pressure ? " due to UDP send pressure" :
                        (client_completion_low ? " due to low client completion" : " due to repair pressure"));
        }
        return;
    }

    bool useful_activity = stats->udp_tiles_sent != 0 || stats->dirty_tiles != 0 || stats->client_tiles_completed != 0;
    if (!useful_activity)
    {
        policy->frame_rate_good_windows = 0;
        return;
    }

    if (old_fps >= policy->target_fps)
    {
        policy->effective_target_fps = policy->target_fps;
        policy->frame_rate_good_windows = 0;
        return;
    }

    policy->frame_rate_good_windows++;
    if (policy->frame_rate_good_windows < WD_ADAPTIVE_FPS_GOOD_WINDOWS_TO_INCREASE)
    {
        return;
    }

    policy->frame_rate_good_windows = 0;

    uint32_t percent_fps = ((uint32_t)old_fps * WD_ADAPTIVE_FPS_INCREASE_PERCENT) / 100u;
    uint32_t new_fps = percent_fps > (uint32_t)old_fps ? percent_fps : (uint32_t)old_fps + 1u;
    if (new_fps > policy->target_fps)
    {
        new_fps = policy->target_fps;
    }

    if ((uint16_t)new_fps != old_fps)
    {
        policy->effective_target_fps = (uint16_t)new_fps;
        policy->last_frame_send_ns = 0;
        stats->frame_rate_upshifts++;
        WD_LOG_INFO("WayDisplay: adaptive frame rate up: %u -> %u fps", old_fps, (unsigned)new_fps);
    }
}

static void wd_stream_policy_update_limited_rate_locked(struct wd_stream_policy* policy, struct wd_stats* stats, bool bad_window,
                                                        bool send_pressure, bool client_completion_low) {
    if (!policy || !stats)
    {
        return;
    }

    uint64_t old_rate = wd_stream_clamp_limited_udp_byte_rate(policy->limited_udp_bytes_per_second);
    uint64_t new_rate = old_rate;

    if (bad_window)
    {
        policy->limited_rate_good_windows     = 0;
        policy->client_completion_low_windows = 0;
        policy->throttle_bad_windows++;

        uint32_t decrease_percent = send_pressure ? WD_LIMITED_RATE_PRESSURE_DECREASE_PERCENT : WD_LIMITED_RATE_DECREASE_PERCENT;
        new_rate = old_rate * (uint64_t)decrease_percent / 100ull;

        wd_stream_policy_set_limited_rate_adaptive_locked(policy, new_rate);
        if (policy->limited_udp_bytes_per_second != old_rate)
        {
            stats->limited_rate_downshifts++;
            WD_LOG_INFO("WayDisplay: adaptive limited rate down: %llu -> %llu KiB/s%s",
                        (unsigned long long)(old_rate / 1024ull),
                        (unsigned long long)(policy->limited_udp_bytes_per_second / 1024ull),
                        send_pressure ? " due to UDP send pressure" :
                        (client_completion_low ? " due to low client completion" : " due to repair pressure"));
        }
        return;
    }

    policy->throttle_bad_windows = 0;

    bool useful_tile_activity = stats->udp_tiles_sent != 0 || stats->dirty_tiles != 0;
    if (!useful_tile_activity)
    {
        policy->limited_rate_good_windows = 0;
        return;
    }

    policy->limited_rate_good_windows++;
    if (policy->limited_rate_good_windows < WD_LIMITED_RATE_GOOD_WINDOWS_TO_INCREASE)
    {
        return;
    }

    policy->limited_rate_good_windows = 0;

    uint64_t percent_rate = old_rate * (uint64_t)WD_LIMITED_RATE_INCREASE_PERCENT / 100ull;
    uint64_t step_rate    = old_rate + WD_LIMITED_RATE_INCREASE_MIN_BYTES;
    new_rate              = percent_rate > step_rate ? percent_rate : step_rate;

    wd_stream_policy_set_limited_rate_adaptive_locked(policy, new_rate);
    if (policy->limited_udp_bytes_per_second != old_rate)
    {
        stats->limited_rate_upshifts++;
        WD_LOG_INFO("WayDisplay: adaptive limited rate up: %llu -> %llu KiB/s",
                    (unsigned long long)(old_rate / 1024ull),
                    (unsigned long long)(policy->limited_udp_bytes_per_second / 1024ull));
    }
}

static void wd_stream_policy_update_adaptive_locked(struct wd_server* server, struct wd_stream_policy* policy, struct wd_stats* stats) {
    if (!server || !policy || !stats)
    {
        return;
    }

    bool send_pressure = stats->udp_send_pressure_drops != 0;

    bool client_completion_low_sample = false;
    if (stats->client_stats_rx != 0 && stats->client_udp_packets_rx >= WD_LIMITED_RATE_CLIENT_COMPLETION_MIN_SENT)
    {
        /*
         * Compare client-side completed packet contribution with client-side
         * packets received. Server and client stats windows are not phase
         * locked; comparing server-sent tiles in this second against
         * client-completed tiles reported for a slightly different second can
         * falsely look like congestion and ratchet the stream down.
         */
        client_completion_low_sample = stats->client_completed_packets * 100ull <
                                       stats->client_udp_packets_rx * (uint64_t)WD_LIMITED_RATE_CLIENT_COMPLETION_PERCENT;
    }

    if (client_completion_low_sample)
    {
        if (policy->client_completion_low_windows < UINT32_MAX)
        {
            policy->client_completion_low_windows++;
        }
    }
    else
    {
        policy->client_completion_low_windows = 0;
    }

    bool client_completion_low = policy->client_completion_low_windows >= WD_LIMITED_RATE_CLIENT_COMPLETION_BAD_WINDOWS;
    bool client_repair_pressure = stats->client_partial_tiles_timed_out != 0;
    bool bad_window = send_pressure || client_completion_low || client_repair_pressure;

    /*
     * Stream modes were removed: every session now runs a single adaptive
     * max-rate policy. Start at the probed/capped byte ceiling and adjust the
     * byte budget plus render cadence from real pressure/completion feedback;
     * never downgrade into a separate partial/limited/live state.
     */
    wd_stream_policy_update_tile_size_locked(server, policy, stats, bad_window, client_completion_low);
    wd_stream_policy_update_frame_rate_locked(policy, stats, bad_window, send_pressure, client_completion_low);
    wd_stream_policy_update_limited_rate_locked(policy, stats, bad_window, send_pressure, client_completion_low);
}


bool wd_stream_policy_should_render_now(struct wd_server* server, uint64_t now_ns) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    bool                     full_frame_needed = net->full_frame_needed;
    bool                     client_connected  = net->client_connected;
    struct wd_stream_policy* policy            = &net->stream_policy;

    if (!client_connected)
    {
        pthread_mutex_unlock(&net->lock);
        return false;
    }

    if (!server->scene_dirty && !full_frame_needed)
    {
        pthread_mutex_unlock(&net->lock);
        return false;
    }

    bool should = false;

    uint16_t fps = wd_stream_policy_effective_fps_locked(policy);
    uint64_t interval_ns = 1000000000ull / fps;

    if (policy->last_frame_send_ns == 0 || now_ns - policy->last_frame_send_ns >= interval_ns || full_frame_needed)
    {
        policy->last_frame_send_ns = now_ns;
        should                     = true;
    }


    pthread_mutex_unlock(&net->lock);

    return should;
}

bool wd_stream_init(struct wd_server* server) {
    const size_t compressed_capacity = wd_zstd_compress_bound(server->uncompressed_tile_bytes);

    server->net.tiles = calloc(server->total_tiles, sizeof(*server->net.tiles));
    if (!server->net.tiles)
    {
        return false;
    }

    server->damage_tiles = calloc(server->total_base_tiles, sizeof(*server->damage_tiles));
    if (!server->damage_tiles)
    {
        wd_stream_destroy(server);
        return false;
    }
    server->damage_all_tiles  = true;
    server->damage_tile_count = 0;

    for (uint16_t i = 0; i < server->total_tiles; ++i)
    {
        struct wd_cached_tile* tile = &server->net.tiles[i];

        memset(tile, 0, sizeof(*tile));

        tile->compressed_capacity = (uint32_t)compressed_capacity;
        tile->compressed          = malloc(compressed_capacity);

        if (!tile->compressed)
        {
            wd_stream_destroy(server);
            return false;
        }
    }

    return true;
}

void wd_stream_destroy(struct wd_server* server) {
    if (!server || !server->net.tiles)
    {
        return;
    }

    for (uint16_t i = 0; i < server->total_tiles; ++i)
    {
        free(server->net.tiles[i].compressed);
        server->net.tiles[i].compressed          = NULL;
        server->net.tiles[i].compressed_size     = 0;
        server->net.tiles[i].compressed_capacity = 0;
    }

    free(server->net.tiles);
    server->net.tiles = NULL;

    free(server->damage_tiles);
    server->damage_tiles      = NULL;
    server->damage_all_tiles  = false;
    server->damage_tile_count = 0;
}


static void wd_stream_free_cached_tiles(struct wd_cached_tile* tiles, uint16_t total_tiles) {
    if (!tiles)
    {
        return;
    }

    for (uint16_t i = 0; i < total_tiles; ++i)
    {
        free(tiles[i].compressed);
        tiles[i].compressed = NULL;
    }
    free(tiles);
}

static void wd_stream_free_protocol_tile_state(struct wd_server* server, uint16_t cached_tile_count) {
    if (!server)
    {
        return;
    }

    wd_stream_free_cached_tiles(server->net.tiles, cached_tile_count);
    server->net.tiles = NULL;

    free(server->net.dirty_queue);
    server->net.dirty_queue = NULL;
    free(server->net.dirty_queued);
    server->net.dirty_queued = NULL;
    free(server->net.dirty_queue_enqueued_ns);
    server->net.dirty_queue_enqueued_ns = NULL;
    free(server->net.retransmit_queue);
    server->net.retransmit_queue = NULL;
    free(server->net.retransmit_queued);
    server->net.retransmit_queued = NULL;
    free(server->net.retransmit_queue_enqueued_ns);
    server->net.retransmit_queue_enqueued_ns = NULL;
    free(server->net.retransmit_requested_generation);
    server->net.retransmit_requested_generation = NULL;
    free(server->net.summary_dirty_tiles);
    server->net.summary_dirty_tiles = NULL;
    free(server->net.full_frame_sent_tiles);
    server->net.full_frame_sent_tiles = NULL;

    free(server->damage_tiles);
    server->damage_tiles = NULL;
    server->damage_all_tiles = false;
    server->damage_tile_count = 0;
}

bool wd_server_reconfigure_tile_size_locked(struct wd_server* server, uint16_t tile_width, uint16_t tile_height) {
    if (!server || tile_width == 0 || tile_height == 0)
    {
        return false;
    }

    if (tile_width == server->tile_width && tile_height == server->tile_height)
    {
        return true;
    }

    const uint16_t old_total_tiles = server->total_tiles;

    if (!wd_server_set_tile_size(server, tile_width, tile_height) || !wd_server_set_geometry(server, server->display_width, server->display_height))
    {
        return false;
    }

    wd_stream_free_protocol_tile_state(server, old_total_tiles);

    server->net.dirty_queue                     = calloc(server->total_tiles, sizeof(*server->net.dirty_queue));
    server->net.dirty_queued                    = calloc(server->total_tiles, sizeof(*server->net.dirty_queued));
    server->net.dirty_queue_enqueued_ns         = calloc(server->total_tiles, sizeof(*server->net.dirty_queue_enqueued_ns));
    server->net.retransmit_queue                = calloc(server->total_tiles, sizeof(*server->net.retransmit_queue));
    server->net.retransmit_queued               = calloc(server->total_tiles, sizeof(*server->net.retransmit_queued));
    server->net.retransmit_queue_enqueued_ns    = calloc(server->total_tiles, sizeof(*server->net.retransmit_queue_enqueued_ns));
    server->net.retransmit_requested_generation = calloc(server->total_tiles, sizeof(*server->net.retransmit_requested_generation));
    server->net.summary_dirty_tiles             = calloc(server->total_tiles, sizeof(*server->net.summary_dirty_tiles));
    server->net.full_frame_sent_tiles           = calloc(server->total_tiles, sizeof(*server->net.full_frame_sent_tiles));

    if (!server->net.dirty_queue || !server->net.dirty_queued || !server->net.dirty_queue_enqueued_ns ||
        !server->net.retransmit_queue || !server->net.retransmit_queued || !server->net.retransmit_queue_enqueued_ns ||
        !server->net.retransmit_requested_generation || !server->net.summary_dirty_tiles || !server->net.full_frame_sent_tiles)
    {
        wd_stream_free_protocol_tile_state(server, server->total_tiles);
        return false;
    }

    if (!wd_stream_init(server))
    {
        wd_stream_free_protocol_tile_state(server, server->total_tiles);
        return false;
    }

    server->net.full_frame_needed       = true;
    server->net.full_frame_next_tile    = 0;
    server->net.full_frame_start_ns     = 0;
    server->net.full_frame_tiles_sent   = 0;
    server->net.dirty_scan_next_tile    = 0;
    server->net.dirty_queue_read        = 0;
    server->net.dirty_queue_write       = 0;
    server->net.dirty_queue_count       = 0;
    server->net.retransmit_queue_count  = 0;
    server->net.summary_dirty_count     = 0;
    server->net.dirty_priority_pop_count      = 0;
    server->net.retransmit_priority_pop_count = 0;
    server->net.full_frame_priority_pop_count = 0;
    server->last_summary_ns             = 0;
    server->last_delta_summary_ns       = 0;
    server->scene_dirty                 = true;

    return true;
}

static void wd_note_udp_send_pressure_locked(struct wd_net_state* net, int send_errno) {
    if (!net)
    {
        return;
    }

    net->udp_send_pressure_drops++;
    net->stats.udp_send_pressure_drops++;

    uint64_t now = wd_now_ns();
    if (net->udp_send_pressure_log_ns != 0 && now - net->udp_send_pressure_log_ns < WD_UDP_SEND_PRESSURE_LOG_INTERVAL_NS)
    {
        return;
    }

    uint64_t drops = net->udp_send_pressure_drops;
    net->udp_send_pressure_drops = 0;
    net->udp_send_pressure_log_ns = now;

    WD_LOG_DEBUG("WayDisplay: dropped %llu UDP tile packets under send pressure: %s", (unsigned long long)drops, strerror(send_errno));
}


static void wd_stream_mark_summary_dirty_locked(struct wd_server* server, uint16_t tile_id) {
    if (!server || tile_id >= server->total_tiles)
    {
        return;
    }

    struct wd_net_state* net = &server->net;

    if (!net->summary_dirty_tiles)
    {
        return;
    }

    if (!net->summary_dirty_tiles[tile_id])
    {
        net->summary_dirty_tiles[tile_id] = true;
        net->summary_dirty_count++;
    }
}

static uint8_t wd_stream_tile_protocol_for_packet(uint16_t packet_count, uint64_t input_sequence, bool compressed_payload) {
    if (packet_count <= 1)
    {
        if (compressed_payload)
        {
            return input_sequence ? WD_TILE_COMPRESSED_SINGLE_LATENCY : WD_TILE_COMPRESSED_SINGLE;
        }

        return input_sequence ? WD_TILE_UNCOMPRESSED_SINGLE_LATENCY : WD_TILE_UNCOMPRESSED_SINGLE;
    }

    if (compressed_payload)
    {
        return input_sequence ? WD_TILE_COMPRESSED_MULTI_LATENCY : WD_TILE_COMPRESSED_MULTI;
    }

    return input_sequence ? WD_TILE_UNCOMPRESSED_MULTI_LATENCY : WD_TILE_UNCOMPRESSED_MULTI;
}

static uint32_t wd_stream_tile_wire_bytes_for_payload(uint32_t payload_size, uint16_t udp_payload_target, uint64_t input_sequence,
                                                      bool compressed_payload) {
    if (payload_size == 0)
    {
        return 0;
    }

    if (udp_payload_target == 0)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    if (udp_payload_target > WD_UDP_TILE_PAYLOAD_MAX)
    {
        udp_payload_target = WD_UDP_TILE_PAYLOAD_MAX;
    }

    if (udp_payload_target < 512)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    uint32_t packet_count = (payload_size + udp_payload_target - 1u) / udp_payload_target;
    uint8_t protocol = wd_stream_tile_protocol_for_packet((uint16_t)packet_count, input_sequence, compressed_payload);
    uint16_t header_size = wd_udp_tile_header_size_for_protocol(protocol);
    return payload_size + packet_count * (uint32_t)header_size;
}


static bool wd_stream_use_compressed_tile_payload(uint32_t compressed_size, uint32_t uncompressed_size, uint16_t udp_payload_target,
                                                  uint64_t input_sequence) {
    if (compressed_size == 0)
    {
        return false;
    }

    uint32_t compressed_wire = wd_stream_tile_wire_bytes_for_payload(compressed_size, udp_payload_target, input_sequence, true);
    uint32_t uncompressed_wire = wd_stream_tile_wire_bytes_for_payload(uncompressed_size, udp_payload_target, input_sequence, false);

    return compressed_wire <= uncompressed_wire;
}

static void wd_stream_note_tile_choice_locked(struct wd_net_state* net, uint32_t compressed_size, uint32_t uncompressed_size,
                                              uint16_t udp_payload_target, uint64_t input_sequence, bool compressed_payload) {
    if (!net)
    {
        return;
    }

    uint32_t compressed_wire = wd_stream_tile_wire_bytes_for_payload(compressed_size, udp_payload_target, input_sequence, true);
    uint32_t uncompressed_wire = wd_stream_tile_wire_bytes_for_payload(uncompressed_size, udp_payload_target, input_sequence, false);
    uint32_t chosen_wire = compressed_payload ? compressed_wire : uncompressed_wire;
    uint32_t alternate_wire = compressed_payload ? uncompressed_wire : compressed_wire;

    if (compressed_payload)
    {
        net->stats.tile_choice_compressed++;
    }
    else
    {
        net->stats.tile_choice_uncompressed++;
    }

    net->stats.tile_choice_compressed_payload_sum += compressed_size;
    net->stats.tile_choice_uncompressed_payload_sum += uncompressed_size;
    net->stats.tile_choice_compressed_wire_sum += compressed_wire;
    net->stats.tile_choice_uncompressed_wire_sum += uncompressed_wire;
    net->stats.tile_choice_chosen_wire_sum += chosen_wire;
    if (alternate_wire > chosen_wire)
    {
        net->stats.tile_choice_saved_wire_sum += alternate_wire - chosen_wire;
    }
}

static uint64_t wd_stream_policy_limited_byte_budget_locked(struct wd_stream_policy* policy, uint64_t now_ns) {
    if (!policy)
    {
        return UINT64_MAX;
    }

    uint64_t rate = wd_stream_clamp_limited_udp_byte_rate(policy->limited_udp_bytes_per_second);
    policy->limited_udp_bytes_per_second = rate;

    if (policy->last_limited_udp_byte_refill_ns == 0)
    {
        policy->last_limited_udp_byte_refill_ns = now_ns;
        policy->limited_udp_byte_tokens         = 0.0;
    }
    else
    {
        uint64_t elapsed_ns = now_ns - policy->last_limited_udp_byte_refill_ns;
        double elapsed_seconds = (double)elapsed_ns / (double)WD_NSEC_PER_SEC;
        policy->limited_udp_byte_tokens += elapsed_seconds * (double)rate;

        uint64_t burst_cap = wd_stream_byte_burst_cap_for_rate(rate);
        if (policy->limited_udp_byte_tokens > (double)burst_cap)
        {
            policy->limited_udp_byte_tokens = (double)burst_cap;
        }

        policy->last_limited_udp_byte_refill_ns = now_ns;
    }

    return (uint64_t)policy->limited_udp_byte_tokens;
}

static void wd_stream_policy_consume_limited_bytes_locked(struct wd_stream_policy* policy, uint32_t bytes) {
    if (!policy || bytes == 0)
    {
        return;
    }

    if (policy->limited_udp_byte_tokens >= (double)bytes)
    {
        policy->limited_udp_byte_tokens -= (double)bytes;
    }
    else
    {
        policy->limited_udp_byte_tokens = 0.0;
    }
}

static bool wd_stream_send_tile_payload_locked(struct wd_server* server, uint16_t tile_id, uint64_t generation, uint64_t timestamp_ns,
                                               uint64_t input_sequence, const uint8_t* tile_payload, uint32_t tile_payload_size,
                                               bool compressed_payload, bool* launched, bool* send_blocked, uint32_t* bytes_sent) {
    struct wd_net_state* net = &server->net;

    if (launched)
    {
        *launched = false;
    }

    if (send_blocked)
    {
        *send_blocked = false;
    }

    if (bytes_sent)
    {
        *bytes_sent = 0;
    }

    if (tile_id >= server->total_tiles)
    {
        return false;
    }

    if (!net->client_connected || tile_payload_size == 0 || !tile_payload)
    {
        return true;
    }

    uint16_t udp_payload_target = net->udp_payload_target;

    if (udp_payload_target == 0)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    if (udp_payload_target > WD_UDP_TILE_PAYLOAD_MAX)
    {
        udp_payload_target = WD_UDP_TILE_PAYLOAD_MAX;
    }

    if (udp_payload_target < 512)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    uint16_t packet_count = (uint16_t)((tile_payload_size + udp_payload_target - 1) / udp_payload_target);

    for (uint16_t packet_id = 0; packet_id < packet_count; ++packet_id)
    {
        uint32_t offset = (uint32_t)packet_id * udp_payload_target;

        uint16_t payload_size =
            (uint16_t)(((tile_payload_size - offset) > udp_payload_target) ? udp_payload_target : (tile_payload_size - offset));

        uint8_t header_buf[WD_UDP_TILE_HEADER_MAX_SIZE];
        memset(header_buf, 0, sizeof(header_buf));

        uint8_t protocol = wd_stream_tile_protocol_for_packet(packet_count, input_sequence, compressed_payload);
        uint16_t header_size = wd_udp_tile_header_size_for_protocol(protocol);
        uint8_t tile_size = wd_tile_size_code_for_dimensions(server->tile_width, server->tile_height);

        switch (protocol)
        {
            case WD_TILE_UNCOMPRESSED_SINGLE: {
                struct wd_udp_tile_packet_header_uncompressed_single h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.tile_timestamp_ns = timestamp_ns;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_UNCOMPRESSED_SINGLE_LATENCY: {
                struct wd_udp_tile_packet_header_uncompressed_single_latency h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.tile_timestamp_ns = timestamp_ns;
                h.input_sequence = input_sequence;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_COMPRESSED_SINGLE: {
                struct wd_udp_tile_packet_header_compressed_single h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.compressed_tile_size = (uint16_t)tile_payload_size;
                h.tile_timestamp_ns = timestamp_ns;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_COMPRESSED_SINGLE_LATENCY: {
                struct wd_udp_tile_packet_header_compressed_single_latency h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.compressed_tile_size = (uint16_t)tile_payload_size;
                h.tile_timestamp_ns = timestamp_ns;
                h.input_sequence = input_sequence;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_UNCOMPRESSED_MULTI: {
                struct wd_udp_tile_packet_header_uncompressed_multi h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.tile_pkt_count = (uint8_t)packet_count;
                h.tile_pkt_id = (uint8_t)packet_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.tile_timestamp_ns = timestamp_ns;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_UNCOMPRESSED_MULTI_LATENCY: {
                struct wd_udp_tile_packet_header_uncompressed_multi_latency h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.tile_pkt_count = (uint8_t)packet_count;
                h.tile_pkt_id = (uint8_t)packet_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.tile_timestamp_ns = timestamp_ns;
                h.input_sequence = input_sequence;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_COMPRESSED_MULTI: {
                struct wd_udp_tile_packet_header_compressed_multi h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.tile_pkt_count = (uint8_t)packet_count;
                h.tile_pkt_id = (uint8_t)packet_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.compressed_tile_size = (uint16_t)tile_payload_size;
                h.tile_timestamp_ns = timestamp_ns;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_COMPRESSED_MULTI_LATENCY: {
                struct wd_udp_tile_packet_header_compressed_multi_latency h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.tile_pkt_count = (uint8_t)packet_count;
                h.tile_pkt_id = (uint8_t)packet_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.compressed_tile_size = (uint16_t)tile_payload_size;
                h.tile_timestamp_ns = timestamp_ns;
                h.input_sequence = input_sequence;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            default:
                return false;
        }

        struct iovec iov[2];
        memset(iov, 0, sizeof(iov));
        iov[0].iov_base = header_buf;
        iov[0].iov_len  = header_size;
        iov[1].iov_base = (uint8_t*)tile_payload + offset;
        iov[1].iov_len  = payload_size;

        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_name    = &net->client_udp_addr;
        msg.msg_namelen = sizeof(net->client_udp_addr);
        msg.msg_iov     = iov;
        msg.msg_iovlen  = 2;

        ssize_t sent = sendmsg(net->udp_fd, &msg, 0);

        if (sent < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
            {
                wd_note_udp_send_pressure_locked(net, errno);
                if (send_blocked)
                {
                    *send_blocked = true;
                }
                break;
            }

            WD_LOG_ERROR("WayDisplay: sendto failed: %s", strerror(errno));
            return false;
        }

        if (launched)
        {
            *launched = true;
        }

        net->stats.udp_packets_sent++;
        net->stats.udp_bytes_sent += (uint64_t)sent;
        if (bytes_sent)
        {
            *bytes_sent += (uint32_t)sent;
        }
    }

    if (launched && *launched)
    {
        net->stats.udp_tiles_sent++;
        if (compressed_payload)
        {
            net->stats.udp_compressed_tiles_sent++;
            net->stats.udp_compressed_tile_bytes_sent += tile_payload_size;
        }
        else
        {
            net->stats.udp_uncompressed_tiles_sent++;
            net->stats.udp_uncompressed_tile_bytes_sent += tile_payload_size;
        }
        wd_stream_mark_summary_dirty_locked(server, tile_id);
    }

    return true;
}

bool wd_stream_send_cached_tile_locked(struct wd_server* server, uint16_t tile_id, bool* launched, bool* send_blocked, uint32_t* bytes_sent) {
    struct wd_net_state* net = &server->net;

    if (tile_id >= server->total_tiles)
    {
        return false;
    }

    struct wd_cached_tile* cache = &net->tiles[tile_id];

    return wd_stream_send_tile_payload_locked(server, tile_id, cache->generation, cache->timestamp_ns, cache->input_sequence,
                                              cache->compressed, cache->compressed_size, cache->compressed_payload, launched, send_blocked,
                                              bytes_sent);
}

static uint32_t wd_stream_cached_tile_priority_cost_locked(const struct wd_server* server, uint16_t tile_id) {
    if (!server || tile_id >= server->total_tiles)
    {
        return UINT32_MAX;
    }

    const struct wd_net_state* net = &server->net;
    const struct wd_cached_tile* tile = &net->tiles[tile_id];

    if (!tile->compressed || tile->compressed_size == 0)
    {
        return UINT32_MAX / 2u;
    }

    uint32_t wire_bytes = wd_stream_tile_wire_bytes_for_payload(tile->compressed_size, net->udp_payload_target, tile->input_sequence,
                                                                tile->compressed_payload);
    return wire_bytes ? wire_bytes : (UINT32_MAX / 2u);
}

static bool wd_stream_pointer_priority_active_locked(const struct wd_server* server, uint64_t now_ns) {
    if (!server || !server->net.pointer_priority_valid || server->net.pointer_priority_ns == 0)
    {
        return false;
    }

    if (now_ns < server->net.pointer_priority_ns)
    {
        return false;
    }

    return now_ns - server->net.pointer_priority_ns <= WD_TILE_PRIORITY_POINTER_ACTIVE_NS;
}

static uint32_t wd_stream_tile_pointer_distance_penalty_locked(const struct wd_server* server, uint16_t tile_id) {
    if (!server || tile_id >= server->total_tiles || server->tile_width == 0 || server->tile_height == 0)
    {
        return 0;
    }

    const uint32_t start_x = wd_tile_start_x_for_tile(tile_id, server->tiles_x, server->tile_width);
    const uint32_t start_y = wd_tile_start_y_for_tile(tile_id, server->tiles_x, server->tile_height);
    const uint32_t width   = wd_tile_visible_width_for_tile(server->display_width, tile_id, server->tiles_x, server->tile_width);
    const uint32_t height  = wd_tile_visible_height_for_tile(server->display_height, tile_id, server->tiles_x, server->tile_height);

    const uint32_t center_x = start_x + width / 2u;
    const uint32_t center_y = start_y + height / 2u;
    const uint32_t pointer_x = server->net.pointer_priority_x;
    const uint32_t pointer_y = server->net.pointer_priority_y;

    const uint32_t dx = center_x > pointer_x ? center_x - pointer_x : pointer_x - center_x;
    const uint32_t dy = center_y > pointer_y ? center_y - pointer_y : pointer_y - center_y;

    const uint32_t tile_dx = dx / server->tile_width;
    const uint32_t tile_dy = dy / server->tile_height;
    const uint64_t distance_tiles = (uint64_t)tile_dx + (uint64_t)tile_dy;
    const uint64_t penalty = distance_tiles * (uint64_t)WD_TILE_PRIORITY_POINTER_DISTANCE_WEIGHT_BYTES;

    return penalty > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)penalty;
}

static uint32_t wd_stream_cached_tile_priority_score_locked(const struct wd_server* server, uint16_t tile_id, uint64_t now_ns,
                                                            bool* pointer_priority_active) {
    uint32_t score = wd_stream_cached_tile_priority_cost_locked(server, tile_id);

    bool active = wd_stream_pointer_priority_active_locked(server, now_ns);
    if (pointer_priority_active)
    {
        *pointer_priority_active = active;
    }

    if (!active || score == UINT32_MAX)
    {
        return score;
    }

    const uint32_t penalty = wd_stream_tile_pointer_distance_penalty_locked(server, tile_id);
    if (UINT32_MAX - score < penalty)
    {
        return UINT32_MAX;
    }

    return score + penalty;
}

static void wd_stream_note_queue_age_locked(uint64_t enqueued_ns, uint64_t* samples, uint64_t* sum_ns) {
    if (!enqueued_ns || !samples || !sum_ns)
    {
        return;
    }

    uint64_t now = wd_now_ns();
    if (now < enqueued_ns)
    {
        return;
    }

    (*samples)++;
    *sum_ns += now - enqueued_ns;
}

static void wd_tile_queue_remove_at_locked(uint16_t* queue, uint16_t* queue_count, uint16_t index) {
    if (!queue || !queue_count || index >= *queue_count)
    {
        return;
    }

    uint16_t tail_count = (uint16_t)(*queue_count - index - 1u);
    if (tail_count > 0)
    {
        memmove(&queue[index], &queue[index + 1u], (size_t)tail_count * sizeof(queue[0]));
    }

    (*queue_count)--;
}

static bool wd_dirty_queue_push_locked(struct wd_net_state* net, uint16_t tile_id, uint16_t total_tiles) {
    if (!net || tile_id >= total_tiles)
    {
        return false;
    }

    if (net->dirty_queued[tile_id])
    {
        return true;
    }

    if (net->dirty_queue_count >= total_tiles)
    {
        return false;
    }

    /*
     * Fresh content supersedes any queued repair for the same tile. Leave the
     * stale repair entry in-place; the repair popper will discard it when it
     * sees retransmit_queued[tile_id] is no longer set.
     */
    if (net->retransmit_queued && net->retransmit_queued[tile_id])
    {
        net->retransmit_queued[tile_id] = false;
        if (net->retransmit_queue_enqueued_ns)
        {
            net->retransmit_queue_enqueued_ns[tile_id] = 0;
        }
        if (net->retransmit_requested_generation)
        {
            net->retransmit_requested_generation[tile_id] = 0;
        }
        net->stats.retx_tiles_superseded_by_fresh++;
    }

    net->dirty_queue[net->dirty_queue_count++] = tile_id;
    net->dirty_queued[tile_id] = true;
    if (net->dirty_queue_enqueued_ns)
    {
        net->dirty_queue_enqueued_ns[tile_id] = wd_now_ns();
    }

    return true;
}

static bool wd_dirty_queue_pop_priority_locked(struct wd_server* server, uint16_t* out_tile_id) {
    if (!server || !out_tile_id)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    while (net->dirty_queue_count > 0)
    {
        bool     found      = false;
        uint16_t best_index = 0;
        uint32_t best_score = UINT32_MAX;
        bool     used_pointer_priority = false;
        uint64_t now_ns = wd_now_ns();

        net->dirty_priority_pop_count++;
        bool take_oldest = (WD_TILE_PRIORITY_FAIRNESS_INTERVAL != 0u) &&
                           (net->dirty_priority_pop_count % WD_TILE_PRIORITY_FAIRNESS_INTERVAL == 0u);

        for (uint16_t index = 0; index < net->dirty_queue_count; ++index)
        {
            uint16_t tile_id = net->dirty_queue[index];
            if (tile_id >= server->total_tiles || !net->dirty_queued[tile_id])
            {
                continue;
            }

            if (take_oldest)
            {
                best_index = index;
                found      = true;
                break;
            }

            bool pointer_priority_active = false;
            uint32_t score = wd_stream_cached_tile_priority_score_locked(server, tile_id, now_ns, &pointer_priority_active);
            if (!found || score < best_score)
            {
                best_index = index;
                best_score = score;
                found      = true;
                used_pointer_priority = pointer_priority_active;
            }
        }

        if (!found)
        {
            net->dirty_queue_count = 0;
            return false;
        }

        uint16_t tile_id = net->dirty_queue[best_index];
        wd_tile_queue_remove_at_locked(net->dirty_queue, &net->dirty_queue_count, best_index);

        if (tile_id >= server->total_tiles || !net->dirty_queued[tile_id])
        {
            if (tile_id < server->total_tiles && net->dirty_queue_enqueued_ns)
            {
                net->dirty_queue_enqueued_ns[tile_id] = 0;
            }
            continue;
        }

        net->dirty_queued[tile_id] = false;
        if (used_pointer_priority && !take_oldest)
        {
            net->stats.dirty_pointer_priority_pops++;
        }
        if (net->dirty_queue_enqueued_ns)
        {
            wd_stream_note_queue_age_locked(net->dirty_queue_enqueued_ns[tile_id], &net->stats.dirty_queue_age_samples,
                                            &net->stats.dirty_queue_age_sum_ns);
            net->dirty_queue_enqueued_ns[tile_id] = 0;
        }
        *out_tile_id = tile_id;
        return true;
    }

    return false;
}

static bool wd_dirty_queue_reinsert_locked(struct wd_net_state* net, uint16_t tile_id, uint16_t total_tiles) {
    if (!net || tile_id >= total_tiles)
    {
        return false;
    }

    if (net->dirty_queued[tile_id])
    {
        return true;
    }

    if (net->dirty_queue_count >= total_tiles)
    {
        return false;
    }

    net->dirty_queue[net->dirty_queue_count++] = tile_id;
    net->dirty_queued[tile_id] = true;
    if (net->dirty_queue_enqueued_ns)
    {
        net->dirty_queue_enqueued_ns[tile_id] = wd_now_ns();
    }

    return true;
}

bool wd_stream_queue_retransmit_tile_locked(struct wd_server* server, uint16_t tile_id, uint64_t requested_generation) {
    if (!server || tile_id >= server->total_tiles)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    if (!net->retransmit_queue || !net->retransmit_queued)
    {
        return false;
    }

    if (net->dirty_queued && net->dirty_queued[tile_id])
    {
        net->stats.retx_tiles_superseded_by_fresh++;
        return false;
    }

    if (net->retransmit_queued[tile_id])
    {
        if (net->retransmit_requested_generation && net->retransmit_requested_generation[tile_id] < requested_generation)
        {
            net->retransmit_requested_generation[tile_id] = requested_generation;
        }
        return false;
    }

    if (net->retransmit_queue_count >= server->total_tiles)
    {
        return false;
    }

    net->retransmit_queue[net->retransmit_queue_count++] = tile_id;
    net->retransmit_queued[tile_id] = true;
    if (net->retransmit_requested_generation)
    {
        net->retransmit_requested_generation[tile_id] = requested_generation;
    }
    if (net->retransmit_queue_enqueued_ns)
    {
        net->retransmit_queue_enqueued_ns[tile_id] = wd_now_ns();
    }
    server->scene_dirty = true;

    return true;
}

static bool wd_retransmit_queue_pop_priority_locked(struct wd_server* server, uint16_t* out_tile_id, uint64_t* out_requested_generation) {
    if (!server || !out_tile_id || !out_requested_generation)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    while (net->retransmit_queue_count > 0)
    {
        bool     found      = false;
        uint16_t best_index = 0;
        uint32_t best_score = UINT32_MAX;
        bool     used_pointer_priority = false;
        uint64_t now_ns = wd_now_ns();

        net->retransmit_priority_pop_count++;
        bool take_oldest = (WD_TILE_PRIORITY_FAIRNESS_INTERVAL != 0u) &&
                           (net->retransmit_priority_pop_count % WD_TILE_PRIORITY_FAIRNESS_INTERVAL == 0u);

        for (uint16_t index = 0; index < net->retransmit_queue_count; ++index)
        {
            uint16_t tile_id = net->retransmit_queue[index];
            if (tile_id >= server->total_tiles || !net->retransmit_queued[tile_id])
            {
                continue;
            }

            if (net->dirty_queued && net->dirty_queued[tile_id])
            {
                continue;
            }

            if (take_oldest)
            {
                best_index = index;
                found      = true;
                break;
            }

            bool pointer_priority_active = false;
            uint32_t score = wd_stream_cached_tile_priority_score_locked(server, tile_id, now_ns, &pointer_priority_active);
            if (!found || score < best_score)
            {
                best_index = index;
                best_score = score;
                found      = true;
                used_pointer_priority = pointer_priority_active;
            }
        }

        if (!found)
        {
            net->retransmit_queue_count = 0;
            return false;
        }

        uint16_t tile_id = net->retransmit_queue[best_index];
        wd_tile_queue_remove_at_locked(net->retransmit_queue, &net->retransmit_queue_count, best_index);

        if (tile_id >= server->total_tiles || !net->retransmit_queued[tile_id])
        {
            if (tile_id < server->total_tiles && net->retransmit_queue_enqueued_ns)
            {
                net->retransmit_queue_enqueued_ns[tile_id] = 0;
            }
            if (tile_id < server->total_tiles && net->retransmit_requested_generation)
            {
                net->retransmit_requested_generation[tile_id] = 0;
            }
            continue;
        }

        uint64_t requested_generation = 0;
        if (net->retransmit_requested_generation)
        {
            requested_generation = net->retransmit_requested_generation[tile_id];
            net->retransmit_requested_generation[tile_id] = 0;
        }

        net->retransmit_queued[tile_id] = false;
        if (used_pointer_priority && !take_oldest)
        {
            net->stats.retx_pointer_priority_pops++;
        }
        if (net->retransmit_queue_enqueued_ns)
        {
            wd_stream_note_queue_age_locked(net->retransmit_queue_enqueued_ns[tile_id], &net->stats.retx_queue_age_samples,
                                            &net->stats.retx_queue_age_sum_ns);
            net->retransmit_queue_enqueued_ns[tile_id] = 0;
        }

        if (net->dirty_queued && net->dirty_queued[tile_id])
        {
            net->stats.retx_tiles_superseded_by_fresh++;
            continue;
        }

        struct wd_cached_tile* tile = &net->tiles[tile_id];
        if (requested_generation != 0 && tile->generation < requested_generation)
        {
            net->stats.retx_req_waiting_for_generation++;
            continue;
        }

        *out_tile_id = tile_id;
        *out_requested_generation = requested_generation;
        return true;
    }

    return false;
}


static bool wd_full_frame_pick_tile_locked(struct wd_server* server, uint64_t now_ns, uint16_t* out_tile_id, bool* out_pointer_priority) {
    if (!server || !out_tile_id)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    if (out_pointer_priority)
    {
        *out_pointer_priority = false;
    }

    if (net->full_frame_next_tile >= server->total_tiles)
    {
        return false;
    }

    if (!net->full_frame_sent_tiles)
    {
        *out_tile_id = net->full_frame_next_tile;
        return *out_tile_id < server->total_tiles;
    }

    net->full_frame_priority_pop_count++;
    bool take_oldest = (WD_TILE_PRIORITY_FAIRNESS_INTERVAL != 0u) &&
                       (net->full_frame_priority_pop_count % WD_TILE_PRIORITY_FAIRNESS_INTERVAL == 0u);

    bool     found      = false;
    uint16_t best_tile  = 0;
    uint32_t best_score = UINT32_MAX;
    bool     used_pointer_priority = false;

    for (uint16_t tile_id = 0; tile_id < server->total_tiles; ++tile_id)
    {
        if (net->full_frame_sent_tiles[tile_id])
        {
            continue;
        }

        if (take_oldest)
        {
            best_tile = tile_id;
            found     = true;
            break;
        }

        bool pointer_priority_active = false;
        uint32_t score = wd_stream_cached_tile_priority_score_locked(server, tile_id, now_ns, &pointer_priority_active);
        if (!found || score < best_score)
        {
            best_tile = tile_id;
            best_score = score;
            found = true;
            used_pointer_priority = pointer_priority_active;
        }
    }

    if (!found)
    {
        net->full_frame_next_tile = server->total_tiles;
        return false;
    }

    *out_tile_id = best_tile;
    if (out_pointer_priority)
    {
        *out_pointer_priority = used_pointer_priority && !take_oldest;
    }
    return true;
}

static void wd_full_frame_mark_tile_sent_locked(struct wd_server* server, uint16_t tile_id) {
    if (!server || tile_id >= server->total_tiles)
    {
        return;
    }

    struct wd_net_state* net = &server->net;

    if (net->full_frame_sent_tiles)
    {
        if (net->full_frame_sent_tiles[tile_id])
        {
            return;
        }
        net->full_frame_sent_tiles[tile_id] = true;
    }

    if (net->full_frame_next_tile < server->total_tiles)
    {
        net->full_frame_next_tile++;
    }
}

static void wd_clear_damage_tiles(struct wd_server* server) {
    if (!server || !server->damage_tiles)
    {
        return;
    }

    if (server->damage_tile_count > 0)
    {
        memset(server->damage_tiles, 0, (size_t)server->total_base_tiles * sizeof(*server->damage_tiles));
    }

    server->damage_all_tiles  = false;
    server->damage_tile_count = 0;
}

static void wd_detect_one_dirty_tile_into_queue_locked(struct wd_server* server, uint16_t tile_id) {
    struct wd_net_state* net = &server->net;

    if (tile_id >= server->total_tiles)
    {
        return;
    }

    uint32_t hash = wd_fnv1a_tile_hash_xrgb8888_for_tile(server->framebuffer_xrgb8888, server->display_width, server->display_height,
                                                   server->tiles_x, server->total_tiles, tile_id, server->tile_width, server->tile_height);

    if (hash != net->tiles[tile_id].last_hash)
    {
        wd_dirty_queue_push_locked(net, tile_id, server->total_tiles);
    }
}

static bool wd_protocol_tile_intersects_base_damage(const struct wd_server* server, uint16_t tile_id) {
    if (!server || !server->damage_tiles || tile_id >= server->total_tiles || server->base_tile_width == 0 || server->base_tile_height == 0)
    {
        return false;
    }

    const uint32_t x1 = wd_tile_start_x_for_tile(tile_id, server->tiles_x, server->tile_width);
    const uint32_t y1 = wd_tile_start_y_for_tile(tile_id, server->tiles_x, server->tile_height);
    const uint32_t w  = wd_tile_visible_width_for_tile(server->display_width, tile_id, server->tiles_x, server->tile_width);
    const uint32_t h  = wd_tile_visible_height_for_tile(server->display_height, tile_id, server->tiles_x, server->tile_height);
    if (w == 0 || h == 0)
    {
        return false;
    }

    uint32_t start_x = x1 / server->base_tile_width;
    uint32_t end_x   = (x1 + w - 1u) / server->base_tile_width;
    uint32_t start_y = y1 / server->base_tile_height;
    uint32_t end_y   = (y1 + h - 1u) / server->base_tile_height;

    if (end_x >= server->base_tiles_x)
    {
        end_x = (uint32_t)server->base_tiles_x - 1u;
    }
    if (end_y >= server->base_tiles_y)
    {
        end_y = (uint32_t)server->base_tiles_y - 1u;
    }

    for (uint32_t by = start_y; by <= end_y; ++by)
    {
        const uint32_t row = by * (uint32_t)server->base_tiles_x;
        for (uint32_t bx = start_x; bx <= end_x; ++bx)
        {
            const uint32_t base_tile_id = row + bx;
            if (base_tile_id < server->total_base_tiles && server->damage_tiles[base_tile_id])
            {
                return true;
            }
        }
    }

    return false;
}

static void wd_detect_dirty_tiles_into_queue_locked(struct wd_server* server) {
    /*
     * Track compositor damage in a fixed 16x16 base grid.  The active wire tile
     * grid can be 128x64, 64x64, 32x32, or 16x16; convert base-grid damage to
     * the current protocol tile grid only when deciding which cached tiles need
     * hashing and queuing.
     *
     * Full-frame damage is still used for first frames, resized displays, and
     * any legacy mark path that cannot describe a smaller rectangle safely.
     */
    if (!server->damage_all_tiles && server->damage_tiles && server->damage_tile_count > 0)
    {
        for (uint16_t tile_id = 0; tile_id < server->total_tiles; ++tile_id)
        {
            if (!wd_protocol_tile_intersects_base_damage(server, tile_id))
            {
                continue;
            }

            wd_detect_one_dirty_tile_into_queue_locked(server, tile_id);
        }

        wd_clear_damage_tiles(server);
        return;
    }

    for (uint16_t tile_id = 0; tile_id < server->total_tiles; ++tile_id)
    {
        wd_detect_one_dirty_tile_into_queue_locked(server, tile_id);
    }

    wd_clear_damage_tiles(server);
}

bool wd_stream_send_dirty_tiles(struct wd_server* server) {
    struct wd_net_state* net = &server->net;

    if (!server->framebuffer_xrgb8888)
    {
        return false;
    }

    const uint64_t now = wd_now_ns();

    uint8_t* tile_bytes = malloc(server->uncompressed_tile_bytes);
    if (!tile_bytes)
    {
        return false;
    }

    size_t   compressed_capacity = wd_zstd_compress_bound(server->uncompressed_tile_bytes);
    uint8_t* compressed_tile     = malloc(compressed_capacity);

    if (!compressed_tile)
    {
        free(tile_bytes);
        return false;
    }

    pthread_mutex_lock(&net->lock);

    if (!net->client_connected)
    {
        pthread_mutex_unlock(&net->lock);
        free(compressed_tile);
        free(tile_bytes);
        return true;
    }

    /*
     * Phase 1:
     * Progressive full-frame catch-up for new/reconnected clients.
     *
     * This sends each tile once across multiple adaptive-rate passes. The
     * picker is priority-aware, so reconnect catch-up can paint pointer-adjacent
     * or already-cheap cached tiles before less useful background tiles.
     */
    if (net->full_frame_needed)
    {
        wd_stream_note_full_frame_start_locked(net, now);

        while (net->full_frame_next_tile < server->total_tiles)
        {
            uint16_t tile_id = 0;
            bool used_pointer_priority = false;
            if (!wd_full_frame_pick_tile_locked(server, now, &tile_id, &used_pointer_priority))
            {
                break;
            }

            struct wd_cached_tile* tile = &net->tiles[tile_id];

            uint32_t hash = wd_fnv1a_tile_hash_xrgb8888_for_tile(server->framebuffer_xrgb8888, server->display_width, server->display_height,
                                                           server->tiles_x, server->total_tiles, tile_id, server->tile_width, server->tile_height);

            bool cache_valid = tile->compressed_size > 0 && tile->compressed;

            if (!cache_valid || hash != tile->last_hash)
            {
                if (!wd_extract_tile_xrgb8888_for_tile(server->framebuffer_xrgb8888, server->display_width, server->display_height,
                                                 server->tiles_x, server->total_tiles, tile_id, server->tile_width, server->tile_height, tile_bytes))
                {
                    continue;
                }

                uint32_t compressed_size = 0;

                if (!wd_zstd_compress(tile_bytes, server->uncompressed_tile_bytes, tile->compressed, tile->compressed_capacity, WD_ZSTD_LEVEL,
                                      &compressed_size))
                {
                    WD_LOG_ERROR("WayDisplay: zstd compression failed for full-frame tile %u", tile_id);
                    continue;
                }

                tile->last_hash = hash;
                tile->generation++;
                tile->timestamp_ns = now;
                tile->input_sequence = 0;
                tile->compressed_payload = wd_stream_use_compressed_tile_payload(compressed_size, server->uncompressed_tile_bytes,
                                                                                 net->udp_payload_target, 0);
                wd_stream_note_tile_choice_locked(net, compressed_size, server->uncompressed_tile_bytes, net->udp_payload_target, 0,
                                                  tile->compressed_payload);
                if (!tile->compressed_payload)
                {
                    memcpy(tile->compressed, tile_bytes, server->uncompressed_tile_bytes);
                    tile->compressed_size = server->uncompressed_tile_bytes;
                }
                else
                {
                    tile->compressed_size = compressed_size;
                }

                net->stats.dirty_tiles++;
            }

            bool     launched     = false;
            bool     send_blocked = false;
            uint32_t bytes_sent    = 0;

            uint32_t predicted_bytes = wd_stream_tile_wire_bytes_for_payload(tile->compressed_size, net->udp_payload_target,
                                                                             tile->input_sequence, tile->compressed_payload);
            if (wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now) < predicted_bytes)
            {
                break;
            }

            if (!wd_stream_send_cached_tile_locked(server, tile_id, &launched, &send_blocked, &bytes_sent))
            {
                continue;
            }

            if (!launched && send_blocked)
            {
                break;
            }

            if (launched)
            {
                wd_full_frame_mark_tile_sent_locked(server, tile_id);
                if (used_pointer_priority)
                {
                    net->stats.full_frame_pointer_priority_pops++;
                }
                net->stats.udp_fresh_tiles_sent++;
                wd_stream_note_full_frame_tile_sent_locked(net);
                wd_stream_note_input_to_first_fresh_tile_locked(net, now);
                wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, bytes_sent);
            }

            if (send_blocked)
            {
                break;
            }
        }

        if (net->full_frame_next_tile >= server->total_tiles)
        {
            wd_stream_note_full_frame_complete_locked(net, wd_now_ns());

            net->full_frame_needed    = false;
            net->full_frame_next_tile = 0;
            if (net->full_frame_sent_tiles)
            {
                memset(net->full_frame_sent_tiles, 0, server->total_tiles * sizeof(*net->full_frame_sent_tiles));
            }
            net->dirty_scan_next_tile = 0;

            /*
             * After initial catch-up, clear stale queue state and force the
             * next normal pass to detect current dirty tiles from hashes.
             */
            if (net->dirty_queued)
            {
                memset(net->dirty_queued, 0, server->total_tiles * sizeof(*net->dirty_queued));
            }
            if (net->dirty_queue_enqueued_ns)
            {
                memset(net->dirty_queue_enqueued_ns, 0, server->total_tiles * sizeof(*net->dirty_queue_enqueued_ns));
            }
            if (net->retransmit_queued)
            {
                memset(net->retransmit_queued, 0, server->total_tiles * sizeof(*net->retransmit_queued));
            }
            if (net->retransmit_queue_enqueued_ns)
            {
                memset(net->retransmit_queue_enqueued_ns, 0, server->total_tiles * sizeof(*net->retransmit_queue_enqueued_ns));
            }
            if (net->retransmit_requested_generation)
            {
                memset(net->retransmit_requested_generation, 0, server->total_tiles * sizeof(*net->retransmit_requested_generation));
            }
            if (net->summary_dirty_tiles)
            {
                memset(net->summary_dirty_tiles, 0, server->total_tiles * sizeof(*net->summary_dirty_tiles));
            }
            net->summary_dirty_count    = 0;
            net->dirty_queue_read       = 0;
            net->dirty_queue_write      = 0;
            net->dirty_queue_count      = 0;
            net->retransmit_queue_count = 0;

            wd_clear_damage_tiles(server);
            server->scene_dirty = false;
        }
        else
        {
            server->scene_dirty = true;
        }

        pthread_mutex_unlock(&net->lock);

        free(compressed_tile);
        return true;
    }

    /*
     * Phase 2:
     * Normal adaptive pass. Detect new dirty work and send fresh/latest tiles
     * before repair traffic. Retransmits are useful, but letting them consume
     * the whole adaptive byte budget turns transient summary races into a
     * priority inversion where current UI updates stall behind stale repairs.
     */
    wd_detect_dirty_tiles_into_queue_locked(server);


    while (net->dirty_queue_count > 0)
    {
        uint16_t tile_id = 0;

        if (!wd_dirty_queue_pop_priority_locked(server, &tile_id))
        {
            break;
        }

        if (tile_id >= server->total_tiles)
        {
            continue;
        }

        struct wd_cached_tile* tile = &net->tiles[tile_id];

        uint32_t hash = wd_fnv1a_tile_hash_xrgb8888_for_tile(server->framebuffer_xrgb8888, server->display_width, server->display_height,
                                                       server->tiles_x, server->total_tiles, tile_id, server->tile_width, server->tile_height);

        /*
         * It may have been queued earlier but become identical by now.
         */
        if (hash == tile->last_hash)
        {
            net->stats.dirty_tiles_stale_skipped++;
            continue;
        }

        if (!wd_extract_tile_xrgb8888_for_tile(server->framebuffer_xrgb8888, server->display_width, server->display_height, server->tiles_x,
                                         server->total_tiles, tile_id, server->tile_width, server->tile_height, tile_bytes))
        {
            continue;
        }

        uint32_t compressed_size = 0;

        if (!wd_zstd_compress(tile_bytes, server->uncompressed_tile_bytes, compressed_tile, (uint32_t)compressed_capacity, WD_ZSTD_LEVEL,
                              &compressed_size))
        {
            WD_LOG_ERROR("WayDisplay: zstd compression failed for dirty tile %u", tile_id);
            continue;
        }

        const uint64_t tile_input_sequence = net->input_since_last_fresh_tile ? net->last_input_sequence : 0;
        const bool compressed_payload = wd_stream_use_compressed_tile_payload(compressed_size, server->uncompressed_tile_bytes,
                                                                              net->udp_payload_target, tile_input_sequence);
        wd_stream_note_tile_choice_locked(net, compressed_size, server->uncompressed_tile_bytes, net->udp_payload_target, tile_input_sequence,
                                          compressed_payload);
        const uint8_t* tile_payload = compressed_payload ? compressed_tile : tile_bytes;
        const uint32_t tile_payload_size = compressed_payload ? compressed_size : server->uncompressed_tile_bytes;

        uint64_t next_generation = tile->generation + 1;
        bool     launched        = false;
        bool     send_blocked    = false;
        uint32_t bytes_sent       = 0;

        uint32_t predicted_bytes = wd_stream_tile_wire_bytes_for_payload(tile_payload_size, net->udp_payload_target, tile_input_sequence,
                                                                         compressed_payload);
        if (wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now) < predicted_bytes)
        {
            wd_dirty_queue_reinsert_locked(net, tile_id, server->total_tiles);
            break;
        }

        if (!wd_stream_send_tile_payload_locked(server, tile_id, next_generation, now, tile_input_sequence, tile_payload, tile_payload_size,
                                                compressed_payload, &launched, &send_blocked, &bytes_sent))
        {
            continue;
        }

        if (!launched)
        {
            wd_dirty_queue_reinsert_locked(net, tile_id, server->total_tiles);
            break;
        }

        memcpy(tile->compressed, tile_payload, tile_payload_size);
        tile->last_hash          = hash;
        tile->generation         = next_generation;
        tile->timestamp_ns       = now;
        tile->input_sequence     = tile_input_sequence;
        tile->compressed_size    = tile_payload_size;
        tile->compressed_payload = compressed_payload;

        net->stats.dirty_tiles++;
        net->stats.udp_fresh_tiles_sent++;
        wd_stream_note_input_to_first_fresh_tile_locked(net, now);
        wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, bytes_sent);

        if (send_blocked)
        {
            break;
        }
    }


    while (net->retransmit_queue_count > 0)
    {
        uint16_t tile_id              = 0;
        uint64_t requested_generation = 0;

        if (!wd_retransmit_queue_pop_priority_locked(server, &tile_id, &requested_generation))
        {
            break;
        }

        if (tile_id >= server->total_tiles)
        {
            continue;
        }

        if (net->dirty_queued && net->dirty_queued[tile_id])
        {
            continue;
        }

        bool     launched     = false;
        bool     send_blocked = false;
        uint32_t bytes_sent    = 0;

        struct wd_cached_tile* cache = &net->tiles[tile_id];
        uint32_t predicted_bytes = wd_stream_tile_wire_bytes_for_payload(cache->compressed_size, net->udp_payload_target, cache->input_sequence,
                                                                         cache->compressed_payload);
        if (wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now) < predicted_bytes)
        {
            wd_stream_queue_retransmit_tile_locked(server, tile_id, requested_generation);
            break;
        }

        if (!wd_stream_send_cached_tile_locked(server, tile_id, &launched, &send_blocked, &bytes_sent))
        {
            continue;
        }

        if (send_blocked && !launched)
        {
            wd_stream_queue_retransmit_tile_locked(server, tile_id, requested_generation);
            break;
        }

        if (launched)
        {
            net->stats.udp_retx_tiles_sent++;
            wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, bytes_sent);
        }

        if (send_blocked)
        {
            break;
        }
    }

    /* Keep scene_dirty true if there are queued fresh or repair tiles. */
    server->scene_dirty = net->dirty_queue_count > 0 || net->retransmit_queue_count > 0;

    pthread_mutex_unlock(&net->lock);

    free(compressed_tile);
    free(tile_bytes);
    return true;
}

static bool wd_stream_send_generation_summary_kind_locked(struct wd_server* server, bool full_summary) {
    struct wd_net_state* net = &server->net;

    if (!net->client_connected || net->tcp_fd < 0)
    {
        return true;
    }

    uint16_t requested_tile_count = full_summary ? server->total_tiles : net->summary_dirty_count;
    if (requested_tile_count == 0)
    {
        return true;
    }

    size_t payload_capacity = sizeof(struct wd_tile_summary_payload_header) +
                              (size_t)server->total_tiles * sizeof(struct wd_tile_generation_entry);

    uint8_t* payload = malloc(payload_capacity);
    if (!payload)
    {
        return false;
    }

    struct wd_tile_generation_entry* entries =
        (struct wd_tile_generation_entry*)(payload + sizeof(struct wd_tile_summary_payload_header));

    uint16_t entry_count = 0;

    for (uint16_t i = 0; i < server->total_tiles; ++i)
    {
        if (!full_summary && (!net->summary_dirty_tiles || !net->summary_dirty_tiles[i]))
        {
            continue;
        }

        entries[entry_count].tile_id           = i;
        entries[entry_count].reserved          = 0;
        entries[entry_count].tile_generation   = net->tiles[i].generation;
        entries[entry_count].tile_timestamp_ns = net->tiles[i].timestamp_ns;
        entry_count++;
    }

    if (entry_count == 0)
    {
        free(payload);
        net->summary_dirty_count = 0;
        return true;
    }

    struct wd_tile_summary_payload_header header;

    header.session_id          = net->session_id;
    header.server_timestamp_ns = wd_now_ns();
    header.tile_count          = entry_count;
    header.reserved            = full_summary ? 0u : 1u;

    memcpy(payload, &header, sizeof(header));

    size_t payload_size = sizeof(header) + (size_t)entry_count * sizeof(struct wd_tile_generation_entry);
    bool ok = wd_send_tcp_message(net->tcp_fd, WD_MSG_TILE_GENERATION_SUMMARY, payload, (uint32_t)payload_size);

    if (ok)
    {
        if (full_summary)
        {
            if (net->summary_dirty_tiles)
            {
                memset(net->summary_dirty_tiles, 0, server->total_tiles * sizeof(*net->summary_dirty_tiles));
            }
            net->summary_dirty_count = 0;
            net->stats.tcp_summary_full_tx++;
        }
        else
        {
            for (uint16_t i = 0; i < entry_count; ++i)
            {
                uint16_t tile_id = entries[i].tile_id;
                if (tile_id < server->total_tiles && net->summary_dirty_tiles && net->summary_dirty_tiles[tile_id])
                {
                    net->summary_dirty_tiles[tile_id] = false;
                    if (net->summary_dirty_count > 0)
                    {
                        net->summary_dirty_count--;
                    }
                }
            }
            net->stats.tcp_summary_delta_tx++;
            net->stats.tcp_summary_delta_tiles += entry_count;
        }

        if (net->input_since_last_summary && net->last_input_inject_ns != 0 && header.server_timestamp_ns >= net->last_input_inject_ns)
        {
            net->stats.input_to_summary_samples++;
            net->stats.input_to_summary_sum_ns += header.server_timestamp_ns - net->last_input_inject_ns;
            net->input_since_last_summary = false;
        }

        net->stats.tcp_summary_tx++;
    }

    free(payload);

    return ok;
}

bool wd_stream_send_generation_summary_locked(struct wd_server* server) {
    return wd_stream_send_generation_summary_kind_locked(server, true);
}

bool wd_stream_send_pending_generation_summary_locked(struct wd_server* server) {
    return wd_stream_send_generation_summary_kind_locked(server, false);
}

static double wd_avg_ms(uint64_t sum_ns, uint64_t samples) {
    if (samples == 0)
    {
        return 0.0;
    }

    return (double)sum_ns / (double)samples / 1000000.0;
}

void wd_stream_print_and_reset_stats(struct wd_server* server) {
    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    struct wd_stats s = net->stats;
    memset(&net->stats, 0, sizeof(net->stats));
    wd_stream_policy_update_adaptive_locked(server, &net->stream_policy, &s);
    uint64_t limited_udp_kib_per_second = net->stream_policy.limited_udp_bytes_per_second / 1024ull;
    uint16_t target_fps = net->stream_policy.target_fps;
    uint16_t effective_target_fps = wd_stream_policy_effective_fps_locked(&net->stream_policy);
    uint16_t tile_width = server->tile_width;
    uint16_t tile_height = server->tile_height;
    bool input_channel_connected = net->input_tcp_fd >= 0;
    bool selection_channel_connected = net->selection_tcp_fd >= 0;

    pthread_mutex_unlock(&net->lock);

    static bool     have_prev_state = false;
    static uint16_t prev_target_fps = 0;
    static uint16_t prev_effective_fps = 0;
    static uint64_t prev_limited_kib = 0;
    static uint16_t prev_tile_width = 0;
    static uint16_t prev_tile_height = 0;
    static bool     prev_input_channel = false;
    static bool     prev_selection_channel = false;

    bool state_changed = !have_prev_state ||
                         prev_target_fps != target_fps || prev_effective_fps != effective_target_fps ||
                         prev_limited_kib != limited_udp_kib_per_second || prev_tile_width != tile_width ||
                         prev_tile_height != tile_height || prev_input_channel != input_channel_connected ||
                         prev_selection_channel != selection_channel_connected;

    if (state_changed)
    {
        WD_LOG_DEBUG("WayDisplay state/s: transport=adaptive target_fps=%u effective_fps=%u adaptive_udp_kib_per_sec=%llu tile=%ux%u input_channel=%s selection_channel=%s",
                     (unsigned)target_fps, (unsigned)effective_target_fps,
                     (unsigned long long)limited_udp_kib_per_second, (unsigned)tile_width, (unsigned)tile_height,
                     input_channel_connected ? "yes" : "no", selection_channel_connected ? "yes" : "no");

        have_prev_state = true;
        prev_target_fps = target_fps;
        prev_effective_fps = effective_target_fps;
        prev_limited_kib = limited_udp_kib_per_second;
        prev_tile_width = tile_width;
        prev_tile_height = tile_height;
        prev_input_channel = input_channel_connected;
        prev_selection_channel = selection_channel_connected;
    }

    bool video_activity = s.dirty_tiles != 0 || s.dirty_tiles_stale_skipped != 0 || s.udp_tiles_sent != 0 ||
                          s.udp_fresh_tiles_sent != 0 || s.udp_retx_tiles_sent != 0 || s.udp_packets_sent != 0 ||
                          s.udp_bytes_sent != 0 || s.udp_send_pressure_drops != 0 ||
                          s.tile_choice_compressed != 0 || s.tile_choice_uncompressed != 0 ||
                          s.dirty_queue_age_samples != 0 || s.retx_queue_age_samples != 0 ||
                          s.dirty_pointer_priority_pops != 0 || s.retx_pointer_priority_pops != 0;
    if (video_activity)
    {
        uint64_t choices = s.tile_choice_compressed + s.tile_choice_uncompressed;
        WD_LOG_DEBUG("WayDisplay video/s: dirty=%llu stale_skip=%llu udp_tiles=%llu fresh=%llu retx=%llu pkts=%llu kib=%.1f wire_avg_B=%.1f comp_sent=%llu uncomp_sent=%llu comp_payload_avg_B=%.1f uncomp_payload_avg_B=%.1f choice_comp=%llu choice_uncomp=%llu choice_comp_payload_avg_B=%.1f choice_raw_payload_avg_B=%.1f choice_comp_wire_avg_B=%.1f choice_uncomp_wire_avg_B=%.1f choice_chosen_wire_avg_B=%.1f choice_saved_kib=%.1f pressure_drops=%llu dirty_q_avg_ms=%.2f retx_q_avg_ms=%.2f pointer_prio_dirty=%llu pointer_prio_retx=%llu",
                     (unsigned long long)s.dirty_tiles, (unsigned long long)s.dirty_tiles_stale_skipped,
                     (unsigned long long)s.udp_tiles_sent, (unsigned long long)s.udp_fresh_tiles_sent,
                     (unsigned long long)s.udp_retx_tiles_sent, (unsigned long long)s.udp_packets_sent,
                     (double)s.udp_bytes_sent / 1024.0,
                     s.udp_tiles_sent ? (double)s.udp_bytes_sent / (double)s.udp_tiles_sent : 0.0,
                     (unsigned long long)s.udp_compressed_tiles_sent, (unsigned long long)s.udp_uncompressed_tiles_sent,
                     s.udp_compressed_tiles_sent
                         ? (double)s.udp_compressed_tile_bytes_sent / (double)s.udp_compressed_tiles_sent
                         : 0.0,
                     s.udp_uncompressed_tiles_sent
                         ? (double)s.udp_uncompressed_tile_bytes_sent / (double)s.udp_uncompressed_tiles_sent
                         : 0.0,
                     (unsigned long long)s.tile_choice_compressed, (unsigned long long)s.tile_choice_uncompressed,
                     choices ? (double)s.tile_choice_compressed_payload_sum / (double)choices : 0.0,
                     choices ? (double)s.tile_choice_uncompressed_payload_sum / (double)choices : 0.0,
                     choices ? (double)s.tile_choice_compressed_wire_sum / (double)choices : 0.0,
                     choices ? (double)s.tile_choice_uncompressed_wire_sum / (double)choices : 0.0,
                     choices ? (double)s.tile_choice_chosen_wire_sum / (double)choices : 0.0,
                     (double)s.tile_choice_saved_wire_sum / 1024.0,
                     (unsigned long long)s.udp_send_pressure_drops,
                     wd_avg_ms(s.dirty_queue_age_sum_ns, s.dirty_queue_age_samples),
                     wd_avg_ms(s.retx_queue_age_sum_ns, s.retx_queue_age_samples),
                     (unsigned long long)s.dirty_pointer_priority_pops, (unsigned long long)s.retx_pointer_priority_pops);
    }

    bool repair_activity = s.retx_req_rx != 0 || s.retx_tiles_req != 0 || s.retx_req_ignored_live != 0 ||
                           s.retx_req_stale_generation != 0 || s.retx_req_waiting_for_generation != 0 ||
                           s.retx_tiles_superseded_by_fresh != 0 || s.tcp_summary_tx != 0 || s.tcp_summary_delta_tx != 0 ||
                           s.tcp_summary_delta_tiles != 0 || s.limited_rate_downshifts != 0 || s.limited_rate_upshifts != 0 ||
                           s.frame_rate_downshifts != 0 || s.frame_rate_upshifts != 0 ||
                           s.tile_size_downshifts != 0 || s.tile_size_upshifts != 0;
    if (repair_activity)
    {
        WD_LOG_DEBUG("WayDisplay repair/s: summaries=%llu full=%llu delta=%llu delta_tiles=%llu retx_req=%llu retx_tiles=%llu stale_gen=%llu waiting_gen=%llu ignored_live=%llu superseded=%llu limited_down=%llu limited_up=%llu fps_down=%llu fps_up=%llu tile_down=%llu tile_up=%llu",
                     (unsigned long long)s.tcp_summary_tx, (unsigned long long)s.tcp_summary_full_tx,
                     (unsigned long long)s.tcp_summary_delta_tx, (unsigned long long)s.tcp_summary_delta_tiles,
                     (unsigned long long)s.retx_req_rx, (unsigned long long)s.retx_tiles_req,
                     (unsigned long long)s.retx_req_stale_generation, (unsigned long long)s.retx_req_waiting_for_generation,
                     (unsigned long long)s.retx_req_ignored_live, (unsigned long long)s.retx_tiles_superseded_by_fresh,
                     (unsigned long long)s.limited_rate_downshifts, (unsigned long long)s.limited_rate_upshifts,
                     (unsigned long long)s.frame_rate_downshifts, (unsigned long long)s.frame_rate_upshifts,
                     (unsigned long long)s.tile_size_downshifts, (unsigned long long)s.tile_size_upshifts);
    }

    bool client_activity = s.client_tiles_completed != 0 || s.client_udp_bytes_rx != 0 || s.client_partial_tiles_timed_out != 0 ||
                           s.client_old_generation_tiles != 0 || s.client_retx_requests_tx != 0 ||
                           s.client_udp_interarrival_samples != 0;
    if (client_activity)
    {
        WD_LOG_DEBUG("WayDisplay client/s: reports=%llu completed=%llu udp_kib=%.1f partial_timeouts=%llu old_gen=%llu retx_req_tx=%llu interarrival_avg_ms=%.2f jitter_avg_ms=%.2f max_gap_ms=%.2f",
                     (unsigned long long)s.client_stats_rx, (unsigned long long)s.client_tiles_completed,
                     (double)s.client_udp_bytes_rx / 1024.0, (unsigned long long)s.client_partial_tiles_timed_out,
                     (unsigned long long)s.client_old_generation_tiles, (unsigned long long)s.client_retx_requests_tx,
                     wd_avg_ms(s.client_udp_interarrival_sum_ns, s.client_udp_interarrival_samples),
                     wd_avg_ms(s.client_udp_interarrival_jitter_sum_ns, s.client_udp_interarrival_jitter_samples),
                     (double)s.client_udp_interarrival_max_ns / 1000000.0);
    }

    bool control_activity = s.tcp_hello_rx != 0 || s.tcp_config_tx != 0 || s.tcp_input_channel_rx != 0 ||
                            s.tcp_input_channel_accepted != 0 || s.tcp_input_channel_closed != 0 ||
                            s.tcp_selection_channel_rx != 0 || s.tcp_selection_channel_accepted != 0 ||
                            s.tcp_selection_channel_closed != 0;
    if (control_activity)
    {
        WD_LOG_DEBUG("WayDisplay control/s: hello=%llu config=%llu input_rx=%llu input_accepted=%llu input_closed=%llu selection_rx=%llu selection_accepted=%llu selection_closed=%llu",
                     (unsigned long long)s.tcp_hello_rx, (unsigned long long)s.tcp_config_tx,
                     (unsigned long long)s.tcp_input_channel_rx, (unsigned long long)s.tcp_input_channel_accepted,
                     (unsigned long long)s.tcp_input_channel_closed, (unsigned long long)s.tcp_selection_channel_rx,
                     (unsigned long long)s.tcp_selection_channel_accepted, (unsigned long long)s.tcp_selection_channel_closed);
    }

    bool input_activity = s.key_events_rx != 0 || s.key_events_injected != 0 || s.key_events_dropped != 0 ||
                          s.key_state_duplicate_presses != 0 || s.key_state_release_without_press != 0 ||
                          s.keyboard_enter_events != 0 || s.pointer_events_rx != 0 || s.pointer_events_injected != 0 ||
                          s.pointer_events_dropped != 0 || s.pointer_button_grab_started != 0 ||
                          s.pointer_button_grab_ended != 0 || s.pointer_button_grab_cleared != 0 ||
                          s.pointer_button_grab_surface_destroyed != 0 || s.input_queue_latency_samples != 0 ||
                          s.input_to_summary_samples != 0 || s.input_to_first_fresh_tile_samples != 0;
    if (input_activity)
    {
        WD_LOG_DEBUG("WayDisplay input/s: key_rx=%llu key_injected=%llu key_dropped=%llu dup_press=%llu release_without_press=%llu keyboard_enter=%llu pointer_rx=%llu pointer_injected=%llu pointer_dropped=%llu grabs_start=%llu grabs_end=%llu grabs_clear=%llu grab_surface_destroyed=%llu queue_avg_ms=%.2f input_to_summary_avg_ms=%.2f input_to_first_tile_avg_ms=%.2f",
                     (unsigned long long)s.key_events_rx, (unsigned long long)s.key_events_injected,
                     (unsigned long long)s.key_events_dropped, (unsigned long long)s.key_state_duplicate_presses,
                     (unsigned long long)s.key_state_release_without_press, (unsigned long long)s.keyboard_enter_events,
                     (unsigned long long)s.pointer_events_rx, (unsigned long long)s.pointer_events_injected,
                     (unsigned long long)s.pointer_events_dropped, (unsigned long long)s.pointer_button_grab_started,
                     (unsigned long long)s.pointer_button_grab_ended, (unsigned long long)s.pointer_button_grab_cleared,
                     (unsigned long long)s.pointer_button_grab_surface_destroyed,
                     wd_avg_ms(s.input_queue_latency_sum_ns, s.input_queue_latency_samples),
                     wd_avg_ms(s.input_to_summary_sum_ns, s.input_to_summary_samples),
                     wd_avg_ms(s.input_to_first_fresh_tile_sum_ns, s.input_to_first_fresh_tile_samples));
    }

    bool compositor_activity = s.full_frame_catchup_started != 0 || s.full_frame_catchup_completed != 0 ||
                               s.full_frame_catchup_tiles_sent != 0 || s.full_frame_pointer_priority_pops != 0 ||
                               s.xdg_move_invalid_serial != 0 || s.xdg_resize_invalid_serial != 0 ||
                               s.popup_explicit_scene_trees != 0 || s.popup_explicit_scene_tree_failures != 0 ||
                               s.cursor_shape_requests != 0 || s.cursor_set_cursor_requests != 0 ||
                               s.cursor_set_cursor_rejected != 0 || s.cursor_set_cursor_hidden != 0 ||
                               s.cursor_set_cursor_fallback != 0;
    if (compositor_activity)
    {
        WD_LOG_DEBUG("WayDisplay compositor/s: full_frame_started=%llu completed=%llu tiles=%llu avg_ms=%.2f pointer_prio=%llu xdg_move_bad_serial=%llu xdg_resize_bad_serial=%llu popup_scene=%llu popup_scene_fail=%llu cursor_shape=%llu cursor_set=%llu cursor_reject=%llu cursor_hidden=%llu cursor_fallback=%llu",
                     (unsigned long long)s.full_frame_catchup_started, (unsigned long long)s.full_frame_catchup_completed,
                     (unsigned long long)s.full_frame_catchup_tiles_sent,
                     wd_avg_ms(s.full_frame_catchup_duration_sum_ns, s.full_frame_catchup_completed),
                     (unsigned long long)s.full_frame_pointer_priority_pops,
                     (unsigned long long)s.xdg_move_invalid_serial, (unsigned long long)s.xdg_resize_invalid_serial,
                     (unsigned long long)s.popup_explicit_scene_trees, (unsigned long long)s.popup_explicit_scene_tree_failures,
                     (unsigned long long)s.cursor_shape_requests, (unsigned long long)s.cursor_set_cursor_requests,
                     (unsigned long long)s.cursor_set_cursor_rejected, (unsigned long long)s.cursor_set_cursor_hidden,
                     (unsigned long long)s.cursor_set_cursor_fallback);
    }
}
