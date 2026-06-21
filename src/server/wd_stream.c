#include "waydisplay/wd_net.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "waydisplay/wd_zstd.h"
#include "wd_server.h"
#include "wd_tile_policy.h"
#include "wd_dirty_region_scheduler.h"
#include "wd_async_tcp.h"
#include "wd_async_udp.h"
#include "wd_async_udp_accounting.h"
#include "wd_video_encoder.h"
#include "wd_video_transition.h"
#include "wd_input_correlation.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>


#define WD_ENCODER_MAX_THREADS 4u
#define WD_ENCODER_MAX_RESULTS_PER_JOB 32u
#define WD_DIRTY_REGION_STARVATION_NS 100000000ull
#define WD_TILE_COMPRESSION_MIN_SAVINGS_BYTES 64u
#define WD_TILE_COMPRESSION_MIN_SAVINGS_PERCENT 3u

static void wd_stream_encoder_pool_destroy(struct wd_server* server);
static void wd_stream_encode_workspace_destroy(struct wd_server* server);
static void wd_detect_one_dirty_tile_into_queue_locked(struct wd_server* server, uint16_t tile_id);
static bool wd_stream_collect_wire_tile_base_ids(const struct wd_server* server, uint16_t tile_id, uint16_t tile_width,
                                                 uint16_t tile_height, uint16_t* out_ids, uint16_t* out_count,
                                                 uint16_t max_count);

static void wd_stream_note_input_delivery_locked(struct wd_net_state* net, uint64_t input_sequence,
                                                 uint64_t input_inject_ns, uint64_t delivery_ns,
                                                 bool success) {
    if (!net || input_sequence == 0)
    {
        return;
    }

    const struct wd_input_correlation_completion completion =
        wd_input_correlation_complete(net->input_correlation_inflight_sequence,
                                      net->last_input_sequence, input_sequence, success);
    if (!completion.matched_inflight && net->input_correlation_inflight_sequence != 0)
    {
        return;
    }
    if (completion.matched_inflight)
    {
        net->input_correlation_inflight_sequence = 0;
    }

    if (!success)
    {
        net->stats.input_correlation_delivery_failed++;
        return;
    }

    if (input_inject_ns != 0 && delivery_ns >= input_inject_ns)
    {
        net->stats.input_to_first_fresh_tile_samples++;
        net->stats.input_to_first_fresh_tile_sum_ns += delivery_ns - input_inject_ns;
    }
    if (completion.clear_pending ||
        (net->input_correlation_inflight_sequence == 0 && net->last_input_sequence == input_sequence))
    {
        net->input_since_last_fresh_tile = false;
    }
}



static uint64_t wd_stream_coverage_per_mille(uint64_t tiles, uint32_t total_tiles) {
    if (total_tiles == 0)
    {
        return 0;
    }

    if (tiles > (uint64_t)total_tiles)
    {
        tiles = total_tiles;
    }

    return (tiles * 1000ull) / (uint64_t)total_tiles;
}

static double wd_stream_coverage_pct(uint64_t per_mille) {
    if (per_mille > 1000ull)
    {
        per_mille = 1000ull;
    }

    return (double)per_mille / 10.0;
}

static uint64_t wd_stream_byte_burst_cap_for_rate(uint64_t bytes_per_second) {
    if (bytes_per_second == 0)
    {
        return 0;
    }

    uint64_t cap = bytes_per_second / WD_STREAM_TOKEN_BURST_DIVISOR;
    const uint64_t max_uncompressed_wire_tile =
        (uint64_t)WD_WIRE_TILE_MAX_WIDTH * (uint64_t)WD_WIRE_TILE_MAX_HEIGHT * (uint64_t)WD_BYTES_PER_PIXEL +
        (uint64_t)WD_UDP_TILE_HEADER_MAX_SIZE;
    if (cap < max_uncompressed_wire_tile)
    {
        cap = max_uncompressed_wire_tile;
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
    policy->last_video_frame_send_ns        = 0;
    policy->limited_udp_byte_tokens         = 0.0;
    policy->last_limited_udp_byte_refill_ns = 0;
}

void wd_stream_policy_set_defaults(struct wd_stream_policy* policy) {
    if (!policy)
    {
        return;
    }

    memset(policy, 0, sizeof(*policy));

    policy->requested_capture_fps                    = WD_DEFAULT_PARTIAL_FPS;
    policy->adaptive_capture_fps          = WD_DEFAULT_PARTIAL_FPS;
    policy->stream_mode                   = WD_STREAM_MODE_TILES;
    policy->video_mode                    = WD_VIDEO_MODE_AUTO;
    policy->video_min_dirty_percent       = WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT;
    policy->video_enter_seconds           = WD_VIDEO_ENTER_SECONDS_DEFAULT;
    policy->video_exit_dirty_percent      = WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT;
    policy->video_exit_seconds            = WD_VIDEO_EXIT_SECONDS_DEFAULT;
    policy->video_bitrate_kib_per_second  = 0;
    policy->video_candidate_seconds       = 0;
    policy->tile_recovery_seconds         = 0;
    policy->video_client_failure_seconds  = 0;
    policy->tile_refresh_pending          = false;
    policy->frame_rate_good_seconds       = 0;
    policy->limited_udp_bytes_per_second  = WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    policy->limited_udp_rate_floor        = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
    policy->limited_udp_rate_ceiling      = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
    policy->link_good_seconds             = 0;
    policy->link_loss_seconds             = 0;
    policy->multipacket_loss_cooldown_seconds = 0;
    policy->client_render_pressure_seconds = 0;
    policy->client_render_visible = true;
    wd_stream_policy_reset_tokens(policy);
}

void wd_stream_policy_apply_client_hello(struct wd_stream_policy* policy, const struct wd_client_hello_payload* hello) {
    if (!policy || !hello)
    {
        return;
    }

    uint16_t fps = hello->requested_capture_fps;
    if (fps == 0)
    {
        fps = WD_DEFAULT_PARTIAL_FPS;
    }

    if (fps > WD_MAX_REASONABLE_FPS)
    {
        fps = WD_MAX_REASONABLE_FPS;
    }

    policy->requested_capture_fps           = fps;
    policy->adaptive_capture_fps = fps;
    policy->stream_mode = WD_STREAM_MODE_TILES;
    policy->video_mode = hello->video_mode <= WD_VIDEO_MODE_FORCE ? hello->video_mode : WD_VIDEO_MODE_AUTO;
    policy->video_min_dirty_percent = hello->video_min_dirty_percent != 0 ? hello->video_min_dirty_percent : WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT;
    if (policy->video_min_dirty_percent > WD_VIDEO_MIN_DIRTY_PERCENT_MAX)
    {
        policy->video_min_dirty_percent = WD_VIDEO_MIN_DIRTY_PERCENT_MAX;
    }
    policy->video_enter_seconds = hello->video_enter_seconds != 0 ? hello->video_enter_seconds : WD_VIDEO_ENTER_SECONDS_DEFAULT;
    if (policy->video_enter_seconds > WD_VIDEO_ENTER_SECONDS_MAX)
    {
        policy->video_enter_seconds = WD_VIDEO_ENTER_SECONDS_MAX;
    }
    policy->video_exit_dirty_percent = hello->video_exit_dirty_percent != 0 ? hello->video_exit_dirty_percent : WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT;
    if (policy->video_exit_dirty_percent > WD_VIDEO_EXIT_DIRTY_PERCENT_MAX)
    {
        policy->video_exit_dirty_percent = WD_VIDEO_EXIT_DIRTY_PERCENT_MAX;
    }
    policy->video_exit_seconds = hello->video_exit_seconds != 0 ? hello->video_exit_seconds : WD_VIDEO_EXIT_SECONDS_DEFAULT;
    if (policy->video_exit_seconds > WD_VIDEO_EXIT_SECONDS_MAX)
    {
        policy->video_exit_seconds = WD_VIDEO_EXIT_SECONDS_MAX;
    }
    policy->video_bitrate_kib_per_second = hello->video_bitrate_kib_per_second;
    policy->video_candidate_seconds = 0;
    policy->tile_recovery_seconds = 0;
    policy->video_client_failure_seconds = 0;
    policy->tile_refresh_pending = false;
    policy->frame_rate_good_seconds = 0;
    policy->link_good_seconds = 0;
    policy->link_loss_seconds = 0;
    policy->multipacket_loss_cooldown_seconds = 0;
    policy->client_render_pressure_seconds = 0;
    policy->client_render_visible = true;
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
        policy->link_good_seconds = 0;
    }

    policy->multipacket_loss_cooldown_seconds = 0;

    wd_stream_policy_reset_tokens(policy);
}

static const char* wd_video_mode_name(uint8_t mode) {
    switch (mode)
    {
    case WD_VIDEO_MODE_AUTO:
        return "auto";
    case WD_VIDEO_MODE_OFF:
        return "off";
    case WD_VIDEO_MODE_FORCE:
        return "force";
    default:
        return "unknown";
    }
}

static const char* wd_stream_mode_name(enum wd_stream_mode mode) {
    switch (mode)
    {
    case WD_STREAM_MODE_TILES:
        return "tiles";
    case WD_STREAM_MODE_VIDEO_CANDIDATE:
        return "video-candidate";
    case WD_STREAM_MODE_VIDEO_READY:
        return "video-ready";
    case WD_STREAM_MODE_VIDEO_ACTIVE:
        return "video-active";
    case WD_STREAM_MODE_TILE_RECOVERY:
        return "tile-recovery";
    default:
        return "unknown";
    }
}

static bool wd_stream_mode_uses_video_frames(enum wd_stream_mode mode) {
    return mode == WD_STREAM_MODE_VIDEO_READY || mode == WD_STREAM_MODE_VIDEO_ACTIVE;
}

static bool wd_stream_mode_video_owns_display(enum wd_stream_mode mode) {
    return mode == WD_STREAM_MODE_VIDEO_ACTIVE;
}

static const char* wd_stream_mode_owner_name(enum wd_stream_mode mode) {
    return wd_stream_mode_video_owns_display(mode) ? "video" : "tiles";
}

static void wd_stream_policy_restore_requested_capture_fps_locked(struct wd_stream_policy* policy, const char* reason) {
    if (!policy)
    {
        return;
    }

    if (policy->requested_capture_fps == 0)
    {
        policy->requested_capture_fps = WD_DEFAULT_PARTIAL_FPS;
    }

    uint16_t old_fps = policy->adaptive_capture_fps != 0 ? policy->adaptive_capture_fps : policy->requested_capture_fps;
    if (old_fps == policy->requested_capture_fps)
    {
        policy->adaptive_capture_fps = policy->requested_capture_fps;
        policy->frame_rate_good_seconds = 0;
        return;
    }

    policy->adaptive_capture_fps = policy->requested_capture_fps;
    policy->frame_rate_good_seconds = 0;
    policy->client_render_pressure_seconds = 0;
    policy->last_frame_send_ns = 0;
    policy->last_video_frame_send_ns = 0;

    WD_LOG_INFO("stream capture rate reset: %u -> %u fps due to %s",
                (unsigned)old_fps, (unsigned)policy->requested_capture_fps, reason ? reason : "stream mode change");
}

static void wd_stream_advance_content_epoch_locked(struct wd_server* server, const char* reason) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    net->content_epoch++;
    net->input_correlation_inflight_sequence = 0;
    if (net->content_epoch == 0)
    {
        net->content_epoch = 1;
    }
    WD_LOG_INFO("stream content epoch: epoch=%llu reason=%s",
                (unsigned long long)net->content_epoch, reason ? reason : "ownership transition");
}

static void wd_stream_policy_set_mode_locked(struct wd_stream_policy* policy, enum wd_stream_mode mode,
                                             const char* reason, double dirty_avg_pct, double dirty_peak_pct,
                                             double budget_pressure_pct, bool video_channel_connected,
                                             bool video_encoder_available) {
    if (!policy || policy->stream_mode == mode)
    {
        return;
    }

    enum wd_stream_mode old_mode = policy->stream_mode;
    policy->stream_mode = mode;

    if (wd_stream_mode_video_owns_display(old_mode) && !wd_stream_mode_video_owns_display(mode))
    {
        policy->tile_refresh_pending = true;
    }

    if (wd_stream_mode_uses_video_frames(mode) && !wd_stream_mode_uses_video_frames(old_mode))
    {
        wd_stream_policy_restore_requested_capture_fps_locked(policy, "video mode entry");
    }

    WD_LOG_INFO("stream mode state: %s -> %s reason=%s dirty_avg_pct=%.1f dirty_peak_pct=%.1f budget_pressure_pct=%.1f video_channel=%s video_encoder=%s",
                wd_stream_mode_name(old_mode), wd_stream_mode_name(mode), reason ? reason : "unspecified",
                dirty_avg_pct, dirty_peak_pct, budget_pressure_pct, video_channel_connected ? "yes" : "no",
                video_encoder_available ? "yes" : "no");
}

static void wd_stream_policy_update_mode_locked(struct wd_stream_policy* policy, const struct wd_stats* stats,
                                                uint16_t total_tiles, bool video_negotiated, bool video_channel_connected,
                                                bool video_encoder_available) {
    if (!policy || !stats || total_tiles == 0)
    {
        return;
    }

    (void)total_tiles;

    const uint64_t sample_count = stats->stream_mode_frame_samples != 0 ? stats->stream_mode_frame_samples : 1ull;
    const double dirty_avg_pct = wd_stream_coverage_pct(stats->stream_mode_dirty_coverage_per_mille_sum / sample_count);
    const double dirty_peak_pct = wd_stream_coverage_pct(stats->stream_mode_dirty_coverage_per_mille_peak);
    const double budget_pressure_pct = ((double)stats->stream_mode_budget_pressure_frames / (double)sample_count) * 100.0;

    const uint8_t min_dirty_pct = policy->video_min_dirty_percent != 0
                                      ? policy->video_min_dirty_percent
                                      : WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT;
    const struct wd_video_auto_entry_metrics entry_metrics = {
        .frame_samples = stats->stream_mode_frame_samples,
        .changed_frame_samples = stats->stream_mode_changed_frame_samples,
        .dirty_coverage_per_mille_sum = stats->stream_mode_dirty_coverage_per_mille_sum,
        .dirty_coverage_per_mille_peak = stats->stream_mode_dirty_coverage_per_mille_peak,
        .tile_wire_bytes = stats->udp_bytes_sent,
        .tile_budget_bytes_per_second = policy->limited_udp_bytes_per_second,
        .send_pressure_events = stats->udp_send_pressure_drops,
        .requested_capture_fps = policy->requested_capture_fps,
        .adaptive_capture_fps = policy->adaptive_capture_fps,
        .minimum_dirty_percent = min_dirty_pct,
    };
    const struct wd_video_auto_entry_result auto_entry =
        wd_video_auto_entry_evaluate(&entry_metrics);

    const uint8_t exit_dirty_pct = policy->video_exit_dirty_percent != 0
                                       ? policy->video_exit_dirty_percent
                                       : WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT;
    const uint16_t exit_seconds = policy->video_exit_seconds != 0
                                      ? policy->video_exit_seconds
                                      : WD_VIDEO_EXIT_SECONDS_DEFAULT;

    const bool low_dirty = stats->stream_mode_frame_samples != 0 && dirty_avg_pct <= (double)exit_dirty_pct;
    const bool video_ready = video_negotiated && video_channel_connected && video_encoder_available;
    const bool video_forced = policy->video_mode == WD_VIDEO_MODE_FORCE;
    const bool video_disabled = policy->video_mode == WD_VIDEO_MODE_OFF;
    const bool video_entry_candidate = !video_disabled &&
                                       (video_forced || auto_entry.candidate);

    if (auto_entry.candidate && policy->stream_mode != WD_STREAM_MODE_VIDEO_ACTIVE)
    {
        WD_LOG_DEBUG("video auto candidate: changed_frames=%u%% changed_dirty=%u%% tile_budget=%u%% send_pressure=%llu",
                     (unsigned)auto_entry.changed_frame_percent,
                     (unsigned)auto_entry.changed_dirty_percent,
                     (unsigned)auto_entry.tile_budget_percent,
                     (unsigned long long)stats->udp_send_pressure_drops);
    }

    if (video_disabled || !video_ready)
    {
        policy->video_candidate_seconds = 0;
        policy->tile_recovery_seconds = 0;
        if (policy->stream_mode != WD_STREAM_MODE_TILES)
        {
            wd_stream_policy_set_mode_locked(policy,
                                             wd_stream_mode_video_owns_display(policy->stream_mode) ? WD_STREAM_MODE_TILE_RECOVERY : WD_STREAM_MODE_TILES,
                                             video_disabled ? "video disabled" : "video unavailable",
                                             dirty_avg_pct, dirty_peak_pct, budget_pressure_pct, video_channel_connected,
                                             video_encoder_available);
        }
        return;
    }

    if (video_entry_candidate)
    {
        policy->tile_recovery_seconds = 0;

        if (policy->stream_mode == WD_STREAM_MODE_VIDEO_ACTIVE || policy->stream_mode == WD_STREAM_MODE_VIDEO_READY)
        {
            return;
        }

        if (policy->video_candidate_seconds < UINT32_MAX)
        {
            policy->video_candidate_seconds++;
        }

        const uint16_t enter_seconds = policy->video_enter_seconds != 0
                                           ? policy->video_enter_seconds
                                           : WD_VIDEO_ENTER_SECONDS_DEFAULT;
        if (video_forced || policy->video_candidate_seconds >= enter_seconds)
        {
            wd_stream_policy_set_mode_locked(policy, WD_STREAM_MODE_VIDEO_READY,
                                             video_forced ? "video forced" : "sustained tile cost",
                                             dirty_avg_pct, dirty_peak_pct, budget_pressure_pct, video_channel_connected,
                                             video_encoder_available);
        }
        else
        {
            wd_stream_policy_set_mode_locked(policy, WD_STREAM_MODE_VIDEO_CANDIDATE, "sustained tile cost observed",
                                             dirty_avg_pct, dirty_peak_pct, budget_pressure_pct, video_channel_connected,
                                             video_encoder_available);
        }
        return;
    }

    if (policy->stream_mode == WD_STREAM_MODE_TILES)
    {
        policy->video_candidate_seconds = 0;
        policy->tile_recovery_seconds = 0;
        return;
    }

    policy->video_candidate_seconds = 0;

    if (video_forced)
    {
        policy->tile_recovery_seconds = 0;
        return;
    }

    if (!low_dirty)
    {
        policy->tile_recovery_seconds = 0;
        return;
    }

    if (policy->tile_recovery_seconds < UINT32_MAX)
    {
        policy->tile_recovery_seconds++;
    }

    if (policy->tile_recovery_seconds >= exit_seconds)
    {
        wd_stream_policy_set_mode_locked(policy, WD_STREAM_MODE_TILE_RECOVERY, "tile exit criteria stable",
                                         dirty_avg_pct, dirty_peak_pct, budget_pressure_pct, video_channel_connected,
                                         video_encoder_available);
        policy->tile_recovery_seconds = 0;
    }
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
    policy->link_good_seconds       = 0;
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

static void wd_stream_policy_set_limited_rate_locked(struct wd_stream_policy* policy, uint64_t rate) {
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
    uint16_t fps = policy ? policy->adaptive_capture_fps : 0;
    if (fps == 0 && policy)
    {
        fps = policy->requested_capture_fps;
    }
    if (fps == 0)
    {
        fps = WD_DEFAULT_PARTIAL_FPS;
    }
    if (fps < WD_STREAM_FPS_MIN)
    {
        fps = WD_STREAM_FPS_MIN;
    }
    if (fps > WD_MAX_REASONABLE_FPS)
    {
        fps = WD_MAX_REASONABLE_FPS;
    }
    return fps;
}


static uint16_t wd_stream_policy_capture_pacing_fps_locked(const struct wd_stream_policy* policy,
                                                            uint16_t output_refresh_hz) {
    uint16_t fps = wd_stream_policy_effective_fps_locked(policy);

    if (policy && !policy->client_render_visible && fps > WD_STREAM_HIDDEN_CLIENT_FPS)
    {
        fps = WD_STREAM_HIDDEN_CLIENT_FPS;
    }

    if (fps < WD_STREAM_FPS_MIN)
    {
        fps = WD_STREAM_FPS_MIN;
    }

    return wd_cap_periodic_capture_fps(fps, output_refresh_hz);
}



static bool wd_stream_video_frame_due_locked(const struct wd_stream_policy* policy, uint64_t now_ns) {
    if (!policy)
    {
        return false;
    }

    uint16_t fps = wd_stream_policy_effective_fps_locked(policy);
    if (fps == 0)
    {
        fps = WD_DEFAULT_PARTIAL_FPS;
    }

    const uint64_t interval_ns = WD_NSEC_PER_SEC / fps;
    return policy->last_video_frame_send_ns == 0 || now_ns - policy->last_video_frame_send_ns >= interval_ns;
}

static uint32_t wd_stream_video_bitrate_kib_locked(const struct wd_stream_policy* policy) {
    if (policy && policy->video_bitrate_kib_per_second != 0)
    {
        return policy->video_bitrate_kib_per_second;
    }

    if (!policy || policy->limited_udp_bytes_per_second == 0)
    {
        return WD_VIDEO_DEFAULT_BITRATE_KIB_PER_SECOND;
    }

    uint64_t kib = (policy->limited_udp_bytes_per_second * 3ull) / (4ull * 1024ull);
    if (kib == 0)
    {
        return WD_VIDEO_DEFAULT_BITRATE_KIB_PER_SECOND;
    }

    if (kib > WD_VIDEO_DERIVED_BITRATE_MAX_KIB_PER_SECOND)
    {
        kib = WD_VIDEO_DERIVED_BITRATE_MAX_KIB_PER_SECOND;
    }

    return (uint32_t)kib;
}



struct wd_video_worker_job {
    uint32_t* pixels;
    size_t    pixel_capacity;

    struct wd_video_encoder_config config;
    uint64_t                       epoch;
    uint64_t                       source_content_epoch;
    uint64_t                       published_ns;
    uint64_t                       pts_usec;
    int                            video_tcp_fd;
    bool                           request_keyframe;
};

struct wd_video_worker {
    struct wd_server* server;
    pthread_t         thread;
    pthread_mutex_t   lock;
    pthread_cond_t    cond;
    bool              thread_started;
    bool              stop;
    bool              pending;

    struct wd_video_worker_job pending_job;
    struct wd_video_worker_job active_job;
};

static bool wd_stream_video_job_current_locked(const struct wd_server* server,
                                               const struct wd_video_worker_job* job) {
    if (!server || !job)
    {
        return false;
    }

    const struct wd_net_state* net = &server->net;
    return net->running && net->video_worker_epoch == job->epoch &&
           net->session_id == job->config.session_id && net->connection_token == job->config.connection_token && net->content_epoch == job->source_content_epoch && net->video_tcp_fd == job->video_tcp_fd &&
           net->video_stream_negotiated && net->video_tx &&
           (net->stream_policy.stream_mode == WD_STREAM_MODE_VIDEO_READY ||
            net->stream_policy.stream_mode == WD_STREAM_MODE_VIDEO_ACTIVE);
}

static void wd_stream_video_worker_process(struct wd_video_worker* worker,
                                           struct wd_video_worker_job* job) {
    if (!worker || !worker->server || !job || !job->pixels)
    {
        return;
    }

    struct wd_server* server = worker->server;
    struct wd_net_state* net = &server->net;

    const uint64_t dequeue_ns = wd_now_ns();

    pthread_mutex_lock(&net->lock);
    if (!wd_stream_video_job_current_locked(server, job))
    {
        net->stats.video_worker_stale_drops++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    wd_async_tcp_sender_reap(net->video_tx);
    if (wd_async_tcp_sender_has_message_type(net->video_tx, WD_MSG_VIDEO_FRAME))
    {
        net->stats.video_keyframe_skipped_pending++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    net->stats.video_frame_attempts++;
    if (job->request_keyframe)
    {
        net->stats.video_keyframe_attempts++;
    }
    if (dequeue_ns >= job->published_ns)
    {
        net->stats.video_worker_queue_samples++;
        net->stats.video_worker_queue_ns += dequeue_ns - job->published_ns;
    }
    pthread_mutex_unlock(&net->lock);

    struct wd_video_encoder_input_xrgb8888 input;
    memset(&input, 0, sizeof(input));
    input.pixels        = job->pixels;
    input.width         = job->config.width;
    input.height        = job->config.height;
    input.stride_pixels = job->config.width;
    input.pts_usec      = job->pts_usec;

    struct wd_video_encoder_packet packet;
    memset(&packet, 0, sizeof(packet));

    const uint64_t encode_start_ns = wd_now_ns();
    bool encoded = false;
    bool no_output = false;
    bool payload_invalid = false;
    uint8_t* payload = NULL;
    uint32_t payload_size = 0;
    struct wd_video_frame_payload_header header;
    memset(&header, 0, sizeof(header));
    struct wd_video_entry_plan entry_plan = wd_video_entry_plan_make(job->source_content_epoch, false, false);

    pthread_mutex_lock(&net->video_encoder_lock);
    if (wd_video_encoder_configure(net->video_encoder, &job->config) &&
        (!job->request_keyframe || wd_video_encoder_request_keyframe(net->video_encoder)))
    {
        encoded = wd_video_encoder_encode_xrgb8888(net->video_encoder, &input, &packet);
        no_output = encoded && (!packet.data || packet.header.data_size == 0);
        if (encoded && !no_output)
        {
            header = packet.header;
            header.session_id = job->config.session_id;
            header.connection_token = job->config.connection_token;
            entry_plan = wd_video_entry_plan_make(job->source_content_epoch, job->request_keyframe,
                                                  (header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0);
            header.content_epoch = entry_plan.frame_content_epoch;
            header.codec = job->config.codec;
            header.pts_usec = input.pts_usec;
            header.width = job->config.width;
            header.height = job->config.height;
            if (header.coded_width == 0)
            {
                header.coded_width = header.width;
            }
            if (header.coded_height == 0)
            {
                header.coded_height = header.height;
            }

            const uint64_t payload_size64 = (uint64_t)sizeof(header) + (uint64_t)header.data_size;
            if (header.data_size > WD_VIDEO_FRAME_MAX_PAYLOAD_BYTES || payload_size64 > UINT32_MAX)
            {
                payload_invalid = true;
            }
            else
            {
                payload = malloc((size_t)payload_size64);
                if (!payload)
                {
                    payload_invalid = true;
                }
                else
                {
                    payload_size = (uint32_t)payload_size64;
                    memcpy(payload, &header, sizeof(header));
                    memcpy(payload + sizeof(header), packet.data, header.data_size);
                }
            }
        }
    }
    pthread_mutex_unlock(&net->video_encoder_lock);

    const uint64_t encode_ns = wd_now_ns() - encode_start_ns;

    pthread_mutex_lock(&net->lock);
    net->stats.video_encode_ns += encode_ns;

    if (!encoded || payload_invalid)
    {
        free(payload);
        net->stats.video_encode_failed++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    if (no_output)
    {
        pthread_mutex_unlock(&net->lock);
        return;
    }

    if (!wd_stream_video_job_current_locked(server, job))
    {
        free(payload);
        net->stats.video_worker_stale_drops++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    wd_async_tcp_sender_reap(net->video_tx);
    if (wd_async_tcp_sender_has_message_type(net->video_tx, WD_MSG_VIDEO_FRAME))
    {
        free(payload);
        net->stats.video_keyframe_skipped_pending++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    if (job->request_keyframe && !entry_plan.commit_on_queue)
    {
        free(payload);
        net->stats.video_encode_failed++;
        pthread_mutex_unlock(&net->lock);
        return;
    }

    const bool queued = wd_async_tcp_send_message(net->video_tx, net->video_tcp_fd, WD_MSG_VIDEO_FRAME,
                                                  payload, payload_size);
    free(payload);

    if (!queued)
    {
        net->stats.video_tcp_send_failed++;
        wd_stream_video_reset_locked(server, "video tcp send failed", false, false);
        pthread_mutex_unlock(&net->lock);
        return;
    }

    net->stats.video_frames_tx++;
    if ((header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0)
    {
        net->stats.video_keyframes_tx++;
    }
    net->stats.video_tcp_bytes_tx += payload_size;

    if (net->stream_policy.stream_mode == WD_STREAM_MODE_VIDEO_READY &&
        wd_video_entry_plan_can_commit(&entry_plan, net->content_epoch, true))
    {
        net->content_epoch = entry_plan.frame_content_epoch;
        net->input_correlation_inflight_sequence = 0;
        WD_LOG_INFO("stream content epoch: epoch=%llu reason=first video keyframe queued",
                    (unsigned long long)net->content_epoch);
        wd_stream_policy_set_mode_locked(&net->stream_policy, WD_STREAM_MODE_VIDEO_ACTIVE,
                                         "video keyframe queued", 0.0, 0.0, 0.0, true, true);
    }

    pthread_mutex_unlock(&net->lock);
}

static void* wd_stream_video_worker_main(void* data) {
    struct wd_video_worker* worker = data;
    if (!worker)
    {
        return NULL;
    }

    for (;;)
    {
        pthread_mutex_lock(&worker->lock);
        while (!worker->stop && !worker->pending)
        {
            pthread_cond_wait(&worker->cond, &worker->lock);
        }

        if (worker->stop)
        {
            pthread_mutex_unlock(&worker->lock);
            break;
        }

        struct wd_video_worker_job tmp = worker->active_job;
        worker->active_job = worker->pending_job;
        worker->pending_job = tmp;
        worker->pending = false;
        pthread_mutex_unlock(&worker->lock);

        wd_stream_video_worker_process(worker, &worker->active_job);
    }

    return NULL;
}

static bool wd_stream_video_worker_init(struct wd_server* server) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    if (!wd_video_encoder_available(net->video_encoder))
    {
        return true;
    }
    if (net->video_worker)
    {
        return true;
    }

    struct wd_video_worker* worker = calloc(1, sizeof(*worker));
    if (!worker)
    {
        return false;
    }
    worker->server = server;

    if (pthread_mutex_init(&worker->lock, NULL) != 0)
    {
        free(worker);
        return false;
    }
    if (pthread_cond_init(&worker->cond, NULL) != 0)
    {
        pthread_mutex_destroy(&worker->lock);
        free(worker);
        return false;
    }

    net->video_worker_epoch = 1;
    net->video_worker = worker;
    if (pthread_create(&worker->thread, NULL, wd_stream_video_worker_main, worker) != 0)
    {
        net->video_worker = NULL;
        pthread_cond_destroy(&worker->cond);
        pthread_mutex_destroy(&worker->lock);
        free(worker);
        return false;
    }
    worker->thread_started = true;
    return true;
}

static void wd_stream_video_worker_destroy(struct wd_server* server) {
    if (!server || !server->net.video_worker)
    {
        return;
    }

    struct wd_video_worker* worker = server->net.video_worker;
    server->net.video_worker = NULL;

    pthread_mutex_lock(&worker->lock);
    worker->stop = true;
    worker->pending = false;
    pthread_cond_broadcast(&worker->cond);
    pthread_mutex_unlock(&worker->lock);

    if (worker->thread_started)
    {
        pthread_join(worker->thread, NULL);
    }

    free(worker->pending_job.pixels);
    free(worker->active_job.pixels);
    pthread_cond_destroy(&worker->cond);
    pthread_mutex_destroy(&worker->lock);
    free(worker);
}

static void wd_stream_video_worker_discard_locked(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    net->video_worker_epoch++;
    if (net->video_worker_epoch == 0)
    {
        net->video_worker_epoch = 1;
    }

    struct wd_video_worker* worker = net->video_worker;
    if (!worker)
    {
        return;
    }

    pthread_mutex_lock(&worker->lock);
    if (worker->pending)
    {
        worker->pending = false;
        net->stats.video_worker_stale_drops++;
    }
    pthread_mutex_unlock(&worker->lock);
}

static bool wd_stream_queue_video_control_frame_locked(struct wd_server* server, uint16_t flags) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    if (!net->video_stream_negotiated || net->video_tcp_fd < 0 || !net->video_tx)
    {
        return false;
    }

    struct wd_video_frame_payload_header header;
    memset(&header, 0, sizeof(header));
    header.session_id = net->session_id;
    header.connection_token = net->connection_token;
    header.content_epoch = net->content_epoch;
    header.codec      = net->video_codecs != 0 ? net->video_codecs : WD_VIDEO_CODEC_H265;
    header.flags      = flags;
    header.pts_usec   = wd_now_ns() / 1000ull;
    header.width        = (uint16_t)server->display_width;
    header.height       = (uint16_t)server->display_height;
    header.coded_width  = (uint16_t)server->display_width;
    header.coded_height = (uint16_t)server->display_height;

    const bool queued = wd_async_tcp_send_message(net->video_tx, net->video_tcp_fd, WD_MSG_VIDEO_FRAME,
                                                  &header, (uint32_t)sizeof(header));
    if (!queued)
    {
        net->stats.video_tcp_send_failed++;
        return false;
    }

    net->stats.video_control_frames_tx++;
    net->stats.video_tcp_bytes_tx += sizeof(header);
    if ((flags & WD_VIDEO_FRAME_END_OF_STREAM) != 0)
    {
        net->stats.video_end_of_stream_tx++;
    }
    return true;
}

void wd_stream_video_reset_locked(struct wd_server* server, const char* reason, bool notify_client, bool resize) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    const bool leaving_video = wd_stream_mode_video_owns_display(net->stream_policy.stream_mode);
    if (resize || leaving_video)
    {
        wd_stream_advance_content_epoch_locked(server, reason ? reason : "video reset");
    }
    if (net->video_tx)
    {
        (void)wd_async_tcp_sender_drop_message_type(net->video_tx, WD_MSG_VIDEO_FRAME);
    }

    if (notify_client)
    {
        uint16_t flags = WD_VIDEO_FRAME_END_OF_STREAM;
        if (resize)
        {
            flags |= WD_VIDEO_FRAME_RESIZE;
        }
        (void)wd_stream_queue_video_control_frame_locked(server, flags);
    }

    /* Invalidate both the pending mailbox frame and any frame currently being
     * encoded. The active worker will validate the epoch before it queues its
     * packet, so reset/resize cannot publish stale output. */
    wd_stream_video_worker_discard_locked(server);

    pthread_mutex_lock(&net->video_encoder_lock);
    wd_video_encoder_reset(net->video_encoder);
    pthread_mutex_unlock(&net->video_encoder_lock);
    net->stream_policy.last_video_frame_send_ns = 0;
    net->stream_policy.video_candidate_seconds = 0;
    net->stream_policy.tile_recovery_seconds = 0;
    net->stats.video_resets++;
    if (resize)
    {
        net->stats.video_resize_resets++;
    }

    if (net->stream_policy.stream_mode != WD_STREAM_MODE_TILES)
    {
        wd_stream_policy_set_mode_locked(&net->stream_policy,
                                         net->client_connected ? WD_STREAM_MODE_TILE_RECOVERY : WD_STREAM_MODE_TILES,
                                         reason ? reason : "video reset", 0.0, 0.0, 0.0,
                                         net->video_tcp_fd >= 0, wd_video_encoder_available(net->video_encoder));
    }
    else if (reason)
    {
        WD_LOG_INFO("video stream reset: reason=%s notify_client=%s resize=%s video_channel=%s",
                    reason, notify_client ? "yes" : "no", resize ? "yes" : "no",
                    net->video_tcp_fd >= 0 ? "yes" : "no");
    }
}

static bool wd_stream_try_publish_video_frame_locked(struct wd_server* server, uint64_t now_ns) {
    if (!server || !server->framebuffer_xrgb8888)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    struct wd_video_worker* worker = net->video_worker;
    if (!worker ||
        (net->stream_policy.stream_mode != WD_STREAM_MODE_VIDEO_READY &&
         net->stream_policy.stream_mode != WD_STREAM_MODE_VIDEO_ACTIVE) ||
        !net->video_stream_negotiated || net->video_tcp_fd < 0 || !net->video_tx ||
        !wd_video_encoder_available(net->video_encoder))
    {
        return false;
    }

    wd_async_tcp_sender_reap(net->video_tx);
    if (wd_async_tcp_sender_has_message_type(net->video_tx, WD_MSG_VIDEO_FRAME))
    {
        net->stats.video_keyframe_skipped_pending++;
        return false;
    }

    if (!wd_stream_video_frame_due_locked(&net->stream_policy, now_ns))
    {
        return false;
    }

    const uint32_t width = server->display_width;
    const uint32_t height = server->display_height;
    if (width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX)
    {
        net->stats.video_encode_failed++;
        return false;
    }

    const size_t pixel_count = (size_t)width * (size_t)height;
    if ((height != 0 && pixel_count / height != width) ||
        pixel_count > SIZE_MAX / sizeof(uint32_t))
    {
        net->stats.video_encode_failed++;
        return false;
    }

    struct wd_video_encoder_config config;
    memset(&config, 0, sizeof(config));
    config.session_id = net->session_id;
    config.connection_token = net->connection_token;
    config.content_epoch = net->content_epoch;
    config.width = (uint16_t)width;
    config.height = (uint16_t)height;
    config.target_fps = wd_stream_policy_effective_fps_locked(&net->stream_policy);
    config.bitrate_kib_per_second = wd_stream_video_bitrate_kib_locked(&net->stream_policy);
    config.codec = net->video_codecs != 0 ? net->video_codecs : WD_VIDEO_CODEC_H265;

    const bool request_keyframe = net->stream_policy.stream_mode == WD_STREAM_MODE_VIDEO_READY;
    const uint64_t copy_start_ns = wd_now_ns();

    pthread_mutex_lock(&worker->lock);
    if (worker->stop)
    {
        pthread_mutex_unlock(&worker->lock);
        return false;
    }

    if (worker->pending_job.pixel_capacity < pixel_count)
    {
        uint32_t* new_pixels = realloc(worker->pending_job.pixels, pixel_count * sizeof(uint32_t));
        if (!new_pixels)
        {
            pthread_mutex_unlock(&worker->lock);
            net->stats.video_encode_failed++;
            return false;
        }
        worker->pending_job.pixels = new_pixels;
        worker->pending_job.pixel_capacity = pixel_count;
    }

    const bool superseded = worker->pending;
    memcpy(worker->pending_job.pixels, server->framebuffer_xrgb8888,
           pixel_count * sizeof(*worker->pending_job.pixels));
    worker->pending_job.config = config;
    worker->pending_job.epoch = net->video_worker_epoch;
    worker->pending_job.source_content_epoch = net->content_epoch;
    worker->pending_job.published_ns = now_ns;
    worker->pending_job.pts_usec = now_ns / 1000ull;
    worker->pending_job.video_tcp_fd = net->video_tcp_fd;
    worker->pending_job.request_keyframe = request_keyframe;
    worker->pending = true;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->lock);

    net->stats.video_frames_published++;
    if (superseded)
    {
        net->stats.video_frames_superseded++;
    }
    net->stats.video_publish_copy_samples++;
    net->stats.video_publish_copy_ns += wd_now_ns() - copy_start_ns;

    /* This timestamp paces capture/publication, not TCP completion. The worker
     * may discard an older pending frame when a fresher one arrives. */
    net->stream_policy.last_video_frame_send_ns = now_ns;
    return true;
}

static bool wd_stream_client_packet_loss_sample(const struct wd_stats* stats) {
    if (!stats || stats->client_stats_rx == 0 || stats->client_udp_packets_rx < WD_STREAM_CLIENT_COMPLETION_MIN_PACKETS)
    {
        return false;
    }

    return stats->client_completed_packets * 100ull <
           stats->client_udp_packets_rx * (uint64_t)WD_STREAM_CLIENT_COMPLETION_LOSS_PERCENT;
}

static bool wd_stream_client_render_pressure_sample(const struct wd_stream_policy* policy, const struct wd_stats* stats) {
    if (!policy || !stats || stats->client_stats_rx == 0)
    {
        return false;
    }

    if (stats->client_render_hidden_reports != 0 || stats->client_render_visible_reports == 0)
    {
        return false;
    }

    const uint16_t effective_fps = wd_stream_policy_effective_fps_locked(policy);
    if (effective_fps == 0 || stats->stream_mode_changed_frame_samples == 0)
    {
        return false;
    }

    /* A tile client has no reason to present at the configured FPS when the
     * scene is static. Compare remote presentations only with scene frames
     * that actually produced changed pixels. Cap demand by the number of FPS
     * slots covered by the received one-second reports so a delayed health
     * sample cannot manufacture impossible demand. */
    const uint64_t report_capacity =
        (uint64_t)effective_fps * (uint64_t)stats->client_stats_rx;
    const uint64_t render_demand =
        stats->stream_mode_changed_frame_samples < report_capacity ?
            stats->stream_mode_changed_frame_samples : report_capacity;

    if (render_demand != 0 &&
        stats->client_render_frames * 100ull <
            render_demand * WD_STREAM_CLIENT_RENDER_FPS_PRESSURE_PERCENT)
    {
        return true;
    }

    /* Present latency is useful telemetry, but a client can report high
     * SDL_RenderPresent() samples because it was allowed to over-present local
     * dirty updates, because the window system briefly blocked, or because a
     * single visible sample spiked. Input-to-present latency is even broader
     * telemetry because it includes server scheduling, application damage
     * behavior, link/repair delays, and input-sequence correlation. Neither
     * latency metric should ratchet the sender down by itself. */
    return false;
}

static bool wd_stream_client_reporting_tile_loss_locked(const struct wd_stream_policy* policy, const struct wd_stats* stats) {
    if (stats && (stats->client_partial_tiles_timed_out != 0 || stats->client_retx_requests_tx != 0 || wd_stream_client_packet_loss_sample(stats)))
    {
        return true;
    }

    return policy && policy->multipacket_loss_cooldown_seconds != 0;
}

static void wd_stream_policy_update_frame_rate_locked(struct wd_stream_policy* policy, struct wd_stats* stats, bool frame_pressure,
                                                       bool strong_pressure, const char* pressure_reason) {
    if (!policy || !stats)
    {
        return;
    }

    if (policy->requested_capture_fps == 0)
    {
        policy->requested_capture_fps = WD_DEFAULT_PARTIAL_FPS;
    }
    if (policy->adaptive_capture_fps == 0)
    {
        policy->adaptive_capture_fps = policy->requested_capture_fps;
    }

    uint16_t old_fps = wd_stream_policy_effective_fps_locked(policy);

    if (frame_pressure)
    {
        policy->frame_rate_good_seconds = 0;

        uint32_t decrease_percent = strong_pressure ? WD_STREAM_FPS_PRESSURE_DECREASE_PERCENT : WD_STREAM_FPS_DECREASE_PERCENT;
        uint32_t new_fps = ((uint32_t)old_fps * decrease_percent) / 100u;
        if (new_fps >= old_fps && old_fps > WD_STREAM_FPS_MIN)
        {
            new_fps = old_fps - 1u;
        }
        if (new_fps < WD_STREAM_FPS_MIN)
        {
            new_fps = WD_STREAM_FPS_MIN;
        }

        if ((uint16_t)new_fps != old_fps)
        {
            policy->adaptive_capture_fps = (uint16_t)new_fps;
            policy->last_frame_send_ns = 0;
            stats->frame_rate_downshifts++;
            WD_LOG_INFO("stream capture rate down: %u -> %u fps due to %s", old_fps, (unsigned)new_fps,
                        pressure_reason ? pressure_reason : "stream pressure");
        }
        return;
    }

    bool useful_activity = stats->udp_tiles_sent != 0 || stats->dirty_tiles != 0 || stats->client_tiles_completed != 0;
    if (!useful_activity)
    {
        policy->frame_rate_good_seconds = 0;
        return;
    }

    if (old_fps >= policy->requested_capture_fps)
    {
        policy->adaptive_capture_fps = policy->requested_capture_fps;
        policy->frame_rate_good_seconds = 0;
        return;
    }

    policy->frame_rate_good_seconds++;
    if (policy->frame_rate_good_seconds < WD_STREAM_FPS_GOOD_SECONDS_TO_INCREASE)
    {
        return;
    }

    policy->frame_rate_good_seconds = 0;

    uint32_t percent_fps = ((uint32_t)old_fps * WD_STREAM_FPS_INCREASE_PERCENT) / 100u;
    uint32_t new_fps = percent_fps > (uint32_t)old_fps ? percent_fps : (uint32_t)old_fps + 1u;
    if (new_fps > policy->requested_capture_fps)
    {
        new_fps = policy->requested_capture_fps;
    }

    if ((uint16_t)new_fps != old_fps)
    {
        policy->adaptive_capture_fps = (uint16_t)new_fps;
        policy->last_frame_send_ns = 0;
        stats->frame_rate_upshifts++;
        WD_LOG_INFO("stream capture rate up: %u -> %u fps", old_fps, (unsigned)new_fps);
    }
}

static void wd_stream_policy_update_limited_rate_locked(struct wd_stream_policy* policy, struct wd_stats* stats, bool rate_pressure) {
    if (!policy || !stats)
    {
        return;
    }

    const bool useful_tile_activity = stats->udp_tiles_sent != 0 || stats->dirty_tiles != 0 || stats->client_tiles_completed != 0;
    uint64_t old_rate = wd_stream_clamp_limited_udp_byte_rate(policy->limited_udp_bytes_per_second);
    uint64_t new_rate = old_rate;

    if (rate_pressure)
    {
        policy->link_good_seconds = 0;
        if (policy->link_loss_seconds < UINT32_MAX)
        {
            policy->link_loss_seconds++;
        }

        if (policy->link_loss_seconds < WD_STREAM_LINK_LOSS_SECONDS_TO_DECREASE)
        {
            return;
        }

        policy->link_loss_seconds = 0;

        new_rate = old_rate * (uint64_t)WD_STREAM_RATE_PRESSURE_DECREASE_PERCENT / 100ull;

        wd_stream_policy_set_limited_rate_locked(policy, new_rate);
        if (policy->limited_udp_bytes_per_second != old_rate)
        {
            stats->rate_decreases++;
            WD_LOG_INFO("stream byte budget down: %llu -> %llu KiB/s due to UDP send pressure",
                        (unsigned long long)(old_rate / 1024ull),
                        (unsigned long long)(policy->limited_udp_bytes_per_second / 1024ull));
        }
        return;
    }

    policy->link_loss_seconds = 0;
    if (!useful_tile_activity)
    {
        policy->link_good_seconds = 0;
        return;
    }

    if (policy->link_good_seconds < UINT32_MAX)
    {
        policy->link_good_seconds++;
    }
    if (policy->link_good_seconds < WD_STREAM_LINK_GOOD_SECONDS_TO_INCREASE)
    {
        return;
    }

    policy->link_good_seconds = 0;

    uint64_t percent_rate = old_rate * (uint64_t)WD_STREAM_RATE_INCREASE_PERCENT / 100ull;
    uint64_t step_rate    = old_rate + WD_STREAM_RATE_INCREASE_MIN_BYTES;
    new_rate              = percent_rate > step_rate ? percent_rate : step_rate;

    wd_stream_policy_set_limited_rate_locked(policy, new_rate);
    if (policy->limited_udp_bytes_per_second != old_rate)
    {
        stats->rate_increases++;
        WD_LOG_INFO("stream byte budget up: %llu -> %llu KiB/s",
                    (unsigned long long)(old_rate / 1024ull),
                    (unsigned long long)(policy->limited_udp_bytes_per_second / 1024ull));
    }
}

static void wd_stream_policy_update_health_locked(struct wd_stream_policy* policy, struct wd_stats* stats) {
    if (!policy || !stats)
    {
        return;
    }

    if (stats->client_render_hidden_reports != 0)
    {
        policy->client_render_visible = false;
    }
    else if (stats->client_render_visible_reports != 0)
    {
        policy->client_render_visible = true;
    }

    const bool send_pressure = stats->udp_send_pressure_drops != 0;
    /*
     * dirty_budget_blocked means the sender had fresh dirty work ready, but
     * the configured/probed UDP byte budget could not admit the next tile.
     * That is different from socket send pressure: the link may be healthy,
     * but the requested FPS is too high for the available stream budget and
     * frame size.  Treat it as frame-rate pressure so the sender accumulates
     * more byte tokens per output frame instead of visually dribbling partial
     * frame coverage at the nominal FPS.  Keep byte-budget adaptation tied to
     * real UDP send pressure below.
     */
    const uint64_t normal_dirty_budget_blocked =
        stats->dirty_budget_blocked > stats->dirty_budget_blocked_full_refresh ?
            stats->dirty_budget_blocked - stats->dirty_budget_blocked_full_refresh : 0;
    const bool budget_frame_pressure = normal_dirty_budget_blocked != 0 &&
                                       (stats->dirty_tiles != 0 || stats->udp_fresh_tiles_sent != 0);
    const bool client_render_pressure_sample = wd_stream_client_render_pressure_sample(policy, stats);
    if (!policy->client_render_visible || !client_render_pressure_sample)
    {
        policy->client_render_pressure_seconds = 0;
    }
    else if (policy->client_render_pressure_seconds < UINT32_MAX)
    {
        policy->client_render_pressure_seconds++;
    }
    const bool client_render_pressure = policy->client_render_pressure_seconds >= WD_STREAM_CLIENT_RENDER_PRESSURE_SECONDS_TO_DECREASE;
    const bool client_render_pressure_warming = client_render_pressure_sample && !client_render_pressure;
    const bool client_packet_loss = wd_stream_client_packet_loss_sample(stats);
    const bool client_tile_repair = stats->client_partial_tiles_timed_out != 0 || stats->client_retx_requests_tx != 0;

    const bool video_frame_mode = wd_stream_mode_uses_video_frames(policy->stream_mode);

    if (video_frame_mode)
    {
        const bool client_saw_video = stats->client_video_data_frames_rx != 0 || stats->client_video_frames_rx != 0 ||
                                      stats->client_video_control_frames_rx != 0;
        const bool client_presented_video = stats->client_video_frames_presented != 0 ||
                                            stats->client_video_last_frame_id_presented != 0;
        const bool client_reported_video_failure = stats->client_video_decode_failed != 0 ||
                                                   stats->client_video_publish_failed != 0 ||
                                                   stats->client_video_need_keyframe_drops != 0;

        if (stats->video_frames_tx != 0 && stats->client_stats_rx != 0 &&
            ((client_saw_video && !client_presented_video) || client_reported_video_failure))
        {
            if (policy->video_client_failure_seconds < UINT32_MAX)
            {
                policy->video_client_failure_seconds++;
            }
        }
        else
        {
            policy->video_client_failure_seconds = 0;
        }

        if (policy->video_client_failure_seconds >= 3)
        {
            wd_stream_policy_set_mode_locked(policy, WD_STREAM_MODE_TILE_RECOVERY,
                                             "client video decode/present failure", 0.0, 0.0, 0.0,
                                             true, true);
            policy->video_client_failure_seconds = 0;
            return;
        }

        policy->multipacket_loss_cooldown_seconds = 0;
        policy->link_loss_seconds = 0;
        policy->link_good_seconds = 0;
        policy->client_render_pressure_seconds = 0;
        if (policy->adaptive_capture_fps != policy->requested_capture_fps)
        {
            wd_stream_policy_restore_requested_capture_fps_locked(policy, "video mode frame pacing");
        }
        return;
    }

    if (client_tile_repair || client_packet_loss)
    {
        policy->multipacket_loss_cooldown_seconds = WD_STREAM_MULTIPACKET_LOSS_COOLDOWN_SECONDS;
    }
    else if (policy->multipacket_loss_cooldown_seconds != 0)
    {
        policy->multipacket_loss_cooldown_seconds--;
    }

    /* Tile loss is a repair signal, not a congestion signal.  It temporarily
     * disables fragmented 128x64 tiles through multipacket_loss_cooldown, but
     * it must not shrink the byte budget or frame rate by itself.  Otherwise a
     * few stale/superseded repair requests can force the sender into many more
     * tiny tiles, which creates the downward spiral seen on small-MTU links. */
    if (client_render_pressure_warming && !send_pressure && !budget_frame_pressure)
    {
        /* Client render samples are quantized to the one-second feedback cadence.
         * A capped client can occasionally report one short sample below the
         * threshold because the sample window and local present interval are not
         * phase-aligned.  Hold the current FPS while this warms up instead of
         * ratcheting down or immediately counting it as a good second. */
        policy->frame_rate_good_seconds = 0;
    }
    else
    {
        const bool stream_frame_pressure = send_pressure || budget_frame_pressure;
        const bool tile_frame_pressure = stream_frame_pressure;
        const char* pressure_reason = NULL;
        if (!video_frame_mode && send_pressure)
        {
            pressure_reason = "UDP send pressure";
        }
        else if (!video_frame_mode && budget_frame_pressure)
        {
            pressure_reason = "UDP budget pressure";
        }
        else if (client_render_pressure)
        {
            pressure_reason = "client render pressure";
        }

        wd_stream_policy_update_frame_rate_locked(policy, stats, tile_frame_pressure || client_render_pressure,
                                                  tile_frame_pressure, pressure_reason);
    }
    wd_stream_policy_update_limited_rate_locked(policy, stats, send_pressure);
}


bool wd_stream_policy_should_render_now(struct wd_server* server, uint64_t now_ns) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    bool                     client_connected  = net->client_connected;
    struct wd_stream_policy* policy            = &net->stream_policy;

    if (!client_connected || net->config_update_pending)
    {
        pthread_mutex_unlock(&net->lock);
        return false;
    }

    if (!server->scene_dirty)
    {
        pthread_mutex_unlock(&net->lock);
        return false;
    }

    bool should = false;

    const uint16_t output_refresh_hz =
        (uint16_t)((server->output_refresh_mhz + 500u) / 1000u);
    uint16_t fps = wd_stream_policy_capture_pacing_fps_locked(policy, output_refresh_hz);
    uint64_t interval_ns = WD_NSEC_PER_SEC / fps;

    if (policy->last_frame_send_ns == 0 || now_ns - policy->last_frame_send_ns >= interval_ns)
    {
        policy->last_frame_send_ns = now_ns;
        should                     = true;
    }


    pthread_mutex_unlock(&net->lock);

    return should;
}

void wd_stream_invalidate_all_tiles_locked(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;

    if (net->tiles)
    {
        for (uint16_t i = 0; i < server->total_tiles; ++i)
        {
            net->tiles[i].input_sequence = 0;
        }
    }

    if (server->damage_tiles && server->total_base_tiles > 0)
    {
        memset(server->damage_tiles, 0, (size_t)server->total_base_tiles * sizeof(*server->damage_tiles));
    }

    server->damage_all_tiles  = true;
    server->damage_tile_count = 0;
    server->scene_dirty       = true;
    server->framebuffer_shadow_valid = false;
}

bool wd_stream_init(struct wd_server* server) {
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
    server->framebuffer_shadow_valid = false;

    if (!wd_stream_video_worker_init(server))
    {
        WD_LOG_ERROR("failed to create video encoder worker");
        wd_stream_destroy(server);
        return false;
    }

    return true;
}

void wd_stream_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    wd_stream_video_worker_destroy(server);
    wd_stream_encoder_pool_destroy(server);
    wd_stream_encode_workspace_destroy(server);

    if (!server->net.tiles)
    {
        return;
    }

    free(server->net.tiles);
    server->net.tiles = NULL;

    free(server->damage_tiles);
    server->damage_tiles      = NULL;
    server->damage_all_tiles  = false;
    server->damage_tile_count = 0;
}


static void wd_stream_free_tile_states(struct wd_tile_state* tiles, uint16_t total_tiles) {
    (void)total_tiles;
    free(tiles);
}

static void wd_stream_free_protocol_tile_state(struct wd_server* server, uint16_t tile_state_count) {
    if (!server)
    {
        return;
    }

    wd_stream_free_tile_states(server->net.tiles, tile_state_count);
    server->net.tiles = NULL;

    wd_dirty_region_scheduler_destroy(server->net.dirty_region_scheduler);
    server->net.dirty_region_scheduler = NULL;

    free(server->net.dirty_regions);
    server->net.dirty_regions = NULL;
    free(server->net.dirty_region_queued);
    server->net.dirty_region_queued = NULL;
    server->net.dirty_region_count = 0;
    free(server->net.dirty_epochs);
    server->net.dirty_epochs = NULL;
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

    free(server->net.summary_dirty_queue);
    server->net.summary_dirty_queue = NULL;
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

    server->net.dirty_regions                   = calloc(server->total_tiles, sizeof(*server->net.dirty_regions));
    server->net.dirty_region_queued             = calloc(server->total_tiles, sizeof(*server->net.dirty_region_queued));
    server->net.dirty_region_count              = 0;
    server->net.dirty_epochs                    = calloc(server->total_tiles, sizeof(*server->net.dirty_epochs));
    server->net.dirty_queue                     = calloc(server->total_tiles, sizeof(*server->net.dirty_queue));
    server->net.dirty_queued                    = calloc(server->total_tiles, sizeof(*server->net.dirty_queued));
    server->net.dirty_queue_enqueued_ns         = calloc(server->total_tiles, sizeof(*server->net.dirty_queue_enqueued_ns));
    server->net.retransmit_queue                = calloc(server->total_tiles, sizeof(*server->net.retransmit_queue));
    server->net.retransmit_queued               = calloc(server->total_tiles, sizeof(*server->net.retransmit_queued));
    server->net.retransmit_queue_enqueued_ns    = calloc(server->total_tiles, sizeof(*server->net.retransmit_queue_enqueued_ns));
    server->net.retransmit_requested_generation = calloc(server->total_tiles, sizeof(*server->net.retransmit_requested_generation));
    server->net.summary_dirty_tiles             = calloc(server->total_tiles, sizeof(*server->net.summary_dirty_tiles));
    server->net.summary_dirty_queue             = calloc(server->total_tiles, sizeof(*server->net.summary_dirty_queue));

    if (!server->net.dirty_regions || !server->net.dirty_region_queued || !server->net.dirty_epochs || !server->net.dirty_queue ||
        !server->net.dirty_queued || !server->net.dirty_queue_enqueued_ns ||
        !server->net.retransmit_queue || !server->net.retransmit_queued || !server->net.retransmit_queue_enqueued_ns ||
        !server->net.retransmit_requested_generation || !server->net.summary_dirty_tiles || !server->net.summary_dirty_queue)
    {
        wd_stream_free_protocol_tile_state(server, server->total_tiles);
        return false;
    }

    if (!wd_stream_init(server))
    {
        wd_stream_free_protocol_tile_state(server, server->total_tiles);
        return false;
    }

    server->net.config_epoch++;
    if (server->net.config_epoch == 0)
    {
        server->net.config_epoch = 1;
    }
    wd_stream_video_reset_locked(server, "tile geometry reconfigure", true, true);

    server->net.dirty_queue_read        = 0;
    server->net.dirty_queue_write       = 0;
    server->net.dirty_queue_count       = 0;
    server->net.retransmit_queue_count  = 0;
    server->net.summary_dirty_count     = 0;
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
    if (!wd_log_rate_limit_should_log(&net->udp_send_pressure_log_ns, now, WD_LOG_RATE_LIMIT_INTERVAL_NS))
    {
        return;
    }

    uint64_t drops = net->udp_send_pressure_drops;
    net->udp_send_pressure_drops = 0;

    WD_LOG_DEBUG("dropped %llu UDP tile packets under send pressure: %s", (unsigned long long)drops, strerror(send_errno));
}


static void wd_stream_mark_summary_dirty_locked(struct wd_server* server, uint16_t tile_id) {
    if (!server || tile_id >= server->total_tiles)
    {
        return;
    }

    struct wd_net_state* net = &server->net;

    if (!net->summary_dirty_tiles || !net->summary_dirty_queue)
    {
        return;
    }

    if (!net->summary_dirty_tiles[tile_id])
    {
        if (net->summary_dirty_count >= server->total_tiles)
        {
            return;
        }

        net->summary_dirty_tiles[tile_id] = true;
        net->summary_dirty_queue[net->summary_dirty_count++] = tile_id;
    }
}

static uint32_t wd_stream_tile_wire_bytes_for_payload(uint32_t payload_size, uint16_t udp_payload_target, uint64_t input_sequence,
                                                      bool compressed_payload) {
    (void)compressed_payload;
    udp_payload_target = wd_tile_normalize_udp_payload_target(udp_payload_target, WD_UDP_PAYLOAD_TARGET,
                                                              WD_UDP_TILE_PAYLOAD_MAX);
    return wd_tile_wire_bytes_for_payload(payload_size, udp_payload_target, WD_UDP_TILE_HEADER_MIN_SIZE,
                                          input_sequence ? WD_UDP_TILE_HEADER_MAX_SIZE : WD_UDP_TILE_HEADER_MIN_SIZE);
}

static bool wd_stream_use_compressed_tile_payload(uint32_t compressed_size, uint32_t uncompressed_size, uint16_t udp_payload_target,
                                                  uint64_t input_sequence) {
    udp_payload_target = wd_tile_normalize_udp_payload_target(udp_payload_target, WD_UDP_PAYLOAD_TARGET,
                                                              WD_UDP_TILE_PAYLOAD_MAX);
    return wd_tile_compression_is_worthwhile(
        compressed_size, uncompressed_size, udp_payload_target, WD_UDP_TILE_HEADER_MIN_SIZE,
        input_sequence ? WD_UDP_TILE_HEADER_MAX_SIZE : WD_UDP_TILE_HEADER_MIN_SIZE,
        WD_TILE_COMPRESSION_MIN_SAVINGS_BYTES, WD_TILE_COMPRESSION_MIN_SAVINGS_PERCENT);
}

static void wd_stream_note_tile_choice_locked(struct wd_net_state* net, uint32_t compressed_size, uint32_t uncompressed_size,
                                              uint16_t udp_payload_target, uint64_t input_sequence, bool compressed_payload,
                                              uint16_t tile_width, uint16_t tile_height) {
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

    if (tile_width == 128 && tile_height == 64)
    {
        net->stats.tile_size_128x64_sent++;
    }
    else if (tile_width == 64 && tile_height == 64)
    {
        net->stats.tile_size_64x64_sent++;
    }
    else if (tile_width == 32 && tile_height == 32)
    {
        net->stats.tile_size_32x32_sent++;
    }
    else if (tile_width == 16 && tile_height == 16)
    {
        net->stats.tile_size_16x16_sent++;
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

static void wd_stream_policy_consume_limited_bytes_locked(struct wd_stream_policy* policy, uint64_t bytes) {
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

static void wd_stream_policy_refund_limited_bytes_locked(struct wd_stream_policy* policy, uint64_t bytes) {
    if (!policy || bytes == 0)
    {
        return;
    }

    uint64_t rate = wd_stream_clamp_limited_udp_byte_rate(policy->limited_udp_bytes_per_second);
    policy->limited_udp_bytes_per_second = rate;

    policy->limited_udp_byte_tokens += (double)bytes;

    uint64_t burst_cap = wd_stream_byte_burst_cap_for_rate(rate);
    if (policy->limited_udp_byte_tokens > (double)burst_cap)
    {
        policy->limited_udp_byte_tokens = (double)burst_cap;
    }
}

bool wd_stream_try_consume_tcp_control_budget_locked(struct wd_net_state* net, uint32_t bytes, uint64_t now_ns) {
    if (!net || bytes == 0)
    {
        return true;
    }

    uint64_t budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now_ns);
    if (budget < (uint64_t)bytes)
    {
        net->stats.tcp_budget_blocked++;
        return false;
    }

    wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, bytes);
    net->stats.tcp_control_bytes_sent += bytes;
    return true;
}

void wd_stream_account_tcp_control_bytes_locked(struct wd_net_state* net, uint32_t bytes) {
    if (!net || bytes == 0)
    {
        return;
    }

    wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, bytes);
    net->stats.tcp_control_bytes_sent += bytes;
}

static void wd_stream_refund_tcp_control_budget_locked(struct wd_net_state* net, uint64_t bytes) {
    if (!net || bytes == 0)
    {
        return;
    }

    wd_stream_policy_refund_limited_bytes_locked(&net->stream_policy, bytes);
    net->stats.tcp_control_bytes_refunded += bytes;
}

static void wd_stream_init_send_result(struct wd_udp_tile_send_result* result) {
    if (!result)
    {
        return;
    }

    memset(result, 0, sizeof(*result));
}

struct wd_udp_tile_delivery {
    struct wd_server* server;
    struct wd_tile_delivery_status status;
    struct wd_stream_epoch_identity epoch;
    uint64_t generation;
    uint64_t input_sequence;
    uint64_t input_inject_ns;
    uint8_t* encoded_payload;
    uint16_t covered_base_ids[64];
    uint16_t covered_base_count;
};

static void wd_stream_finish_udp_tile_delivery(struct wd_udp_tile_delivery* delivery, bool failed) {
    if (!delivery)
    {
        return;
    }
    if (delivery->server)
    {
        struct wd_server* server = delivery->server;
        struct wd_net_state* net = &server->net;
        const struct wd_stream_epoch_identity current = {
            .connection_epoch = net->connection_epoch,
            .config_epoch = net->config_epoch,
            .content_epoch = net->content_epoch,
            .framebuffer_generation = server->framebuffer_generation,
        };
        if (wd_stream_epoch_identity_equal(&delivery->epoch, &current))
        {
            if (delivery->input_sequence != 0)
            {
                wd_stream_note_input_delivery_locked(net, delivery->input_sequence,
                                                     delivery->input_inject_ns, wd_now_ns(), !failed);
            }
            if (failed)
            {
                for (uint16_t i = 0; i < delivery->covered_base_count; ++i)
                {
                    const uint16_t tile_id = delivery->covered_base_ids[i];
                    if (tile_id < server->total_tiles && net->tiles &&
                        net->tiles[tile_id].generation <= delivery->generation)
                    {
                        wd_detect_one_dirty_tile_into_queue_locked(server, tile_id);
                    }
                }
            }
        }
    }
    free(delivery->encoded_payload);
    free(delivery);
}

static void wd_stream_udp_tile_packet_completion(void* user_data, bool success) {
    struct wd_udp_tile_delivery* delivery = user_data;
    bool failed = false;
    if (delivery && wd_tile_delivery_status_complete(&delivery->status, success, &failed))
    {
        wd_stream_finish_udp_tile_delivery(delivery, failed);
    }
}

static void wd_stream_seal_udp_tile_delivery(struct wd_udp_tile_delivery* delivery, bool failed) {
    if (!delivery)
    {
        return;
    }
    delivery->status.failed = delivery->status.failed || failed;
    bool final_failed = false;
    if (wd_tile_delivery_status_seal(&delivery->status, &final_failed))
    {
        wd_stream_finish_udp_tile_delivery(delivery, final_failed);
    }
}

static bool wd_stream_send_tile_payload_sized_locked(struct wd_server* server, uint16_t tile_id, uint16_t tile_width, uint16_t tile_height,
                                                     uint64_t generation, uint64_t input_sequence,
                                                     uint8_t** tile_payload_io, uint32_t tile_payload_size,
                                                     bool compressed_payload, struct wd_udp_tile_send_result* result) {
    struct wd_net_state* net = &server->net;
    uint8_t* tile_payload = tile_payload_io ? *tile_payload_io : NULL;

    wd_stream_init_send_result(result);

    const uint16_t packet_tiles_x = wd_tiles_for_width_with_tile(server->display_width, tile_width);
    const uint16_t packet_tiles_y = wd_tiles_for_height_with_tile(server->display_height, tile_height);
    const uint32_t packet_total_tiles = (uint32_t)packet_tiles_x * (uint32_t)packet_tiles_y;
    if (tile_width == 0 || tile_height == 0 || packet_tiles_x == 0 || packet_tiles_y == 0 || tile_id >= packet_total_tiles ||
        packet_total_tiles > UINT16_MAX)
    {
        return false;
    }

    if (!net->client_connected || tile_payload_size == 0 || !tile_payload)
    {
        return true;
    }

    struct wd_udp_tile_delivery* delivery = NULL;
    if (net->udp_tx)
    {
        delivery = calloc(1, sizeof(*delivery));
        if (!delivery)
        {
            return false;
        }
        delivery->server = server;
        delivery->epoch.connection_epoch = net->connection_epoch;
        delivery->epoch.config_epoch = net->config_epoch;
        delivery->epoch.content_epoch = net->content_epoch;
        delivery->epoch.framebuffer_generation = server->framebuffer_generation;
        delivery->generation = generation;
        if (!wd_stream_collect_wire_tile_base_ids(server, tile_id, tile_width, tile_height,
                                                   delivery->covered_base_ids, &delivery->covered_base_count,
                                                   (uint16_t)(sizeof(delivery->covered_base_ids) /
                                                              sizeof(delivery->covered_base_ids[0]))))
        {
            free(delivery);
            return false;
        }
        delivery->encoded_payload = tile_payload;
        if (tile_payload_io)
        {
            *tile_payload_io = NULL;
        }
    }

    const uint16_t udp_payload_target = wd_tile_normalize_udp_payload_target(
        net->udp_payload_target, WD_UDP_PAYLOAD_TARGET, WD_UDP_TILE_PAYLOAD_MAX);
    const uint16_t packet_count = wd_tile_packet_count_for_payload(tile_payload_size, udp_payload_target);
    if (packet_count == 0 || packet_count > UINT8_MAX || tile_payload_size > UINT16_MAX)
    {
        if (delivery)
        {
            wd_stream_finish_udp_tile_delivery(delivery, true);
        }
        return false;
    }
    if (delivery && input_sequence != 0)
    {
        delivery->input_sequence = input_sequence;
        delivery->input_inject_ns = net->last_input_inject_ns;
        net->input_correlation_inflight_sequence = input_sequence;
    }
    const uint64_t udp_send_start_ns = wd_now_ns();
    uint16_t packets_sent = 0;
    bool fatal_send_failure = false;

    for (uint16_t packet_id = 0; packet_id < packet_count; ++packet_id)
    {
        uint32_t offset = (uint32_t)packet_id * udp_payload_target;

        uint16_t payload_size =
            (uint16_t)(((tile_payload_size - offset) > udp_payload_target) ? udp_payload_target : (tile_payload_size - offset));

        uint8_t header_buf[WD_UDP_TILE_HEADER_MAX_SIZE];
        memset(header_buf, 0, sizeof(header_buf));

        uint8_t tile_size = 0;
        if (!wd_tile_size_code_for_dimensions(tile_width, tile_height, &tile_size))
        {
            WD_LOG_ERROR("refusing to send unsupported tile geometry %ux%u", tile_width, tile_height);
            fatal_send_failure = true;
            break;
        }

        struct wd_udp_tile_packet_decoded header;
        memset(&header, 0, sizeof(header));
        header.session_id = net->session_id;
        header.connection_token = net->connection_token;
        header.content_epoch = net->content_epoch;
        header.flags = compressed_payload ? WD_UDP_TILE_FLAG_COMPRESSED : 0;
        if (packet_id == 0 && input_sequence != 0)
        {
            header.flags |= WD_UDP_TILE_FLAG_INPUT_SEQUENCE;
            header.input_sequence = input_sequence;
        }
        header.tile_size = tile_size;
        header.tile_pkt_id = (uint8_t)packet_id;
        header.tile_id = tile_id;
        header.tile_pkt_count = (uint8_t)packet_count;
        header.payload_size = payload_size;
        header.tile_payload_size = (uint16_t)tile_payload_size;
        header.tile_generation = generation;
        header.header_size = wd_udp_tile_header_size_for_flags(header.flags);
        if (!wd_udp_tile_packet_encode_header(header_buf, sizeof(header_buf), &header))
        {
            fatal_send_failure = true;
            break;
        }
        const uint16_t header_size = header.header_size;

        uint32_t packet_wire_size = (uint32_t)header_size + (uint32_t)payload_size;
        if (delivery)
        {
            wd_tile_delivery_status_add(&delivery->status);
        }
        const enum wd_async_udp_send_status async_status =
            wd_async_udp_send_packet(net->udp_tx, net->udp_fd, &net->client_udp_addr, header_buf,
                                     header_size, (uint8_t*)tile_payload + offset, payload_size,
                                     delivery ? wd_stream_udp_tile_packet_completion : NULL, delivery);
        const bool async_queued = async_status == WD_ASYNC_UDP_SEND_QUEUED;
        if (!async_queued && delivery)
        {
            bool ignored_failed = false;
            (void)wd_tile_delivery_status_complete(&delivery->status,
                                                    async_status == WD_ASYNC_UDP_SEND_FAILED,
                                                    &ignored_failed);
        }
        if (async_status == WD_ASYNC_UDP_SEND_SATURATED)
        {
            /* Never bypass an older asynchronous backlog with a synchronous
             * send. Treat saturation as congestion and retry the whole tile
             * from the normal dirty queue after completions make progress. */
            wd_note_udp_send_pressure_locked(net, EAGAIN);
            if (result)
            {
                result->send_blocked = true;
            }
            break;
        }
        if (!async_queued)
        {
            if (net->udp_tx)
            {
                net->stats.udp_async_send_failed++;
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

            const size_t expected_wire_size = (size_t)header_size + (size_t)payload_size;
            ssize_t sent = sendmsg(net->udp_fd, &msg, 0);

            if (sent < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
                {
                    wd_note_udp_send_pressure_locked(net, errno);
                    if (result)
                    {
                        result->send_blocked = true;
                    }
                    break;
                }

                WD_LOG_ERROR("sendto failed: %s", strerror(errno));
                fatal_send_failure = true;
                break;
            }

            if ((size_t)sent != expected_wire_size)
            {
                WD_LOG_ERROR("short UDP datagram send: sent=%zd expected=%zu", sent, expected_wire_size);
                fatal_send_failure = true;
                break;
            }

            packet_wire_size = (uint32_t)sent;
        }

        packets_sent++;
        if (result)
        {
            result->any_packet_sent = true;
            result->packets_sent = packets_sent;
            result->bytes_sent += packet_wire_size;
        }

        net->stats.udp_packets_sent++;
        net->stats.udp_bytes_sent += (uint64_t)packet_wire_size;
    }

    const bool delivery_incomplete = fatal_send_failure || packets_sent != packet_count;
    wd_stream_seal_udp_tile_delivery(delivery, delivery_incomplete);
    if (!delivery && input_sequence != 0)
    {
        wd_stream_note_input_delivery_locked(net, input_sequence, net->last_input_inject_ns,
                                             wd_now_ns(), !delivery_incomplete);
    }

    if (fatal_send_failure)
    {
        net->stats.udp_send_ns += wd_now_ns() - udp_send_start_ns;
        return false;
    }

    if (result)
    {
        result->all_packets_sent = packets_sent == packet_count;
        if (result->any_packet_sent && !result->all_packets_sent)
        {
            net->stats.partial_tile_sends++;
            net->stats.partial_tile_packets_sent += result->packets_sent;
        }
    }

    if (!result || result->all_packets_sent)
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
        if (tile_width == server->tile_width && tile_height == server->tile_height)
        {
            wd_stream_mark_summary_dirty_locked(server, tile_id);
        }
    }

    net->stats.udp_send_ns += wd_now_ns() - udp_send_start_ns;
    return true;
}

static void wd_dirty_queue_note_cleared_locked(struct wd_net_state* net, uint16_t tile_id, uint16_t total_tiles) {
    if (!net || !net->dirty_queued || tile_id >= total_tiles || !net->dirty_queued[tile_id])
    {
        return;
    }

    net->dirty_queued[tile_id] = false;
    if (net->dirty_queue_count > 0)
    {
        net->dirty_queue_count--;
    }
    if (net->dirty_queue_enqueued_ns)
    {
        net->dirty_queue_enqueued_ns[tile_id] = 0;
    }
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

static bool wd_dirty_queue_push_locked(struct wd_net_state* net, uint16_t tile_id, uint16_t total_tiles) {
    if (!net || tile_id >= total_tiles || !net->dirty_queued)
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

    /* Fresh content supersedes any queued repair for the same base tile. */
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

    net->dirty_queued[tile_id] = true;
    net->dirty_queue_count++;
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

    if (wd_stream_mode_video_owns_display(net->stream_policy.stream_mode))
    {
        net->stats.retx_req_ignored_live++;
        return false;
    }

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
    return true;
}

static void wd_clear_damage_tiles(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    if (server->damage_tiles && server->damage_tile_count > 0)
    {
        memset(server->damage_tiles, 0, (size_t)server->total_base_tiles * sizeof(*server->damage_tiles));
    }

    server->damage_all_tiles  = false;
    server->damage_tile_count = 0;
}

/* Consume WayDisplay's compositor-side damage metadata without constructing
 * dirty tile queues. Active video frames already carry the complete display,
 * so tile generations, regions, retransmits, and summary state are redundant.
 * The returned count is used only by adaptive video-mode exit heuristics. */
static uint16_t wd_stream_take_video_damage_sample_locked(struct wd_server* server) {
    if (!server)
    {
        return 0;
    }

    uint32_t dirty_tiles = 0;
    if (server->damage_all_tiles || !server->damage_tiles)
    {
        dirty_tiles = server->total_tiles;
    }
    else
    {
        dirty_tiles = server->damage_tile_count;
        if (dirty_tiles > server->total_tiles)
        {
            dirty_tiles = server->total_tiles;
        }
    }

    wd_clear_damage_tiles(server);
    return (uint16_t)dirty_tiles;
}

static bool wd_stream_has_queued_tile_work_locked(const struct wd_server* server) {
    if (!server)
    {
        return false;
    }

    const struct wd_net_state* net = &server->net;
    return net->dirty_queue_count != 0 || net->dirty_region_count != 0 ||
           net->retransmit_queue_count != 0 || net->summary_dirty_count != 0;
}

static void wd_stream_collapse_tile_queues_for_video_locked(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    if (net->dirty_queued)
    {
        memset(net->dirty_queued, 0, (size_t)server->total_tiles * sizeof(*net->dirty_queued));
    }
    if (net->dirty_queue_enqueued_ns)
    {
        memset(net->dirty_queue_enqueued_ns, 0, (size_t)server->total_tiles * sizeof(*net->dirty_queue_enqueued_ns));
    }
    if (net->dirty_region_queued)
    {
        memset(net->dirty_region_queued, 0, (size_t)server->total_tiles * sizeof(*net->dirty_region_queued));
    }
    net->dirty_queue_read = 0;
    net->dirty_queue_write = 0;
    net->dirty_queue_count = 0;
    wd_dirty_region_scheduler_reset(net->dirty_region_scheduler);
    net->dirty_region_count = 0;

    if (net->retransmit_queued)
    {
        memset(net->retransmit_queued, 0, (size_t)server->total_tiles * sizeof(*net->retransmit_queued));
    }
    if (net->retransmit_requested_generation)
    {
        memset(net->retransmit_requested_generation, 0, (size_t)server->total_tiles * sizeof(*net->retransmit_requested_generation));
    }
    if (net->retransmit_queue_enqueued_ns)
    {
        memset(net->retransmit_queue_enqueued_ns, 0, (size_t)server->total_tiles * sizeof(*net->retransmit_queue_enqueued_ns));
    }
    net->retransmit_queue_count = 0;

    if (net->summary_dirty_tiles)
    {
        memset(net->summary_dirty_tiles, 0, (size_t)server->total_tiles * sizeof(*net->summary_dirty_tiles));
    }
    net->summary_dirty_count = 0;
}

static void wd_stream_mark_dirty_top_region_locked(struct wd_server* server, uint16_t base_tile_id);
static void wd_stream_maybe_clear_dirty_top_region_for_base_locked(struct wd_server* server, uint16_t base_tile_id);

static void wd_detect_one_dirty_tile_into_queue_locked(struct wd_server* server, uint16_t tile_id) {
    if (!server || tile_id >= server->total_tiles)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    if (net->dirty_epochs)
    {
        net->dirty_epochs[tile_id]++;
        if (net->dirty_epochs[tile_id] == 0)
        {
            net->dirty_epochs[tile_id] = 1;
        }
    }
    if (wd_dirty_queue_push_locked(net, tile_id, server->total_tiles))
    {
        wd_stream_mark_dirty_top_region_locked(server, tile_id);
    }
    else if (net->dirty_queued && net->dirty_queued[tile_id])
    {
        wd_stream_mark_dirty_top_region_locked(server, tile_id);
    }
}

static bool wd_framebuffer_tile_changed_and_update_shadow_locked(struct wd_server* server, uint16_t tile_id) {
    if (!server || !server->framebuffer_xrgb8888 || !server->framebuffer_shadow_xrgb8888 ||
        tile_id >= server->total_base_tiles || server->base_tiles_x == 0)
    {
        return true;
    }

    const uint32_t tile_x = (uint32_t)(tile_id % server->base_tiles_x) * server->base_tile_width;
    const uint32_t tile_y = (uint32_t)(tile_id / server->base_tiles_x) * server->base_tile_height;
    if (tile_x >= server->display_width || tile_y >= server->display_height)
    {
        return false;
    }

    uint32_t width = server->base_tile_width;
    uint32_t height = server->base_tile_height;
    if (tile_x + width > server->display_width)
    {
        width = server->display_width - tile_x;
    }
    if (tile_y + height > server->display_height)
    {
        height = server->display_height - tile_y;
    }

    bool changed = !server->framebuffer_shadow_valid;
    if (!changed)
    {
        for (uint32_t row = 0; row < height; ++row)
        {
            const uint32_t* current = server->framebuffer_xrgb8888 +
                                      (size_t)(tile_y + row) * server->display_width + tile_x;
            const uint32_t* shadow = server->framebuffer_shadow_xrgb8888 +
                                     (size_t)(tile_y + row) * server->display_width + tile_x;
            if (memcmp(current, shadow, (size_t)width * sizeof(*current)) != 0)
            {
                changed = true;
                break;
            }
        }
    }

    if (changed)
    {
        for (uint32_t row = 0; row < height; ++row)
        {
            const uint32_t* current = server->framebuffer_xrgb8888 +
                                      (size_t)(tile_y + row) * server->display_width + tile_x;
            uint32_t* shadow = server->framebuffer_shadow_xrgb8888 +
                               (size_t)(tile_y + row) * server->display_width + tile_x;
            memcpy(shadow, current, (size_t)width * sizeof(*current));
        }
    }

    return changed;
}

static uint16_t wd_detect_dirty_tiles_into_queue_locked(struct wd_server* server) {
    if (!server || !server->net.tiles)
    {
        return 0;
    }

    struct wd_net_state* net = &server->net;
    const uint64_t diff_start_ns = wd_now_ns();
    const uint32_t limit = server->total_base_tiles < server->total_tiles ?
                               server->total_base_tiles : server->total_tiles;
    const bool full_candidate_pass = !server->framebuffer_shadow_valid ||
                                     server->damage_all_tiles || !server->damage_tiles;
    uint16_t dirty_tiles = 0;

    if (!server->framebuffer_shadow_valid)
    {
        net->stats.framebuffer_diff_full_refreshes++;
    }

    for (uint32_t tile_id = 0; tile_id < limit; ++tile_id)
    {
        if (!full_candidate_pass && !server->damage_tiles[tile_id])
        {
            continue;
        }

        net->stats.framebuffer_diff_candidates++;
        if (wd_framebuffer_tile_changed_and_update_shadow_locked(server, (uint16_t)tile_id))
        {
            net->stats.framebuffer_diff_changed++;
            wd_detect_one_dirty_tile_into_queue_locked(server, (uint16_t)tile_id);
            dirty_tiles++;
        }
        else
        {
            net->stats.framebuffer_diff_unchanged++;
        }
    }

    if (server->framebuffer_shadow_xrgb8888)
    {
        server->framebuffer_shadow_valid = true;
    }

    net->stats.framebuffer_diff_ns += wd_now_ns() - diff_start_ns;
    wd_clear_damage_tiles(server);
    return dirty_tiles;
}

static void wd_stream_note_mode_frame_locked(struct wd_net_state* net, uint16_t dirty_tiles,
                                             uint16_t pending_tiles, uint32_t total_tiles,
                                             bool budget_pressure, bool full_refresh) {
    if (!net)
    {
        return;
    }

    const uint64_t dirty_coverage = wd_stream_coverage_per_mille(dirty_tiles, total_tiles);
    const uint64_t pending_coverage = wd_stream_coverage_per_mille(pending_tiles, total_tiles);

    net->stats.stream_mode_frame_samples++;
    if (dirty_tiles != 0)
    {
        net->stats.stream_mode_changed_frame_samples++;
    }
    if (full_refresh)
    {
        net->stats.stream_mode_full_refresh_samples++;
    }
    net->stats.stream_mode_dirty_coverage_per_mille_sum += dirty_coverage;
    if (dirty_coverage > net->stats.stream_mode_dirty_coverage_per_mille_peak)
    {
        net->stats.stream_mode_dirty_coverage_per_mille_peak = dirty_coverage;
    }

    net->stats.stream_mode_pending_coverage_per_mille_sum += pending_coverage;
    if (pending_coverage > net->stats.stream_mode_pending_coverage_per_mille_peak)
    {
        net->stats.stream_mode_pending_coverage_per_mille_peak = pending_coverage;
    }

    if (budget_pressure)
    {
        net->stats.stream_mode_budget_pressure_frames++;
        if (full_refresh)
        {
            net->stats.stream_mode_full_refresh_budget_pressure_frames++;
        }
    }
}


struct wd_wire_tile_candidate {
    uint16_t width;
    uint16_t height;
    uint16_t tile_id;
    uint16_t covered_base_ids[64];
    uint16_t covered_base_count;
    uint32_t uncompressed_size;
    uint32_t compressed_size;
    uint32_t wire_size;
    bool compressed_payload;
};


struct wd_parallel_encode_result {
    bool valid;
    bool budget_blocked;
    uint16_t top_region_id;
    struct wd_wire_tile_candidate candidate;
    uint64_t covered_dirty_epochs[64];
    uint8_t* payload;
    uint32_t payload_size;
    uint64_t worker_encode_ns;
    uint64_t framebuffer_generation;
};

struct wd_parallel_encode_job {
    struct wd_server* server;
    const uint32_t* framebuffer_xrgb8888;
    uint32_t display_width;
    uint32_t display_height;
    uint16_t tile_width;
    uint16_t tile_height;
    uint16_t tiles_x;
    uint16_t tiles_y;
    uint16_t total_tiles;
    uint16_t top_region_id;
    uint64_t input_sequence;
    uint64_t remaining_byte_budget;
    uint64_t framebuffer_generation;
    uint16_t udp_payload_target;
    uint8_t compression_benchmark_mode;
    bool network_happy;
    const bool* dirty_snapshot;
    const uint64_t* dirty_epoch_snapshot;
    uint64_t compression_attempts;
    uint64_t compression_wins;
    uint64_t compression_entropy_skips;
    uint64_t compression_adaptive_skips;
    uint64_t compression_nonwins;
    uint64_t compression_forced_choices;
    uint64_t compression_ns;
    uint64_t compression_saved_wire_bytes;
    struct wd_parallel_encode_result* result;
    uint16_t result_capacity;
    uint16_t result_count;
};

struct wd_encoder_worker_state {
    struct wd_encoder_pool* pool;
    uint16_t worker_index;
    pthread_t thread;
    uint8_t* tile_bytes;
    uint8_t* compressed_tile;
    size_t compressed_capacity;
    struct wd_zstd_compressor* compressor;
    struct wd_tile_compression_advisor compression_advisors[4];
};

struct wd_parallel_encode_batch {
    struct wd_parallel_encode_job* jobs;
    uint16_t job_count;
    uint16_t next_job;
    uint16_t completed_jobs;
    uint64_t worker_encode_ns;
    bool active;
};

struct wd_encoder_pool {
    pthread_mutex_t lock;
    pthread_cond_t work_cond;
    pthread_cond_t done_cond;
    bool running;
    uint16_t thread_count;
    struct wd_parallel_encode_batch* batch;
    struct wd_encoder_worker_state workers[WD_ENCODER_MAX_THREADS];
};

struct wd_encode_workspace {
    uint16_t tile_capacity;
    uint16_t batch_capacity;
    bool* tile_snapshot;
    uint64_t* epoch_snapshot;
    uint16_t* regions;
    struct wd_parallel_encode_job* jobs;
    struct wd_parallel_encode_result* results;
};

static void wd_stream_encode_workspace_free(struct wd_encode_workspace* workspace) {
    if (!workspace)
    {
        return;
    }
    free(workspace->tile_snapshot);
    free(workspace->epoch_snapshot);
    free(workspace->regions);
    free(workspace->jobs);
    free(workspace->results);
    free(workspace);
}

static void wd_stream_encode_workspace_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }
    wd_stream_encode_workspace_free(server->net.encode_workspace);
    server->net.encode_workspace = NULL;
}

static struct wd_encode_workspace* wd_stream_encode_workspace_ensure(struct wd_server* server,
                                                                     uint16_t tile_capacity,
                                                                     uint16_t batch_capacity) {
    if (!server || tile_capacity == 0 || batch_capacity == 0)
    {
        return NULL;
    }

    struct wd_encode_workspace* workspace = server->net.encode_workspace;
    if (!workspace || workspace->tile_capacity < tile_capacity || workspace->batch_capacity < batch_capacity)
    {
        const uint16_t next_tile_capacity = workspace && workspace->tile_capacity > tile_capacity
                                                ? workspace->tile_capacity
                                                : tile_capacity;
        const uint16_t next_batch_capacity = workspace && workspace->batch_capacity > batch_capacity
                                                 ? workspace->batch_capacity
                                                 : batch_capacity;
        struct wd_encode_workspace* next = calloc(1, sizeof(*next));
        if (!next)
        {
            return NULL;
        }
        next->tile_capacity = next_tile_capacity;
        next->batch_capacity = next_batch_capacity;
        next->tile_snapshot = calloc(next_tile_capacity, sizeof(*next->tile_snapshot));
        next->epoch_snapshot = calloc(next_tile_capacity, sizeof(*next->epoch_snapshot));
        next->regions = calloc(next_tile_capacity, sizeof(*next->regions));
        next->jobs = calloc(next_batch_capacity, sizeof(*next->jobs));
        next->results = calloc((size_t)next_batch_capacity * WD_ENCODER_MAX_RESULTS_PER_JOB,
                               sizeof(*next->results));
        if (!next->tile_snapshot || !next->epoch_snapshot || !next->regions || !next->jobs || !next->results)
        {
            wd_stream_encode_workspace_free(next);
            return NULL;
        }
        wd_stream_encode_workspace_free(workspace);
        server->net.encode_workspace = next;
        workspace = next;
    }

    memset(workspace->jobs, 0, (size_t)batch_capacity * sizeof(*workspace->jobs));
    memset(workspace->results, 0,
           (size_t)batch_capacity * WD_ENCODER_MAX_RESULTS_PER_JOB * sizeof(*workspace->results));
    return workspace;
}

static void* wd_stream_encoder_worker_main(void* data);
static uint16_t wd_stream_encoder_thread_count(void);

/* Keep each synchronous encode batch to one worker wave. The caller sends the
 * completed results only after the whole batch returns, so queuing hundreds of
 * regions here creates head-of-line latency before the first UDP tile can be
 * transmitted. */
static uint16_t wd_stream_low_latency_batch_capacity(const struct wd_server* server, uint16_t available_jobs) {
    if (available_jobs == 0)
    {
        return 0;
    }

    uint16_t workers = wd_stream_encoder_thread_count();
    if (server && server->net.encoder_pool)
    {
        const struct wd_encoder_pool* pool = server->net.encoder_pool;
        if (pool->thread_count != 0)
        {
            workers = pool->thread_count;
        }
    }
    if (workers == 0)
    {
        workers = 1;
    }
    return available_jobs < workers ? available_jobs : workers;
}

static uint16_t wd_stream_encoder_thread_count(void) {
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count < 2)
    {
        return 1;
    }
    if (cpu_count > (long)WD_ENCODER_MAX_THREADS + 1)
    {
        return WD_ENCODER_MAX_THREADS;
    }
    return (uint16_t)(cpu_count - 1);
}


static void wd_stream_encoder_pool_destroy(struct wd_server* server) {
    if (!server || !server->net.encoder_pool)
    {
        return;
    }

    struct wd_encoder_pool* pool = server->net.encoder_pool;
    pthread_mutex_lock(&pool->lock);
    pool->running = false;
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->lock);

    for (uint16_t i = 0; i < pool->thread_count; ++i)
    {
        pthread_join(pool->workers[i].thread, NULL);
        free(pool->workers[i].tile_bytes);
        pool->workers[i].tile_bytes = NULL;
        free(pool->workers[i].compressed_tile);
        pool->workers[i].compressed_tile = NULL;
        wd_zstd_compressor_destroy(pool->workers[i].compressor);
        pool->workers[i].compressor = NULL;
    }

    pthread_cond_destroy(&pool->done_cond);
    pthread_cond_destroy(&pool->work_cond);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
    server->net.encoder_pool = NULL;
}

static bool wd_stream_encoder_pool_ensure(struct wd_server* server) {
    if (!server)
    {
        return false;
    }
    if (server->net.encoder_pool)
    {
        return true;
    }

    struct wd_encoder_pool* pool = calloc(1, sizeof(*pool));
    if (!pool)
    {
        return false;
    }
    if (pthread_mutex_init(&pool->lock, NULL) != 0)
    {
        free(pool);
        return false;
    }
    if (pthread_cond_init(&pool->work_cond, NULL) != 0)
    {
        pthread_mutex_destroy(&pool->lock);
        free(pool);
        return false;
    }
    if (pthread_cond_init(&pool->done_cond, NULL) != 0)
    {
        pthread_cond_destroy(&pool->work_cond);
        pthread_mutex_destroy(&pool->lock);
        free(pool);
        return false;
    }

    const uint32_t max_wire_tile_bytes = WD_WIRE_TILE_MAX_WIDTH * WD_WIRE_TILE_MAX_HEIGHT * WD_BYTES_PER_PIXEL;
    const size_t compressed_capacity = wd_zstd_compress_bound(max_wire_tile_bytes);
    pool->thread_count = wd_stream_encoder_thread_count();
    if (pool->thread_count == 0)
    {
        pool->thread_count = 1;
    }
    pool->running = true;
    server->net.encoder_pool = pool;

    for (uint16_t i = 0; i < pool->thread_count; ++i)
    {
        pool->workers[i].pool = pool;
        pool->workers[i].worker_index = i;
        pool->workers[i].compressed_capacity = compressed_capacity;
        pool->workers[i].tile_bytes = malloc(max_wire_tile_bytes);
        pool->workers[i].compressed_tile = malloc(compressed_capacity);
        pool->workers[i].compressor = wd_zstd_compressor_create();
        if (!pool->workers[i].tile_bytes || !pool->workers[i].compressed_tile || !pool->workers[i].compressor ||
            pthread_create(&pool->workers[i].thread, NULL, wd_stream_encoder_worker_main, &pool->workers[i]) != 0)
        {
            free(pool->workers[i].tile_bytes);
            pool->workers[i].tile_bytes = NULL;
            free(pool->workers[i].compressed_tile);
            pool->workers[i].compressed_tile = NULL;
            wd_zstd_compressor_destroy(pool->workers[i].compressor);
            pool->workers[i].compressor = NULL;
            pool->thread_count = i;
            wd_stream_encoder_pool_destroy(server);
            return false;
        }
    }

    return true;
}

static bool wd_stream_encoder_pool_run(struct wd_server* server, struct wd_parallel_encode_batch* batch,
                                       uint16_t* out_worker_threads) {
    if (out_worker_threads)
    {
        *out_worker_threads = 0;
    }
    if (!server || !batch || batch->job_count == 0 || !wd_stream_encoder_pool_ensure(server))
    {
        return false;
    }

    struct wd_encoder_pool* pool = server->net.encoder_pool;
    pthread_mutex_lock(&pool->lock);
    batch->next_job = 0;
    batch->completed_jobs = 0;
    batch->worker_encode_ns = 0;
    batch->active = true;
    pool->batch = batch;
    pthread_cond_broadcast(&pool->work_cond);
    while (batch->active)
    {
        pthread_cond_wait(&pool->done_cond, &pool->lock);
    }
    pool->batch = NULL;
    if (out_worker_threads)
    {
        *out_worker_threads = pool->thread_count;
    }
    pthread_mutex_unlock(&pool->lock);
    return true;
}

void wd_stream_wait_for_encoder_idle_locked(struct wd_server* server) {
    if (!server)
    {
        return;
    }
    while (server->net.encoder_batch_active)
    {
        pthread_cond_wait(&server->net.encoder_idle_cond, &server->net.lock);
    }
}

static bool wd_stream_collect_wire_tile_base_ids(const struct wd_server* server, uint16_t tile_id, uint16_t tile_width,
                                                 uint16_t tile_height, uint16_t* out_ids, uint16_t* out_count,
                                                 uint16_t max_count) {
    if (!server || !out_ids || !out_count || tile_width == 0 || tile_height == 0 || server->tile_width == 0 || server->tile_height == 0)
    {
        return false;
    }

    *out_count = 0;
    const uint16_t tiles_x = wd_tiles_for_width_with_tile(server->display_width, tile_width);
    if (tiles_x == 0)
    {
        return false;
    }

    const uint32_t x = wd_tile_start_x_for_tile(tile_id, tiles_x, tile_width);
    const uint32_t y = wd_tile_start_y_for_tile(tile_id, tiles_x, tile_height);
    const uint32_t w = wd_tile_visible_width_for_tile(server->display_width, tile_id, tiles_x, tile_width);
    const uint32_t h = wd_tile_visible_height_for_tile(server->display_height, tile_id, tiles_x, tile_height);
    if (w == 0 || h == 0)
    {
        return false;
    }

    uint32_t bx0 = x / server->tile_width;
    uint32_t by0 = y / server->tile_height;
    uint32_t bx1 = (x + w - 1u) / server->tile_width;
    uint32_t by1 = (y + h - 1u) / server->tile_height;
    if (bx1 >= server->tiles_x)
    {
        bx1 = (uint32_t)server->tiles_x - 1u;
    }
    if (by1 >= server->tiles_y)
    {
        by1 = (uint32_t)server->tiles_y - 1u;
    }

    for (uint32_t by = by0; by <= by1; ++by)
    {
        for (uint32_t bx = bx0; bx <= bx1; ++bx)
        {
            uint32_t base_id = by * (uint32_t)server->tiles_x + bx;
            if (base_id >= server->total_tiles || *out_count >= max_count)
            {
                return false;
            }
            out_ids[(*out_count)++] = (uint16_t)base_id;
        }
    }
    return *out_count != 0;
}

static bool wd_stream_wire_tile_for_pixel(const struct wd_server* server, uint32_t x, uint32_t y, uint16_t tile_width,
                                          uint16_t tile_height, uint16_t* out_tile_id) {
    if (!server || !out_tile_id || tile_width == 0 || tile_height == 0 || x >= server->display_width || y >= server->display_height)
    {
        return false;
    }

    const uint16_t tiles_x = wd_tiles_for_width_with_tile(server->display_width, tile_width);
    const uint16_t tiles_y = wd_tiles_for_height_with_tile(server->display_height, tile_height);
    if (tiles_x == 0 || tiles_y == 0)
    {
        return false;
    }

    const uint32_t tx = x / tile_width;
    const uint32_t ty = y / tile_height;
    const uint32_t id = ty * (uint32_t)tiles_x + tx;
    if (tx >= tiles_x || ty >= tiles_y || id > UINT16_MAX)
    {
        return false;
    }

    *out_tile_id = (uint16_t)id;
    return true;
}


static bool wd_stream_top_region_for_base_tile(const struct wd_server* server, uint16_t base_tile_id, uint16_t* out_region_id) {
    if (!server || !out_region_id || base_tile_id >= server->total_tiles || server->tiles_x == 0)
    {
        return false;
    }

    const uint32_t bx = (uint32_t)base_tile_id % server->tiles_x;
    const uint32_t by = (uint32_t)base_tile_id / server->tiles_x;
    const uint32_t x = bx * server->tile_width;
    const uint32_t y = by * server->tile_height;
    return wd_stream_wire_tile_for_pixel(server, x, y, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT, out_region_id);
}

static struct wd_dirty_region_scheduler* wd_stream_dirty_region_scheduler_locked(struct wd_server* server) {
    if (!server)
    {
        return NULL;
    }
    if (!server->net.dirty_region_scheduler)
    {
        const uint16_t regions_x = wd_tiles_for_width_with_tile(server->display_width, WD_WIRE_TILE_MAX_WIDTH);
        const uint16_t regions_total = wd_total_tiles_for_size_with_tile(server->display_width, server->display_height,
                                                                         WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT);
        server->net.dirty_region_scheduler =
            wd_dirty_region_scheduler_create(regions_total, regions_x, WD_DIRTY_REGION_STARVATION_NS);
    }
    return server->net.dirty_region_scheduler;
}

static void wd_stream_sync_dirty_region_count_locked(struct wd_server* server) {
    if (server)
    {
        server->net.dirty_region_count =
            wd_dirty_region_scheduler_count(server->net.dirty_region_scheduler);
    }
}

static void wd_stream_mark_dirty_top_region_locked(struct wd_server* server, uint16_t base_tile_id) {
    if (!server)
    {
        return;
    }

    uint16_t region_id = 0;
    if (!wd_stream_top_region_for_base_tile(server, base_tile_id, &region_id))
    {
        return;
    }

    struct wd_dirty_region_scheduler* scheduler = wd_stream_dirty_region_scheduler_locked(server);
    if (!scheduler)
    {
        return;
    }
    (void)wd_dirty_region_scheduler_enqueue(scheduler, region_id, wd_now_ns());
    wd_stream_sync_dirty_region_count_locked(server);
}

static void wd_stream_remove_dirty_top_region_locked(struct wd_server* server, uint16_t region_id) {
    if (!server || !server->net.dirty_region_scheduler)
    {
        return;
    }
    wd_dirty_region_scheduler_forget(server->net.dirty_region_scheduler, region_id);
    wd_stream_sync_dirty_region_count_locked(server);
}

static bool wd_stream_top_region_still_dirty_locked(struct wd_server* server, uint16_t region_id) {
    if (!server || !server->net.dirty_queued)
    {
        return false;
    }

    uint16_t ids[64];
    uint16_t count = 0;
    if (!wd_stream_collect_wire_tile_base_ids(server, region_id, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT, ids, &count,
                                             (uint16_t)(sizeof(ids) / sizeof(ids[0]))))
    {
        return false;
    }
    for (uint16_t i = 0; i < count; ++i)
    {
        if (ids[i] < server->total_tiles && server->net.dirty_queued[ids[i]])
        {
            return true;
        }
    }
    return false;
}

static void wd_stream_maybe_clear_dirty_top_region_for_base_locked(struct wd_server* server, uint16_t base_tile_id) {
    uint16_t region_id = 0;
    if (!wd_stream_top_region_for_base_tile(server, base_tile_id, &region_id))
    {
        return;
    }
    if (!wd_stream_top_region_still_dirty_locked(server, region_id))
    {
        wd_stream_remove_dirty_top_region_locked(server, region_id);
    }
}

static bool wd_stream_candidate_allowed_for_region_locked(struct wd_server* server, const struct wd_wire_tile_candidate* candidate,
                                                          uint64_t remaining_byte_budget, bool network_happy,
                                                          bool prefer_one_packet) {
    if (!server || !candidate || candidate->wire_size == 0 || candidate->wire_size > remaining_byte_budget)
    {
        return false;
    }

    const uint32_t one_packet_budget = (uint32_t)server->net.udp_payload_target + WD_UDP_TILE_HEADER_MAX_SIZE;
    const bool is_max_tile = candidate->width == WD_WIRE_TILE_MAX_WIDTH && candidate->height == WD_WIRE_TILE_MAX_HEIGHT;
    if (candidate->width == server->tile_width && candidate->height == server->tile_height)
    {
        return true;
    }
    if (prefer_one_packet)
    {
        return candidate->wire_size <= one_packet_budget;
    }
    if (is_max_tile && network_happy)
    {
        return true;
    }
    return candidate->wire_size <= one_packet_budget;
}

static bool wd_stream_candidate_allowed_for_job(const struct wd_parallel_encode_job* job,
                                                const struct wd_wire_tile_candidate* candidate) {
    if (!job || !candidate || candidate->wire_size == 0 || candidate->wire_size > job->remaining_byte_budget)
    {
        return false;
    }

    const uint32_t one_packet_budget = (uint32_t)job->udp_payload_target + WD_UDP_TILE_HEADER_MAX_SIZE;
    const bool is_max_tile = candidate->width == WD_WIRE_TILE_MAX_WIDTH && candidate->height == WD_WIRE_TILE_MAX_HEIGHT;
    if (candidate->width == job->tile_width && candidate->height == job->tile_height)
    {
        return true;
    }
    /* The first damage after input is latency-sensitive. Prefer a tile that
     * fits in one UDP datagram so the client can present it without waiting for
     * multi-packet reassembly or repair. */
    if (job->input_sequence != 0)
    {
        return candidate->wire_size <= one_packet_budget;
    }
    if (is_max_tile && job->network_happy)
    {
        return true;
    }
    return candidate->wire_size <= one_packet_budget;
}

static bool wd_stream_job_collect_wire_tile_base_ids(const struct wd_parallel_encode_job* job, uint16_t tile_id,
                                                        uint16_t tile_width, uint16_t tile_height, uint16_t* out_ids,
                                                        uint16_t* out_count, uint16_t max_count) {
    if (!job || !out_ids || !out_count || tile_width == 0 || tile_height == 0 || job->tile_width == 0 ||
        job->tile_height == 0)
    {
        return false;
    }

    *out_count = 0;
    const uint16_t tiles_x = wd_tiles_for_width_with_tile(job->display_width, tile_width);
    if (tiles_x == 0)
    {
        return false;
    }

    const uint32_t x = wd_tile_start_x_for_tile(tile_id, tiles_x, tile_width);
    const uint32_t y = wd_tile_start_y_for_tile(tile_id, tiles_x, tile_height);
    const uint32_t w = wd_tile_visible_width_for_tile(job->display_width, tile_id, tiles_x, tile_width);
    const uint32_t h = wd_tile_visible_height_for_tile(job->display_height, tile_id, tiles_x, tile_height);
    if (w == 0 || h == 0)
    {
        return false;
    }

    uint32_t bx0 = x / job->tile_width;
    uint32_t by0 = y / job->tile_height;
    uint32_t bx1 = (x + w - 1u) / job->tile_width;
    uint32_t by1 = (y + h - 1u) / job->tile_height;
    if (bx1 >= job->tiles_x)
    {
        bx1 = (uint32_t)job->tiles_x - 1u;
    }
    if (by1 >= job->tiles_y)
    {
        by1 = (uint32_t)job->tiles_y - 1u;
    }

    for (uint32_t by = by0; by <= by1; ++by)
    {
        for (uint32_t bx = bx0; bx <= bx1; ++bx)
        {
            uint32_t base_id = by * (uint32_t)job->tiles_x + bx;
            if (base_id >= job->total_tiles || *out_count >= max_count)
            {
                return false;
            }
            out_ids[(*out_count)++] = (uint16_t)base_id;
        }
    }
    return *out_count != 0;
}

static bool wd_stream_job_wire_tile_for_pixel(const struct wd_parallel_encode_job* job, uint32_t x, uint32_t y,
                                              uint16_t tile_width, uint16_t tile_height, uint16_t* out_tile_id) {
    if (!job || !out_tile_id || tile_width == 0 || tile_height == 0 || x >= job->display_width || y >= job->display_height)
    {
        return false;
    }

    const uint16_t tiles_x = wd_tiles_for_width_with_tile(job->display_width, tile_width);
    const uint16_t tiles_y = wd_tiles_for_height_with_tile(job->display_height, tile_height);
    if (tiles_x == 0 || tiles_y == 0)
    {
        return false;
    }

    const uint32_t tx = x / tile_width;
    const uint32_t ty = y / tile_height;
    const uint32_t id = ty * (uint32_t)tiles_x + tx;
    if (tx >= tiles_x || ty >= tiles_y || id > UINT16_MAX)
    {
        return false;
    }

    *out_tile_id = (uint16_t)id;
    return true;
}

static bool wd_stream_snapshot_region_has_dirty(const struct wd_parallel_encode_job* job, const bool* dirty_snapshot,
                                                uint16_t wire_tile_id, uint16_t tile_width, uint16_t tile_height,
                                                uint16_t* out_ids, uint16_t* out_count, uint16_t max_count) {
    if (!job || !dirty_snapshot || !out_ids || !out_count)
    {
        return false;
    }
    if (!wd_stream_job_collect_wire_tile_base_ids(job, wire_tile_id, tile_width, tile_height, out_ids, out_count, max_count))
    {
        return false;
    }
    for (uint16_t i = 0; i < *out_count; ++i)
    {
        const uint16_t base_id = out_ids[i];
        if (base_id < job->total_tiles && dirty_snapshot[base_id])
        {
            return true;
        }
    }
    return false;
}

static uint8_t wd_stream_compression_advisor_index(uint16_t tile_width, uint16_t tile_height) {
    if (tile_width == 128 && tile_height == 64)
    {
        return 0;
    }
    if (tile_width == 64 && tile_height == 64)
    {
        return 1;
    }
    if (tile_width == 32 && tile_height == 32)
    {
        return 2;
    }
    return 3;
}

static bool wd_stream_try_encode_candidate_for_snapshot(struct wd_parallel_encode_job* job,
                                                        struct wd_encoder_worker_state* worker,
                                                        uint16_t wire_tile_id, uint16_t tile_width,
                                                        uint16_t tile_height, bool allow_compression,
                                                        uint8_t* tile_bytes, uint8_t* compressed_tile,
                                                        size_t compressed_capacity, struct wd_wire_tile_candidate* out,
                                                        uint64_t* out_epochs) {
    if (!job || !worker || !job->server || !job->framebuffer_xrgb8888 || !tile_bytes || !compressed_tile || !out || !out_epochs)
    {
        return false;
    }

    uint16_t covered_count = 0;
    uint16_t covered_ids[64];
    if (!wd_stream_job_collect_wire_tile_base_ids(job, wire_tile_id, tile_width, tile_height, covered_ids, &covered_count,
                                             (uint16_t)(sizeof(covered_ids) / sizeof(covered_ids[0]))))
    {
        return false;
    }

    const uint32_t uncompressed_size = (uint32_t)tile_width * (uint32_t)tile_height * WD_BYTES_PER_PIXEL;
    const uint16_t wire_tiles_x = wd_tiles_for_width_with_tile(job->display_width, tile_width);
    const uint16_t wire_total_tiles = wd_total_tiles_for_size_with_tile(job->display_width, job->display_height, tile_width, tile_height);
    if (!wd_extract_tile_xrgb8888_for_tile(job->framebuffer_xrgb8888, job->display_width, job->display_height,
                                           wire_tiles_x, wire_total_tiles, wire_tile_id, tile_width, tile_height, tile_bytes))
    {
        return false;
    }

    uint32_t compressed_size = 0;
    bool compressed_payload = false;
    if (allow_compression)
    {
        const uint8_t benchmark_mode = job->compression_benchmark_mode;
        const bool entropy_ok = wd_tile_xrgb_payload_may_compress(tile_bytes, uncompressed_size);
        struct wd_tile_compression_advisor* advisor =
            &worker->compression_advisors[wd_stream_compression_advisor_index(tile_width, tile_height)];
        bool advisor_ok = true;
        if (benchmark_mode == WD_TILE_COMPRESSION_BENCH_AUTO && entropy_ok)
        {
            advisor_ok = wd_tile_compression_advisor_should_attempt(advisor);
        }

        if (benchmark_mode == WD_TILE_COMPRESSION_BENCH_AUTO && !entropy_ok)
        {
            job->compression_entropy_skips++;
        }
        else if (benchmark_mode == WD_TILE_COMPRESSION_BENCH_AUTO && !advisor_ok)
        {
            job->compression_adaptive_skips++;
        }
        else if (wd_tile_compression_benchmark_should_attempt(benchmark_mode, entropy_ok, advisor_ok))
        {
            job->compression_attempts++;
            const uint64_t compression_start_ns = wd_now_ns();
            const bool compressed = wd_zstd_compress_with_context(
                worker->compressor, tile_bytes, uncompressed_size, compressed_tile,
                compressed_capacity, WD_ZSTD_LEVEL, &compressed_size);
            job->compression_ns += wd_now_ns() - compression_start_ns;
            const bool worthwhile = compressed && wd_stream_use_compressed_tile_payload(
                compressed_size, uncompressed_size, job->udp_payload_target, job->input_sequence);
            compressed_payload = wd_tile_compression_benchmark_choose_compressed(
                benchmark_mode, compressed, worthwhile);

            if (benchmark_mode == WD_TILE_COMPRESSION_BENCH_AUTO)
            {
                wd_tile_compression_advisor_record(advisor, worthwhile);
            }
            if (worthwhile)
            {
                job->compression_wins++;
                const uint32_t compressed_wire = wd_stream_tile_wire_bytes_for_payload(
                    compressed_size, job->udp_payload_target, job->input_sequence, true);
                const uint32_t uncompressed_wire = wd_stream_tile_wire_bytes_for_payload(
                    uncompressed_size, job->udp_payload_target, job->input_sequence, false);
                if (uncompressed_wire > compressed_wire)
                {
                    job->compression_saved_wire_bytes += uncompressed_wire - compressed_wire;
                }
            }
            else
            {
                job->compression_nonwins++;
                if (compressed_payload)
                {
                    job->compression_forced_choices++;
                }
            }
        }
    }

    const uint32_t payload_size = compressed_payload ? compressed_size : uncompressed_size;
    memset(out, 0, sizeof(*out));
    out->width = tile_width;
    out->height = tile_height;
    out->tile_id = wire_tile_id;
    memcpy(out->covered_base_ids, covered_ids, (size_t)covered_count * sizeof(covered_ids[0]));
    out->covered_base_count = covered_count;
    out->uncompressed_size = uncompressed_size;
    out->compressed_size = compressed_size;
    out->wire_size = wd_stream_tile_wire_bytes_for_payload(payload_size, job->udp_payload_target,
                                                           job->input_sequence, compressed_payload);
    out->compressed_payload = compressed_payload;
    for (uint16_t i = 0; i < covered_count; ++i)
    {
        out_epochs[i] = job->dirty_epoch_snapshot ? job->dirty_epoch_snapshot[covered_ids[i]] : 0;
    }
    return true;
}

static bool wd_stream_append_snapshot_result(struct wd_parallel_encode_job* job,
                                             const struct wd_wire_tile_candidate* candidate,
                                             const uint64_t* covered_epochs,
                                             const uint8_t* tile_bytes,
                                             const uint8_t* compressed_tile) {
    if (!job || !candidate || !covered_epochs || !tile_bytes || !compressed_tile || !job->result ||
        job->result_count >= job->result_capacity)
    {
        return false;
    }

    const uint8_t* payload = candidate->compressed_payload ? compressed_tile : tile_bytes;
    const uint32_t payload_size = candidate->compressed_payload ? candidate->compressed_size : candidate->uncompressed_size;
    struct wd_parallel_encode_result* result = &job->result[job->result_count];
    memset(result, 0, sizeof(*result));
    result->top_region_id = job->top_region_id;
    result->framebuffer_generation = job->framebuffer_generation;
    result->payload = malloc(payload_size);
    if (!result->payload)
    {
        return false;
    }

    memcpy(result->payload, payload, payload_size);
    result->payload_size = payload_size;
    result->candidate = *candidate;
    memcpy(result->covered_dirty_epochs, covered_epochs,
           (size_t)candidate->covered_base_count * sizeof(covered_epochs[0]));
    result->valid = true;
    job->result_count++;
    return true;
}

static bool wd_stream_encode_region_recursive_snapshot(struct wd_parallel_encode_job* job,
                                                       struct wd_encoder_worker_state* worker,
                                                       uint16_t wire_tile_id, uint16_t tile_width,
                                                       uint16_t tile_height, uint8_t* tile_bytes,
                                                       uint8_t* compressed_tile, size_t compressed_capacity,
                                                       bool* out_budget_blocked) {
    if (out_budget_blocked)
    {
        *out_budget_blocked = false;
    }
    if (!job || !worker || !job->server || !tile_bytes || !compressed_tile)
    {
        return false;
    }
    if (job->result_count >= job->result_capacity)
    {
        return false;
    }

    uint16_t covered_count = 0;
    uint16_t covered_ids[64];
    if (!wd_stream_snapshot_region_has_dirty(job, job->dirty_snapshot, wire_tile_id, tile_width, tile_height,
                                             covered_ids, &covered_count, (uint16_t)(sizeof(covered_ids) / sizeof(covered_ids[0]))))
    {
        return false;
    }

    const bool is_base_tile = tile_width == job->tile_width && tile_height == job->tile_height;
    const bool allow_compression = true;
    struct wd_wire_tile_candidate candidate;
    uint64_t candidate_epochs[64] = {0};
    memset(&candidate, 0, sizeof(candidate));
    if (wd_stream_try_encode_candidate_for_snapshot(job, worker, wire_tile_id, tile_width, tile_height,
                                                    allow_compression, tile_bytes, compressed_tile,
                                                    compressed_capacity, &candidate, candidate_epochs) &&
        wd_stream_candidate_allowed_for_job(job, &candidate))
    {
        return wd_stream_append_snapshot_result(job, &candidate, candidate_epochs, tile_bytes, compressed_tile);
    }

    if (is_base_tile)
    {
        if (candidate.wire_size > job->remaining_byte_budget && out_budget_blocked)
        {
            *out_budget_blocked = true;
        }
        return false;
    }

    const uint16_t parent_tiles_x = wd_tiles_for_width_with_tile(job->display_width, tile_width);
    const uint32_t start_x = wd_tile_start_x_for_tile(wire_tile_id, parent_tiles_x, tile_width);
    const uint32_t start_y = wd_tile_start_y_for_tile(wire_tile_id, parent_tiles_x, tile_height);
    uint16_t child_width = 0;
    uint16_t child_height = 0;
    uint16_t child_count = 0;
    uint16_t child_ids[4];

    if (tile_width == WD_WIRE_TILE_MAX_WIDTH && tile_height == WD_WIRE_TILE_MAX_HEIGHT)
    {
        child_width = 64;
        child_height = 64;
        const uint32_t xs[2] = {start_x, start_x + child_width};
        for (uint16_t i = 0; i < 2; ++i)
        {
            if (xs[i] < job->display_width && start_y < job->display_height &&
                wd_stream_job_wire_tile_for_pixel(job, xs[i], start_y, child_width, child_height, &child_ids[child_count]))
            {
                child_count++;
            }
        }
    }
    else if (tile_width == 64 && tile_height == 64)
    {
        child_width = 32;
        child_height = 32;
        const uint32_t xs[2] = {start_x, start_x + child_width};
        const uint32_t ys[2] = {start_y, start_y + child_height};
        for (uint16_t y = 0; y < 2; ++y)
        {
            for (uint16_t x = 0; x < 2; ++x)
            {
                if (xs[x] < job->display_width && ys[y] < job->display_height &&
                    wd_stream_job_wire_tile_for_pixel(job, xs[x], ys[y], child_width, child_height, &child_ids[child_count]))
                {
                    child_count++;
                }
            }
        }
    }
    else if (tile_width == 32 && tile_height == 32)
    {
        child_width = job->tile_width;
        child_height = job->tile_height;
        const uint32_t xs[2] = {start_x, start_x + child_width};
        const uint32_t ys[2] = {start_y, start_y + child_height};
        for (uint16_t y = 0; y < 2; ++y)
        {
            for (uint16_t x = 0; x < 2; ++x)
            {
                if (xs[x] < job->display_width && ys[y] < job->display_height &&
                    wd_stream_job_wire_tile_for_pixel(job, xs[x], ys[y], child_width, child_height, &child_ids[child_count]))
                {
                    child_count++;
                }
            }
        }
    }
    else
    {
        return false;
    }

    bool any_encoded = false;
    bool any_budget_blocked = false;
    for (uint16_t i = 0; i < child_count; ++i)
    {
        bool child_budget_blocked = false;
        if (wd_stream_encode_region_recursive_snapshot(job, worker, child_ids[i], child_width, child_height,
                                                       tile_bytes, compressed_tile, compressed_capacity,
                                                       &child_budget_blocked))
        {
            any_encoded = true;
        }
        if (child_budget_blocked)
        {
            any_budget_blocked = true;
        }
        if (job->result_count >= job->result_capacity)
        {
            break;
        }
    }
    if (any_budget_blocked && out_budget_blocked)
    {
        *out_budget_blocked = true;
    }
    return any_encoded;
}

static void wd_stream_parallel_encode_one_job(struct wd_parallel_encode_job* job,
                                              struct wd_encoder_worker_state* worker) {
    if (!job || !worker || !job->result || !job->server || !worker->tile_bytes || !worker->compressed_tile)
    {
        return;
    }
    for (uint16_t i = 0; i < job->result_capacity; ++i)
    {
        memset(&job->result[i], 0, sizeof(job->result[i]));
        job->result[i].top_region_id = job->top_region_id;
        job->result[i].framebuffer_generation = job->framebuffer_generation;
    }
    job->result_count = 0;

    const uint64_t start_ns = wd_now_ns();
    bool budget_blocked = false;
    if (!wd_stream_encode_region_recursive_snapshot(job, worker, job->top_region_id,
                                                    WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT,
                                                    worker->tile_bytes, worker->compressed_tile,
                                                    worker->compressed_capacity, &budget_blocked))
    {
        job->result[0].budget_blocked = budget_blocked;
    }
    for (uint16_t i = 0; i < job->result_count; ++i)
    {
        job->result[i].worker_encode_ns = 0;
    }
    job->result[0].worker_encode_ns = wd_now_ns() - start_ns;
}

static void* wd_stream_encoder_worker_main(void* data) {
    struct wd_encoder_worker_state* worker = data;
    if (!worker || !worker->pool)
    {
        return NULL;
    }

    struct wd_encoder_pool* pool = worker->pool;
    for (;;)
    {
        pthread_mutex_lock(&pool->lock);
        while (pool->running && (!pool->batch || !pool->batch->active || pool->batch->next_job >= pool->batch->job_count))
        {
            pthread_cond_wait(&pool->work_cond, &pool->lock);
        }

        if (!pool->running)
        {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        struct wd_parallel_encode_batch* batch = pool->batch;
        uint16_t index = batch->next_job++;
        pthread_mutex_unlock(&pool->lock);

        wd_stream_parallel_encode_one_job(&batch->jobs[index], worker);

        pthread_mutex_lock(&pool->lock);
        batch->worker_encode_ns += batch->jobs[index].result ? batch->jobs[index].result->worker_encode_ns : 0;
        batch->completed_jobs++;
        if (batch->completed_jobs >= batch->job_count)
        {
            batch->active = false;
            pthread_cond_signal(&pool->done_cond);
        }
        pthread_mutex_unlock(&pool->lock);
    }
    return NULL;
}

static void wd_stream_compact_retransmit_queue_locked(struct wd_server* server) {
    if (!server || !server->net.retransmit_queue || !server->net.retransmit_queued)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    uint16_t out_count = 0;
    for (uint16_t i = 0; i < net->retransmit_queue_count; ++i)
    {
        const uint16_t tile_id = net->retransmit_queue[i];
        if (tile_id < server->total_tiles && net->retransmit_queued[tile_id])
        {
            net->retransmit_queue[out_count++] = tile_id;
        }
    }
    net->retransmit_queue_count = out_count;
}


static bool wd_stream_region_list_contains(const uint16_t* regions, uint16_t count, uint16_t region_id) {
    if (!regions)
    {
        return false;
    }
    for (uint16_t i = 0; i < count; ++i)
    {
        if (regions[i] == region_id)
        {
            return true;
        }
    }
    return false;
}

static bool wd_stream_update_base_tile_metadata_locked(struct wd_server* server, uint16_t base_tile_id, uint64_t generation,
                                                       uint64_t timestamp_ns, uint64_t input_sequence) {
    if (!server || base_tile_id >= server->total_tiles)
    {
        return false;
    }

    struct wd_tile_state* tile = &server->net.tiles[base_tile_id];
    tile->generation = generation;
    tile->timestamp_ns = timestamp_ns;
    tile->input_sequence = input_sequence;
    return true;
}


static void wd_stream_init_encode_job_locked(struct wd_parallel_encode_job* job, struct wd_server* server,
                                             uint16_t top_region_id, uint64_t input_sequence,
                                             uint64_t remaining_byte_budget, bool network_happy,
                                             const bool* dirty_snapshot, const uint64_t* epoch_snapshot,
                                             struct wd_parallel_encode_result* result, uint16_t result_capacity) {
    if (!job || !server)
    {
        return;
    }

    memset(job, 0, sizeof(*job));
    job->server = server;
    job->framebuffer_xrgb8888 = server->framebuffer_xrgb8888;
    job->display_width = server->display_width;
    job->display_height = server->display_height;
    job->tile_width = server->tile_width;
    job->tile_height = server->tile_height;
    job->tiles_x = server->tiles_x;
    job->tiles_y = server->tiles_y;
    job->total_tiles = server->total_tiles;
    job->top_region_id = top_region_id;
    job->input_sequence = input_sequence;
    job->remaining_byte_budget = remaining_byte_budget;
    job->framebuffer_generation = server->framebuffer_generation;
    job->udp_payload_target = server->net.udp_payload_target;
    job->compression_benchmark_mode = server->tile_compression_benchmark_mode;
    job->network_happy = network_happy;
    job->dirty_snapshot = dirty_snapshot;
    job->dirty_epoch_snapshot = epoch_snapshot;
    job->result = result;
    job->result_capacity = result_capacity;
}

static bool wd_stream_encode_result_stale_locked(const struct wd_server* server, const struct wd_parallel_encode_result* result) {
    if (!server || !result || result->framebuffer_generation != server->framebuffer_generation)
    {
        return true;
    }

    const struct wd_net_state* net = &server->net;
    for (uint16_t i = 0; i < result->candidate.covered_base_count; ++i)
    {
        const uint16_t covered_id = result->candidate.covered_base_ids[i];
        if (covered_id >= server->total_tiles)
        {
            return true;
        }
        if (net->dirty_epochs && net->dirty_epochs[covered_id] != result->covered_dirty_epochs[i])
        {
            return true;
        }
    }
    return false;
}

static uint64_t wd_stream_next_generation_for_result_locked(const struct wd_server* server,
                                                            const struct wd_parallel_encode_result* result) {
    uint64_t next_generation = 1;
    if (!server || !result)
    {
        return next_generation;
    }

    const struct wd_net_state* net = &server->net;
    for (uint16_t i = 0; i < result->candidate.covered_base_count; ++i)
    {
        const uint16_t covered_id = result->candidate.covered_base_ids[i];
        if (covered_id < server->total_tiles && net->tiles[covered_id].generation >= next_generation)
        {
            next_generation = net->tiles[covered_id].generation + 1;
        }
    }
    return next_generation;
}

static void wd_stream_requeue_dirty_top_region_locked(struct wd_server* server, uint16_t top_region_id) {
    if (!server || !wd_stream_top_region_still_dirty_locked(server, top_region_id))
    {
        return;
    }

    uint16_t ids[64];
    uint16_t count = 0;
    if (wd_stream_collect_wire_tile_base_ids(server, top_region_id, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT,
                                             ids, &count, (uint16_t)(sizeof(ids) / sizeof(ids[0]))) && count > 0)
    {
        wd_stream_mark_dirty_top_region_locked(server, ids[0]);
    }
}

static void wd_stream_free_encode_result_payload(struct wd_parallel_encode_result* result) {
    if (!result)
    {
        return;
    }
    free(result->payload);
    result->payload = NULL;
}

static void wd_stream_free_encode_result_payloads(struct wd_parallel_encode_result* results, uint16_t result_count) {
    if (!results)
    {
        return;
    }
    for (uint16_t i = 0; i < result_count; ++i)
    {
        wd_stream_free_encode_result_payload(&results[i]);
    }
}

static bool wd_stream_run_encode_batch_locked(struct wd_server* server, struct wd_parallel_encode_batch* batch) {
    if (!server || !batch)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    uint16_t worker_threads = 0;
    const uint64_t wait_start_ns = wd_now_ns();
    net->encoder_batch_active = true;
    pthread_mutex_unlock(&net->lock);
    const bool encoded = wd_stream_encoder_pool_run(server, batch, &worker_threads);
    pthread_mutex_lock(&net->lock);
    net->encoder_batch_active = false;
    pthread_cond_broadcast(&net->encoder_idle_cond);
    net->stats.encode_wait_ns += wd_now_ns() - wait_start_ns;
    net->stats.encode_batches++;
    if (batch->job_count > net->stats.encode_batch_jobs_peak)
    {
        net->stats.encode_batch_jobs_peak = batch->job_count;
    }
    net->stats.encode_worker_threads += worker_threads;
    net->stats.encode_thread_wakeups += worker_threads;
    net->stats.encode_worker_ns += batch->worker_encode_ns;
    net->stats.tile_encode_ns += batch->worker_encode_ns;
    for (uint16_t i = 0; i < batch->job_count; ++i)
    {
        net->stats.compression_attempts += batch->jobs[i].compression_attempts;
        net->stats.compression_wins += batch->jobs[i].compression_wins;
        net->stats.compression_entropy_skips += batch->jobs[i].compression_entropy_skips;
        net->stats.compression_adaptive_skips += batch->jobs[i].compression_adaptive_skips;
        net->stats.compression_nonwins += batch->jobs[i].compression_nonwins;
        net->stats.compression_forced_choices += batch->jobs[i].compression_forced_choices;
        net->stats.compression_ns += batch->jobs[i].compression_ns;
        net->stats.compression_saved_wire_bytes += batch->jobs[i].compression_saved_wire_bytes;
    }
    return encoded;
}

static void wd_stream_clear_retransmit_request_locked(struct wd_server* server, uint16_t tile_id) {
    if (!server || tile_id >= server->total_tiles)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    if (!net->retransmit_queued || !net->retransmit_queued[tile_id])
    {
        return;
    }

    net->retransmit_queued[tile_id] = false;
    if (net->retransmit_requested_generation)
    {
        net->retransmit_requested_generation[tile_id] = 0;
    }
    if (net->retransmit_queue_enqueued_ns)
    {
        wd_stream_note_queue_age_locked(net->retransmit_queue_enqueued_ns[tile_id], &net->stats.retx_queue_age_samples,
                                        &net->stats.retx_queue_age_sum_ns);
        net->retransmit_queue_enqueued_ns[tile_id] = 0;
    }
}

static void wd_stream_send_retransmits_locked(struct wd_server* server, uint64_t now) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    if (net->retransmit_queue_count == 0 || !net->retransmit_queued || !server->framebuffer_xrgb8888)
    {
        return;
    }

    while (net->retransmit_queue_count > 0)
    {
        uint64_t token_budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now);
        if (token_budget == 0)
        {
            break;
        }

        const uint16_t workspace_batch_capacity =
            wd_stream_low_latency_batch_capacity(server, net->retransmit_queue_count);
        struct wd_encode_workspace* workspace =
            wd_stream_encode_workspace_ensure(server, server->total_tiles, workspace_batch_capacity);
        if (!workspace)
        {
            break;
        }
        bool* retx_snapshot = workspace->tile_snapshot;
        uint64_t* epoch_snapshot = workspace->epoch_snapshot;
        uint16_t* regions = workspace->regions;
        memset(retx_snapshot, 0, (size_t)server->total_tiles * sizeof(*retx_snapshot));

        if (net->dirty_epochs)
        {
            memcpy(epoch_snapshot, net->dirty_epochs, (size_t)server->total_tiles * sizeof(*epoch_snapshot));
        }
        else
        {
            memset(epoch_snapshot, 0, (size_t)server->total_tiles * sizeof(*epoch_snapshot));
        }

        uint16_t region_count = 0;
        for (uint16_t i = 0; i < net->retransmit_queue_count; ++i)
        {
            const uint16_t tile_id = net->retransmit_queue[i];
            if (tile_id >= server->total_tiles || !net->retransmit_queued[tile_id])
            {
                continue;
            }
            if (net->dirty_queued && net->dirty_queued[tile_id])
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
                continue;
            }

            const uint64_t requested_generation = net->retransmit_requested_generation ? net->retransmit_requested_generation[tile_id] : 0;
            if (requested_generation != 0 && net->tiles[tile_id].generation < requested_generation)
            {
                /* The request references a generation that has not reached this
                 * tile state yet. Keep it queued so a later fresh update can
                 * either satisfy it or supersede it instead of dropping the
                 * client's repair request. */
                continue;
            }

            uint16_t region_id = 0;
            if (!wd_stream_top_region_for_base_tile(server, tile_id, &region_id))
            {
                continue;
            }
            retx_snapshot[tile_id] = true;
            if (!wd_stream_region_list_contains(regions, region_count, region_id))
            {
                regions[region_count++] = region_id;
            }
        }

        wd_stream_compact_retransmit_queue_locked(server);
        if (region_count == 0)
        {
            break;
        }

        const uint64_t retx_input_sequence = 0;
        const bool network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);

        uint16_t batch_capacity = wd_stream_low_latency_batch_capacity(server, region_count);
        if (batch_capacity == 0)
        {
            break;
        }

        struct wd_parallel_encode_job* jobs = workspace->jobs;
        struct wd_parallel_encode_result* results = workspace->results;

        uint16_t job_count = 0;
        for (uint16_t i = 0; i < region_count && job_count < batch_capacity; ++i)
        {
            wd_stream_init_encode_job_locked(&jobs[job_count], server, regions[i], retx_input_sequence,
                                             token_budget, network_happy, retx_snapshot, epoch_snapshot,
                                             &results[(size_t)job_count * WD_ENCODER_MAX_RESULTS_PER_JOB],
                                             (uint16_t)WD_ENCODER_MAX_RESULTS_PER_JOB);
            job_count++;
        }

        if (job_count == 0)
        {
            break;
        }

        struct wd_parallel_encode_batch batch;
        memset(&batch, 0, sizeof(batch));
        batch.jobs = jobs;
        batch.job_count = job_count;

        const bool encoded = wd_stream_run_encode_batch_locked(server, &batch);

        bool stop_sending = !encoded;
        for (uint16_t ji = 0; ji < job_count && !stop_sending; ++ji)
        {
            const uint16_t result_count = jobs[ji].result_count != 0 ? jobs[ji].result_count : 1;
            for (uint16_t local_result = 0; local_result < result_count; ++local_result)
            {
                struct wd_parallel_encode_result* result = &jobs[ji].result[local_result];
                if (!result->valid)
                {
                    if (result->budget_blocked)
                    {
                        stop_sending = true;
                    }
                    break;
                }

                if (wd_stream_encode_result_stale_locked(server, result))
                {
                    net->stats.encode_jobs_stale++;
                    wd_stream_free_encode_result_payload(result);
                    continue;
                }

                const uint64_t send_now = wd_now_ns();
                uint64_t current_budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, send_now);
                const bool current_network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
                if (!wd_stream_candidate_allowed_for_region_locked(server, &result->candidate, current_budget,
                                                                   current_network_happy, retx_input_sequence != 0))
                {
                    wd_stream_free_encode_result_payload(result);
                    stop_sending = true;
                    break;
                }

                const uint64_t next_generation = wd_stream_next_generation_for_result_locked(server, result);

                struct wd_udp_tile_send_result send_result;
                if (!wd_stream_send_tile_payload_sized_locked(server, result->candidate.tile_id, result->candidate.width,
                                                              result->candidate.height, next_generation, retx_input_sequence,
                                                              &result->payload, result->payload_size,
                                                              result->candidate.compressed_payload, &send_result))
                {
                    wd_stream_free_encode_result_payload(result);
                    continue;
                }

                if (!send_result.all_packets_sent)
                {
                    if (send_result.any_packet_sent)
                    {
                        wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, send_result.bytes_sent);
                    }
                    wd_stream_free_encode_result_payload(result);
                    stop_sending = true;
                    break;
                }

                wd_stream_note_tile_choice_locked(net, result->candidate.compressed_size, result->candidate.uncompressed_size,
                                                  net->udp_payload_target, retx_input_sequence,
                                                  result->candidate.compressed_payload, result->candidate.width, result->candidate.height);

                for (uint16_t i = 0; i < result->candidate.covered_base_count; ++i)
                {
                    const uint16_t covered_id = result->candidate.covered_base_ids[i];
                    if (covered_id >= server->total_tiles)
                    {
                        continue;
                    }
                    (void)wd_stream_update_base_tile_metadata_locked(server, covered_id, next_generation, send_now, retx_input_sequence);
                    wd_stream_clear_retransmit_request_locked(server, covered_id);
                    if (net->dirty_epochs && net->dirty_epochs[covered_id] == result->covered_dirty_epochs[i])
                    {
                        wd_dirty_queue_note_cleared_locked(net, covered_id, server->total_tiles);
                        wd_stream_maybe_clear_dirty_top_region_for_base_locked(server, covered_id);
                    }
                    wd_stream_mark_summary_dirty_locked(server, covered_id);
                }

                net->stats.udp_retx_tiles_sent++;
                wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, send_result.bytes_sent);
                wd_stream_free_encode_result_payload(result);

                if (send_result.send_blocked)
                {
                    stop_sending = true;
                    break;
                }
            }
        }

        wd_stream_free_encode_result_payloads(results, (uint16_t)(job_count * WD_ENCODER_MAX_RESULTS_PER_JOB));
        wd_stream_compact_retransmit_queue_locked(server);
        if (net->udp_tx)
        {
            (void)wd_async_udp_sender_flush(net->udp_tx);
        }

        if (stop_sending)
        {
            break;
        }
    }
}

static bool wd_stream_send_tiles(struct wd_server* server, bool detect_new_damage) {
    struct wd_net_state* net = &server->net;

    if (!server->framebuffer_xrgb8888)
    {
        return false;
    }

    const uint64_t now = wd_now_ns();

    pthread_mutex_lock(&net->lock);

    if (!net->client_connected)
    {
        pthread_mutex_unlock(&net->lock);
        return true;
    }

    if (net->config_update_pending)
    {
        pthread_mutex_unlock(&net->lock);
        return true;
    }

    if (net->stream_policy.tile_refresh_pending || net->stream_policy.stream_mode == WD_STREAM_MODE_TILE_RECOVERY)
    {
        const bool recovering_from_video = net->stream_policy.tile_refresh_pending ||
                                           net->stream_policy.stream_mode == WD_STREAM_MODE_TILE_RECOVERY;
        if (recovering_from_video && net->video_tcp_fd >= 0 && net->video_tx)
        {
            (void)wd_stream_queue_video_control_frame_locked(server, WD_VIDEO_FRAME_END_OF_STREAM);
        }
        wd_stream_collapse_tile_queues_for_video_locked(server);
        wd_stream_invalidate_all_tiles_locked(server);
        net->stream_policy.tile_refresh_pending = false;
        WD_LOG_INFO("stream mode ownership: tile-recovery owner=tiles fresh_udp_tiles=full-refresh tile_repair=enabled video_eos=%s",
                    recovering_from_video ? "sent" : "no");
        wd_stream_policy_set_mode_locked(&net->stream_policy, WD_STREAM_MODE_TILES,
                                         "full tile refresh after video", 0.0, 0.0, 0.0,
                                         net->video_tcp_fd >= 0, wd_video_encoder_available(net->video_encoder));
    }

    if (wd_stream_mode_video_owns_display(net->stream_policy.stream_mode))
    {
        if (!detect_new_damage)
        {
            pthread_mutex_unlock(&net->lock);
            return true;
        }

        /* Video owns the display, so do not turn compositor damage into dirty
         * tile generations or regions that will immediately be discarded.
         * Forced video does not need tile coverage samples at all. Adaptive
         * video keeps only the cheap metadata count used by its exit policy. */
        if (net->stream_policy.video_mode == WD_VIDEO_MODE_FORCE)
        {
            wd_clear_damage_tiles(server);
        }
        else
        {
            const uint16_t dirty_frame_tiles = wd_stream_take_video_damage_sample_locked(server);
            wd_stream_note_mode_frame_locked(net, dirty_frame_tiles, 0, server->total_tiles, false, false);
        }

        net->stats.video_tile_detection_skipped++;
        (void)wd_stream_try_publish_video_frame_locked(server, now);

        /* Queues should normally already be empty after video ownership was
         * established. Only pay the memset cost if transition-era work remains. */
        if (wd_stream_has_queued_tile_work_locked(server))
        {
            wd_stream_collapse_tile_queues_for_video_locked(server);
        }

        /*
         * Do not force another wlroots render for an unchanged scene. The
         * timer promotes wlr_scene_output_needs_frame() into WayDisplay damage,
         * including commits missed by our manual surface trackers.
         */
        server->scene_dirty = false;
        pthread_mutex_unlock(&net->lock);
        return true;
    }

    /*
     * Normal tile-owner send pass. New/reconnected clients are handled through
     * the same dirty-tile pipeline by invalidating tile state and marking the
     * scene dirty. Avoid a separate full-frame catch-up path, because it
     * competes with current dirty tiles and explicit retransmits for the same
     * byte budget.
     */
    uint16_t dirty_frame_tiles = 0;
    const uint16_t pending_tiles_at_frame_start = net->dirty_queue_count;
    const uint64_t dirty_budget_blocked_at_frame_start = net->stats.dirty_budget_blocked;
    const uint64_t full_refresh_at_frame_start = net->stats.framebuffer_diff_full_refreshes;

    if (detect_new_damage)
    {
        const uint64_t dirty_detect_start_ns = wd_now_ns();
        dirty_frame_tiles = wd_detect_dirty_tiles_into_queue_locked(server);
        net->stats.dirty_detect_ns += wd_now_ns() - dirty_detect_start_ns;
        (void)wd_stream_try_publish_video_frame_locked(server, now);
    }

    const bool client_loss = wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
    if (client_loss && net->retransmit_queue_count > 0)
    {
        wd_stream_send_retransmits_locked(server, now);
    }

    while (net->dirty_region_count > 0)
    {
        const uint64_t remaining_byte_budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now);
        const uint64_t tile_input_sequence =
            wd_input_correlation_select(net->input_since_last_fresh_tile, net->last_input_sequence,
                                        net->input_correlation_inflight_sequence);
        /* Only require enough tokens for the smallest guaranteed-progress
         * fallback. The encoder may produce a highly compressed 128x64 tile;
         * gating on the 32 KiB uncompressed maximum needlessly delays terminal
         * and desktop updates while tokens accumulate. */
        const uint32_t base_uncompressed_bytes =
            (uint32_t)server->tile_width * (uint32_t)server->tile_height * WD_BYTES_PER_PIXEL;
        const uint32_t minimum_wire_bytes = wd_stream_tile_wire_bytes_for_payload(base_uncompressed_bytes,
                                                                                  net->udp_payload_target, tile_input_sequence, false);
        if (minimum_wire_bytes == 0 || minimum_wire_bytes > remaining_byte_budget)
        {
            net->stats.dirty_budget_blocked++;
            break;
        }

        uint16_t batch_capacity = wd_stream_low_latency_batch_capacity(server, net->dirty_region_count);
        if (batch_capacity == 0)
        {
            break;
        }

        struct wd_encode_workspace* workspace =
            wd_stream_encode_workspace_ensure(server, server->total_tiles, batch_capacity);
        if (!workspace)
        {
            break;
        }
        bool* dirty_snapshot = workspace->tile_snapshot;
        uint64_t* epoch_snapshot = workspace->epoch_snapshot;
        struct wd_parallel_encode_job* jobs = workspace->jobs;
        struct wd_parallel_encode_result* results = workspace->results;

        memcpy(dirty_snapshot, net->dirty_queued, (size_t)server->total_tiles * sizeof(*dirty_snapshot));
        if (net->dirty_epochs)
        {
            memcpy(epoch_snapshot, net->dirty_epochs, (size_t)server->total_tiles * sizeof(*epoch_snapshot));
        }
        else
        {
            memset(epoch_snapshot, 0, (size_t)server->total_tiles * sizeof(*epoch_snapshot));
        }

        const bool network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
        const uint16_t top_regions_x = wd_tiles_for_width_with_tile(server->display_width, WD_WIRE_TILE_MAX_WIDTH);

        uint16_t job_count = 0;
        while (job_count < batch_capacity && net->dirty_region_count > 0)
        {
            const uint64_t select_start_ns = wd_now_ns();
            const uint64_t select_now_ns = wd_now_ns();
            (void)top_regions_x;
            uint16_t top_id = 0;
            if (!net->dirty_region_scheduler ||
                !wd_dirty_region_scheduler_take(net->dirty_region_scheduler, net->dirty_region_cursor,
                                                select_now_ns, &top_id))
            {
                break;
            }
            wd_stream_sync_dirty_region_count_locked(server);
            if (!wd_stream_top_region_still_dirty_locked(server, top_id))
            {
                wd_stream_remove_dirty_top_region_locked(server, top_id);
                net->stats.dirty_region_select_ns += wd_now_ns() - select_start_ns;
                continue;
            }
            net->dirty_region_cursor = top_id;
            net->stats.dirty_region_select_ns += wd_now_ns() - select_start_ns;

            const uint64_t job_input_sequence = job_count == 0 ? tile_input_sequence : 0;
            wd_stream_init_encode_job_locked(&jobs[job_count], server, top_id, job_input_sequence,
                                             remaining_byte_budget, network_happy, dirty_snapshot, epoch_snapshot,
                                             &results[(size_t)job_count * WD_ENCODER_MAX_RESULTS_PER_JOB],
                                             (uint16_t)WD_ENCODER_MAX_RESULTS_PER_JOB);
            job_count++;
        }

        if (job_count == 0)
        {
            break;
        }

        net->stats.encode_jobs_submitted += job_count;

        struct wd_parallel_encode_batch batch;
        memset(&batch, 0, sizeof(batch));
        batch.jobs = jobs;
        batch.job_count = job_count;

        const bool encoded = wd_stream_run_encode_batch_locked(server, &batch);
        if (!encoded)
        {
            for (uint16_t ri = 0; ri < job_count; ++ri)
            {
                const uint16_t top_id = jobs[ri].top_region_id;
                if (wd_stream_top_region_still_dirty_locked(server, top_id))
                {
                    uint16_t ids[64];
                    uint16_t count = 0;
                    if (wd_stream_collect_wire_tile_base_ids(server, top_id, WD_WIRE_TILE_MAX_WIDTH,
                                                             WD_WIRE_TILE_MAX_HEIGHT, ids, &count,
                                                             (uint16_t)(sizeof(ids) / sizeof(ids[0]))) && count > 0)
                    {
                        wd_stream_mark_dirty_top_region_locked(server, ids[0]);
                    }
                }
            }
            wd_stream_free_encode_result_payloads(
                results, (uint16_t)(job_count * WD_ENCODER_MAX_RESULTS_PER_JOB));
            break;
        }

        bool stop_sending = false;
        uint64_t pending_input_sequence = tile_input_sequence;
        for (uint16_t ji = 0; ji < job_count && !stop_sending; ++ji)
        {
            const uint16_t result_count = jobs[ji].result_count != 0 ? jobs[ji].result_count : 1;
            for (uint16_t local_result = 0; local_result < result_count; ++local_result)
            {
                struct wd_parallel_encode_result* result = &jobs[ji].result[local_result];
                if (!result->valid)
                {
                    if (result->budget_blocked && net->dirty_region_count > 0)
                    {
                        net->stats.dirty_budget_blocked++;
                        stop_sending = true;
                    }
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    break;
                }

                if (wd_stream_encode_result_stale_locked(server, result))
                {
                    net->stats.encode_jobs_stale++;
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    continue;
                }

                const uint64_t send_now = wd_now_ns();
                const uint64_t send_input_sequence = pending_input_sequence;
                const uint64_t current_budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, send_now);
                const bool current_network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
                if (!wd_stream_candidate_allowed_for_region_locked(server, &result->candidate, current_budget,
                                                                   current_network_happy, send_input_sequence != 0))
                {
                    net->stats.dirty_budget_blocked++;
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    stop_sending = true;
                    break;
                }

                const uint64_t next_generation = wd_stream_next_generation_for_result_locked(server, result);

                struct wd_udp_tile_send_result send_result;
                if (!wd_stream_send_tile_payload_sized_locked(server, result->candidate.tile_id, result->candidate.width,
                                                              result->candidate.height, next_generation, send_input_sequence,
                                                              &result->payload, result->payload_size,
                                                              result->candidate.compressed_payload, &send_result))
                {
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    continue;
                }

                if (!send_result.all_packets_sent)
                {
                    if (send_result.any_packet_sent)
                    {
                        wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, send_result.bytes_sent);
                    }
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    stop_sending = true;
                    break;
                }

                if (send_input_sequence != 0)
                {
                    pending_input_sequence = 0;
                }

                wd_stream_note_tile_choice_locked(net, result->candidate.compressed_size, result->candidate.uncompressed_size,
                                                  net->udp_payload_target, send_input_sequence,
                                                  result->candidate.compressed_payload, result->candidate.width, result->candidate.height);

                for (uint16_t i = 0; i < result->candidate.covered_base_count; ++i)
                {
                    const uint16_t covered_id = result->candidate.covered_base_ids[i];
                    if (covered_id >= server->total_tiles)
                    {
                        continue;
                    }
                    (void)wd_stream_update_base_tile_metadata_locked(server, covered_id, next_generation, send_now, send_input_sequence);
                    if (!net->dirty_epochs || net->dirty_epochs[covered_id] == result->covered_dirty_epochs[i])
                    {
                        wd_dirty_queue_note_cleared_locked(net, covered_id, server->total_tiles);
                    }
                    wd_stream_maybe_clear_dirty_top_region_for_base_locked(server, covered_id);
                    wd_stream_mark_summary_dirty_locked(server, covered_id);
                }

                net->stats.dirty_tiles++;
                net->stats.udp_fresh_tiles_sent++;
                net->stats.encode_jobs_completed++;
                wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, send_result.bytes_sent);
                wd_stream_free_encode_result_payload(result);

                if (send_result.send_blocked)
                {
                    stop_sending = true;
                    break;
                }
            }
        }

        for (uint16_t ri = 0; ri < job_count; ++ri)
        {
            const uint16_t top_id = jobs[ri].top_region_id;
            wd_stream_requeue_dirty_top_region_locked(server, top_id);
        }

        wd_stream_free_encode_result_payloads(results, (uint16_t)(job_count * WD_ENCODER_MAX_RESULTS_PER_JOB));
        if (net->udp_tx)
        {
            (void)wd_async_udp_sender_flush(net->udp_tx);
        }

        if (stop_sending)
        {
            break;
        }
    }

    if (!wd_stream_mode_video_owns_display(net->stream_policy.stream_mode))
    {
        wd_stream_send_retransmits_locked(server, now);
    }
    if (net->udp_tx)
    {
        (void)wd_async_udp_sender_flush(net->udp_tx);
    }

    if (detect_new_damage)
    {
        const bool budget_pressure_this_frame = net->stats.dirty_budget_blocked != dirty_budget_blocked_at_frame_start;
        const bool full_refresh_this_frame = net->stats.framebuffer_diff_full_refreshes != full_refresh_at_frame_start;
        if (budget_pressure_this_frame && full_refresh_this_frame)
        {
            const uint64_t blocked_now = net->stats.dirty_budget_blocked - dirty_budget_blocked_at_frame_start;
            net->stats.dirty_budget_blocked_full_refresh += blocked_now;
        }
        wd_stream_note_mode_frame_locked(net, dirty_frame_tiles, pending_tiles_at_frame_start,
                                         server->total_tiles, budget_pressure_this_frame, full_refresh_this_frame);

        /* Pending network work is serviced independently from wlroots scene
         * rendering. Do not force another render of an unchanged scene merely
         * because dirty or retransmit queues remain. */
        server->scene_dirty = false;
    }

    pthread_mutex_unlock(&net->lock);

    return true;
}

bool wd_stream_send_dirty_tiles(struct wd_server* server) {
    return wd_stream_send_tiles(server, true);
}

bool wd_stream_service_tile_queues(struct wd_server* server) {
    return wd_stream_send_tiles(server, false);
}


struct wd_summary_completion_entry {
    uint16_t tile_id;
    uint64_t tile_generation;
};

struct wd_summary_completion {
    struct wd_server* server;
    bool full_summary;
    bool async_pending;
    bool input_since_last_summary;
    uint64_t server_timestamp_ns;
    uint64_t last_input_inject_ns;
    uint64_t summary_epoch;
    uint64_t budget_bytes;
    uint16_t entry_count;
    struct wd_summary_completion_entry entries[];
};

static void wd_stream_rebuild_summary_dirty_queue_locked(struct wd_server* server) {
    struct wd_net_state* net = &server->net;
    if (!net->summary_dirty_tiles || !net->summary_dirty_queue)
    {
        net->summary_dirty_count = 0;
        return;
    }

    uint16_t count = 0;
    for (uint16_t tile_id = 0; tile_id < server->total_tiles; ++tile_id)
    {
        if (net->summary_dirty_tiles[tile_id])
        {
            net->summary_dirty_queue[count++] = tile_id;
        }
    }
    net->summary_dirty_count = count;
}

static void wd_stream_summary_completion(void* user_data, bool success) {
    struct wd_summary_completion* completion = user_data;
    if (!completion)
    {
        return;
    }

    if (completion->server)
    {
        struct wd_server* server = completion->server;
        struct wd_net_state* net = &server->net;

        if (completion->async_pending && net->summary_async_pending_count > 0)
        {
            net->summary_async_pending_count--;
            if (net->summary_async_pending_count == 0)
            {
                net->summary_async_pending_full = false;
            }
        }

        if (!success && completion->budget_bytes != 0)
        {
            wd_stream_refund_tcp_control_budget_locked(net, completion->budget_bytes);
            completion->budget_bytes = 0;
        }

        if (success && net->summary_dirty_tiles && net->summary_epoch == completion->summary_epoch)
        {
            for (uint16_t i = 0; i < completion->entry_count; ++i)
            {
                uint16_t tile_id = completion->entries[i].tile_id;
                if (tile_id < server->total_tiles && net->tiles[tile_id].generation == completion->entries[i].tile_generation)
                {
                    net->summary_dirty_tiles[tile_id] = false;
                }
            }
            wd_stream_rebuild_summary_dirty_queue_locked(server);
        }

        if (success && completion->input_since_last_summary && completion->last_input_inject_ns != 0 &&
            completion->server_timestamp_ns >= completion->last_input_inject_ns)
        {
            net->stats.input_to_summary_samples++;
            net->stats.input_to_summary_sum_ns += completion->server_timestamp_ns - completion->last_input_inject_ns;
            net->input_since_last_summary = false;
        }
    }

    free(completion);
}

static bool wd_stream_send_generation_summary_kind_locked(struct wd_server* server, bool full_summary) {
    struct wd_net_state* net = &server->net;

    if (!net->client_connected || net->tcp_fd < 0)
    {
        return true;
    }

    if (net->config_update_pending)
    {
        return false;
    }

    if (net->control_tx && net->summary_async_pending_count != 0)
    {
        if (!full_summary && net->summary_async_pending_full)
        {
            return false;
        }

        uint32_t dropped = wd_async_tcp_sender_drop_message_type(net->control_tx, WD_MSG_TILE_GENERATION_SUMMARY);
        if (dropped != 0)
        {
            net->stats.tcp_summary_coalesced += dropped;
        }
        if (wd_async_tcp_sender_has_message_type(net->control_tx, WD_MSG_TILE_GENERATION_SUMMARY))
        {
            return false;
        }
    }

    const uint64_t summary_build_start_ns = wd_now_ns();
    uint16_t requested_tile_count = full_summary ? server->total_tiles : net->summary_dirty_count;
    if (requested_tile_count == 0)
    {
        return true;
    }

    size_t payload_capacity = sizeof(struct wd_tile_summary_payload_header) +
                              (size_t)requested_tile_count * sizeof(struct wd_tile_generation_entry);

    uint8_t* payload = malloc(payload_capacity);
    if (!payload)
    {
        return false;
    }

    struct wd_tile_generation_entry* entries =
        (struct wd_tile_generation_entry*)(payload + sizeof(struct wd_tile_summary_payload_header));

    uint16_t entry_count = 0;

    if (full_summary)
    {
        for (uint16_t i = 0; i < server->total_tiles; ++i)
        {
            entries[entry_count].tile_id         = i;
            entries[entry_count].tile_generation = net->tiles[i].generation;
            entry_count++;
        }
    }
    else if (net->summary_dirty_tiles && net->summary_dirty_queue)
    {
        for (uint16_t queue_index = 0; queue_index < net->summary_dirty_count; ++queue_index)
        {
            uint16_t tile_id = net->summary_dirty_queue[queue_index];
            if (tile_id >= server->total_tiles || !net->summary_dirty_tiles[tile_id])
            {
                continue;
            }

            entries[entry_count].tile_id         = tile_id;
            entries[entry_count].tile_generation = net->tiles[tile_id].generation;
            entry_count++;
        }
    }

    if (entry_count == 0)
    {
        free(payload);
        net->summary_dirty_count = 0;
        net->stats.summary_build_ns += wd_now_ns() - summary_build_start_ns;
        return true;
    }

    struct wd_tile_summary_payload_header header;

    header.session_id          = net->session_id;
    header.connection_token    = net->connection_token;
    header.content_epoch       = net->content_epoch;
    header.server_timestamp_ns = wd_now_ns();
    header.tile_count          = entry_count;
    header.flags               = full_summary ? 0u : WD_TILE_SUMMARY_DELTA;

    memcpy(payload, &header, sizeof(header));

    size_t payload_size = sizeof(header) + (size_t)entry_count * sizeof(struct wd_tile_generation_entry);
    net->stats.summary_build_ns += wd_now_ns() - summary_build_start_ns;

    const uint64_t frame_size = (uint64_t)sizeof(struct wd_tcp_header) + (uint64_t)payload_size;
    if (frame_size > UINT32_MAX ||
        !wd_stream_try_consume_tcp_control_budget_locked(net, (uint32_t)frame_size, header.server_timestamp_ns))
    {
        free(payload);
        return false;
    }

    struct wd_summary_completion* completion = calloc(1, sizeof(*completion) +
                                                          (size_t)entry_count * sizeof(completion->entries[0]));
    if (!completion)
    {
        wd_stream_refund_tcp_control_budget_locked(net, frame_size);
        free(payload);
        return false;
    }
    completion->server = server;
    completion->full_summary = full_summary;
    completion->input_since_last_summary = net->input_since_last_summary;
    completion->server_timestamp_ns = header.server_timestamp_ns;
    completion->last_input_inject_ns = net->last_input_inject_ns;
    completion->summary_epoch = net->summary_epoch;
    completion->budget_bytes = frame_size;
    completion->entry_count = entry_count;
    for (uint16_t i = 0; i < entry_count; ++i)
    {
        completion->entries[i].tile_id = entries[i].tile_id;
        completion->entries[i].tile_generation = entries[i].tile_generation;
    }

    bool ok = false;
    if (net->control_tx)
    {
        ok = wd_async_tcp_send_message_ex(net->control_tx, net->tcp_fd, WD_MSG_TILE_GENERATION_SUMMARY, payload,
                                          (uint32_t)payload_size, wd_stream_summary_completion, completion);
        if (ok)
        {
            completion->async_pending = true;
            net->summary_async_pending_count++;
            if (full_summary)
            {
                net->summary_async_pending_full = true;
            }
        }
        if (!ok)
        {
            net->stats.tcp_async_send_failed++;
            if (net->tcp_fd >= 0)
            {
                (void)shutdown(net->tcp_fd, SHUT_RDWR);
            }
        }
    }
    else
    {
        ok = wd_send_tcp_message(net->tcp_fd, WD_MSG_TILE_GENERATION_SUMMARY, payload, (uint32_t)payload_size);
        wd_stream_summary_completion(completion, ok);
        completion = NULL;
    }

    if (ok)
    {
        if (full_summary)
        {
            net->stats.tcp_summary_full_tx++;
        }
        else
        {
            net->stats.tcp_summary_delta_tx++;
            net->stats.tcp_summary_delta_tiles += entry_count;
        }
        net->stats.tcp_summary_tx++;
    }

    if (!ok && completion)
    {
        wd_stream_summary_completion(completion, false);
    }

    free(payload);

    return ok;
}

bool wd_stream_send_generation_summary_locked(struct wd_server* server) {
    if (server && wd_stream_mode_video_owns_display(server->net.stream_policy.stream_mode))
    {
        return false;
    }
    return wd_stream_send_generation_summary_kind_locked(server, true);
}

bool wd_stream_send_pending_generation_summary_locked(struct wd_server* server) {
    if (server && wd_stream_mode_video_owns_display(server->net.stream_policy.stream_mode))
    {
        return false;
    }
    return wd_stream_send_generation_summary_kind_locked(server, false);
}

static void wd_stats_accumulate(struct wd_stats* dst, const struct wd_stats* src) {
    if (!dst || !src)
    {
        return;
    }

    dst->dirty_tiles += src->dirty_tiles;
    dst->udp_tiles_sent += src->udp_tiles_sent;
    dst->udp_fresh_tiles_sent += src->udp_fresh_tiles_sent;
    dst->udp_retx_tiles_sent += src->udp_retx_tiles_sent;
    dst->udp_compressed_tiles_sent += src->udp_compressed_tiles_sent;
    dst->udp_uncompressed_tiles_sent += src->udp_uncompressed_tiles_sent;
    dst->udp_compressed_tile_bytes_sent += src->udp_compressed_tile_bytes_sent;
    dst->udp_uncompressed_tile_bytes_sent += src->udp_uncompressed_tile_bytes_sent;
    dst->udp_packets_sent += src->udp_packets_sent;
    dst->udp_bytes_sent += src->udp_bytes_sent;
    dst->udp_send_pressure_drops += src->udp_send_pressure_drops;
    dst->tile_choice_compressed += src->tile_choice_compressed;
    dst->tile_choice_uncompressed += src->tile_choice_uncompressed;
    dst->tile_choice_compressed_payload_sum += src->tile_choice_compressed_payload_sum;
    dst->tile_choice_uncompressed_payload_sum += src->tile_choice_uncompressed_payload_sum;
    dst->tile_choice_compressed_wire_sum += src->tile_choice_compressed_wire_sum;
    dst->tile_choice_uncompressed_wire_sum += src->tile_choice_uncompressed_wire_sum;
    dst->tile_choice_chosen_wire_sum += src->tile_choice_chosen_wire_sum;
    dst->tile_choice_saved_wire_sum += src->tile_choice_saved_wire_sum;
    dst->compression_attempts += src->compression_attempts;
    dst->compression_wins += src->compression_wins;
    dst->compression_entropy_skips += src->compression_entropy_skips;
    dst->compression_adaptive_skips += src->compression_adaptive_skips;
    dst->compression_nonwins += src->compression_nonwins;
    dst->compression_forced_choices += src->compression_forced_choices;
    dst->compression_ns += src->compression_ns;
    dst->compression_saved_wire_bytes += src->compression_saved_wire_bytes;
    dst->stream_mode_frame_samples += src->stream_mode_frame_samples;
    dst->stream_mode_changed_frame_samples += src->stream_mode_changed_frame_samples;
    dst->stream_mode_dirty_coverage_per_mille_sum += src->stream_mode_dirty_coverage_per_mille_sum;
    if (src->stream_mode_dirty_coverage_per_mille_peak > dst->stream_mode_dirty_coverage_per_mille_peak)
    {
        dst->stream_mode_dirty_coverage_per_mille_peak = src->stream_mode_dirty_coverage_per_mille_peak;
    }
    dst->stream_mode_pending_coverage_per_mille_sum += src->stream_mode_pending_coverage_per_mille_sum;
    if (src->stream_mode_pending_coverage_per_mille_peak > dst->stream_mode_pending_coverage_per_mille_peak)
    {
        dst->stream_mode_pending_coverage_per_mille_peak = src->stream_mode_pending_coverage_per_mille_peak;
    }
    dst->stream_mode_budget_pressure_frames += src->stream_mode_budget_pressure_frames;
    dst->stream_mode_full_refresh_samples += src->stream_mode_full_refresh_samples;
    dst->stream_mode_full_refresh_budget_pressure_frames += src->stream_mode_full_refresh_budget_pressure_frames;
    dst->tile_size_128x64_sent += src->tile_size_128x64_sent;
    dst->tile_size_64x64_sent += src->tile_size_64x64_sent;
    dst->tile_size_32x32_sent += src->tile_size_32x32_sent;
    dst->tile_size_16x16_sent += src->tile_size_16x16_sent;
    dst->tcp_hello_rx += src->tcp_hello_rx;
    dst->tcp_config_tx += src->tcp_config_tx;
    dst->tcp_config_applied_ack_rx += src->tcp_config_applied_ack_rx;
    dst->tcp_config_apply_ack_samples += src->tcp_config_apply_ack_samples;
    dst->tcp_config_apply_ack_sum_ns += src->tcp_config_apply_ack_sum_ns;
    if (src->tcp_config_apply_ack_max_ns > dst->tcp_config_apply_ack_max_ns)
    {
        dst->tcp_config_apply_ack_max_ns = src->tcp_config_apply_ack_max_ns;
    }
    dst->tcp_summary_tx += src->tcp_summary_tx;
    dst->tcp_input_channel_rx += src->tcp_input_channel_rx;
    dst->tcp_input_channel_accepted += src->tcp_input_channel_accepted;
    dst->tcp_input_channel_closed += src->tcp_input_channel_closed;
    dst->tcp_selection_channel_rx += src->tcp_selection_channel_rx;
    dst->tcp_selection_channel_accepted += src->tcp_selection_channel_accepted;
    dst->tcp_selection_channel_closed += src->tcp_selection_channel_closed;
    dst->tcp_video_channel_rx += src->tcp_video_channel_rx;
    dst->tcp_video_channel_accepted += src->tcp_video_channel_accepted;
    dst->tcp_video_channel_closed += src->tcp_video_channel_closed;
    dst->video_frames_published += src->video_frames_published;
    dst->video_frames_superseded += src->video_frames_superseded;
    dst->video_worker_stale_drops += src->video_worker_stale_drops;
    dst->video_tile_detection_skipped += src->video_tile_detection_skipped;
    dst->video_publish_copy_samples += src->video_publish_copy_samples;
    dst->video_publish_copy_ns += src->video_publish_copy_ns;
    dst->video_worker_queue_samples += src->video_worker_queue_samples;
    dst->video_worker_queue_ns += src->video_worker_queue_ns;
    dst->video_frame_attempts += src->video_frame_attempts;
    dst->video_frames_tx += src->video_frames_tx;
    dst->video_keyframe_attempts += src->video_keyframe_attempts;
    dst->video_keyframes_tx += src->video_keyframes_tx;
    dst->video_tcp_bytes_tx += src->video_tcp_bytes_tx;
    dst->video_encode_ns += src->video_encode_ns;
    dst->video_encode_failed += src->video_encode_failed;
    dst->video_tcp_send_failed += src->video_tcp_send_failed;
    dst->video_keyframe_skipped_pending += src->video_keyframe_skipped_pending;
    dst->video_control_frames_tx += src->video_control_frames_tx;
    dst->video_end_of_stream_tx += src->video_end_of_stream_tx;
    dst->video_resize_resets += src->video_resize_resets;
    dst->video_resets += src->video_resets;
    dst->server_frame_timer_samples += src->server_frame_timer_samples;
    dst->server_frame_timer_sum_ns += src->server_frame_timer_sum_ns;
    if (src->server_frame_timer_max_ns > dst->server_frame_timer_max_ns)
    {
        dst->server_frame_timer_max_ns = src->server_frame_timer_max_ns;
    }
    dst->server_render_readback_samples += src->server_render_readback_samples;
    dst->server_render_readback_sum_ns += src->server_render_readback_sum_ns;
    if (src->server_render_readback_max_ns > dst->server_render_readback_max_ns)
    {
        dst->server_render_readback_max_ns = src->server_render_readback_max_ns;
    }
    dst->server_scene_damage_promotions += src->server_scene_damage_promotions;
    dst->server_render_idle_results += src->server_render_idle_results;
    dst->server_render_failed_results += src->server_render_failed_results;
    dst->client_stats_rx += src->client_stats_rx;
    dst->client_udp_packets_rx += src->client_udp_packets_rx;
    dst->client_udp_bytes_rx += src->client_udp_bytes_rx;
    dst->client_tiles_completed += src->client_tiles_completed;
    dst->client_completed_packets += src->client_completed_packets;
    dst->client_partial_tiles_timed_out += src->client_partial_tiles_timed_out;
    dst->client_old_generation_tiles += src->client_old_generation_tiles;
    dst->client_retx_requests_tx += src->client_retx_requests_tx;
    dst->client_udp_interarrival_samples += src->client_udp_interarrival_samples;
    dst->client_udp_interarrival_sum_ns += src->client_udp_interarrival_sum_ns;
    dst->client_udp_interarrival_jitter_samples += src->client_udp_interarrival_jitter_samples;
    dst->client_udp_interarrival_jitter_sum_ns += src->client_udp_interarrival_jitter_sum_ns;
    dst->client_render_visible_reports += src->client_render_visible_reports;
    dst->client_render_hidden_reports += src->client_render_hidden_reports;
    if (src->client_udp_interarrival_max_ns > dst->client_udp_interarrival_max_ns)
    {
        dst->client_udp_interarrival_max_ns = src->client_udp_interarrival_max_ns;
    }
    dst->client_render_frames += src->client_render_frames;
    dst->client_present_samples += src->client_present_samples;
    dst->client_present_sum_ns += src->client_present_sum_ns;
    if (src->client_present_max_ns > dst->client_present_max_ns)
    {
        dst->client_present_max_ns = src->client_present_max_ns;
    }
    dst->client_input_present_samples += src->client_input_present_samples;
    dst->client_input_present_sum_ns += src->client_input_present_sum_ns;
    dst->client_video_frames_rx += src->client_video_frames_rx;
    dst->client_video_bytes_rx += src->client_video_bytes_rx;
    dst->client_video_frames_decoded += src->client_video_frames_decoded;
    dst->client_video_frames_presented += src->client_video_frames_presented;
    dst->client_video_decode_failed += src->client_video_decode_failed;
    dst->client_video_publish_failed += src->client_video_publish_failed;
    dst->client_video_control_frames_rx += src->client_video_control_frames_rx;
    dst->client_video_need_keyframe_drops += src->client_video_need_keyframe_drops;
    dst->client_video_decoder_resets += src->client_video_decoder_resets;
    dst->client_video_decode_samples += src->client_video_decode_samples;
    dst->client_video_decode_sum_ns += src->client_video_decode_sum_ns;
    dst->client_video_messages_rx += src->client_video_messages_rx;
    dst->client_video_data_frames_rx += src->client_video_data_frames_rx;
    dst->client_video_invalid_frames_rx += src->client_video_invalid_frames_rx;
    dst->client_video_stale_frames_dropped += src->client_video_stale_frames_dropped;
    if (src->client_video_last_frame_id_rx > dst->client_video_last_frame_id_rx)
    {
        dst->client_video_last_frame_id_rx = src->client_video_last_frame_id_rx;
    }
    if (src->client_video_last_frame_id_presented > dst->client_video_last_frame_id_presented)
    {
        dst->client_video_last_frame_id_presented = src->client_video_last_frame_id_presented;
    }
    dst->client_video_present_latency_samples += src->client_video_present_latency_samples;
    dst->client_video_present_latency_sum_ns += src->client_video_present_latency_sum_ns;
    dst->retx_req_rx += src->retx_req_rx;
    dst->retx_tiles_req += src->retx_tiles_req;
    dst->retx_req_ignored_live += src->retx_req_ignored_live;
    dst->key_events_rx += src->key_events_rx;
    dst->key_events_injected += src->key_events_injected;
    dst->key_events_dropped += src->key_events_dropped;
    dst->key_state_duplicate_presses += src->key_state_duplicate_presses;
    dst->key_state_release_without_press += src->key_state_release_without_press;
    dst->keyboard_enter_events += src->keyboard_enter_events;
    dst->pointer_events_rx += src->pointer_events_rx;
    dst->pointer_events_injected += src->pointer_events_injected;
    dst->pointer_events_dropped += src->pointer_events_dropped;
    dst->pointer_button_grab_started += src->pointer_button_grab_started;
    dst->pointer_button_grab_ended += src->pointer_button_grab_ended;
    dst->pointer_button_grab_cleared += src->pointer_button_grab_cleared;
    dst->pointer_button_grab_surface_destroyed += src->pointer_button_grab_surface_destroyed;
    dst->xdg_move_invalid_serial += src->xdg_move_invalid_serial;
    dst->xdg_resize_invalid_serial += src->xdg_resize_invalid_serial;
    dst->popup_explicit_scene_trees += src->popup_explicit_scene_trees;
    dst->popup_explicit_scene_tree_failures += src->popup_explicit_scene_tree_failures;
    dst->cursor_shape_requests += src->cursor_shape_requests;
    dst->cursor_shape_tx += src->cursor_shape_tx;
    dst->cursor_shape_coalesced += src->cursor_shape_coalesced;
    dst->cursor_set_cursor_requests += src->cursor_set_cursor_requests;
    dst->cursor_set_cursor_rejected += src->cursor_set_cursor_rejected;
    dst->cursor_set_cursor_hidden += src->cursor_set_cursor_hidden;
    dst->cursor_set_cursor_fallback += src->cursor_set_cursor_fallback;
    dst->input_net_latency_samples += src->input_net_latency_samples;
    dst->input_net_latency_sum_ns += src->input_net_latency_sum_ns;
    dst->input_queue_latency_samples += src->input_queue_latency_samples;
    dst->input_queue_latency_sum_ns += src->input_queue_latency_sum_ns;
    dst->input_wakeup_signals += src->input_wakeup_signals;
    dst->input_wakeup_callbacks += src->input_wakeup_callbacks;
    dst->input_wakeup_events += src->input_wakeup_events;
    dst->input_wakeup_coalesced += src->input_wakeup_coalesced;
    dst->input_wakeup_failures += src->input_wakeup_failures;
    dst->input_to_summary_samples += src->input_to_summary_samples;
    dst->input_to_summary_sum_ns += src->input_to_summary_sum_ns;
    dst->input_to_first_fresh_tile_samples += src->input_to_first_fresh_tile_samples;
    dst->input_to_first_fresh_tile_sum_ns += src->input_to_first_fresh_tile_sum_ns;
    dst->input_correlation_delivery_failed += src->input_correlation_delivery_failed;
    dst->tcp_summary_full_tx += src->tcp_summary_full_tx;
    dst->tcp_summary_delta_tx += src->tcp_summary_delta_tx;
    dst->tcp_summary_delta_tiles += src->tcp_summary_delta_tiles;
    dst->tcp_summary_coalesced += src->tcp_summary_coalesced;
    dst->tcp_summary_budget_interval_ns += src->tcp_summary_budget_interval_ns;
    dst->tcp_summary_repair_backoff += src->tcp_summary_repair_backoff;
    dst->tcp_control_bytes_sent += src->tcp_control_bytes_sent;
    dst->tcp_control_bytes_refunded += src->tcp_control_bytes_refunded;
    dst->tcp_budget_blocked += src->tcp_budget_blocked;
    dst->tcp_async_send_failed += src->tcp_async_send_failed;
    dst->tcp_async_queue_overflow += src->tcp_async_queue_overflow;
    dst->tcp_async_queued += src->tcp_async_queued;
    dst->tcp_async_completed += src->tcp_async_completed;
    dst->tcp_async_completion_failed += src->tcp_async_completion_failed;
    dst->tcp_async_partial_resubmits += src->tcp_async_partial_resubmits;
    if (src->tcp_async_inflight_max > dst->tcp_async_inflight_max)
    {
        dst->tcp_async_inflight_max = src->tcp_async_inflight_max;
    }
    dst->udp_async_send_failed += src->udp_async_send_failed;
    dst->udp_async_queued += src->udp_async_queued;
    dst->udp_async_completed += src->udp_async_completed;
    dst->udp_async_completion_failed += src->udp_async_completion_failed;
    dst->udp_async_fallback_sync += src->udp_async_fallback_sync;
    dst->udp_async_submit_calls += src->udp_async_submit_calls;
    dst->udp_async_partial_submits += src->udp_async_partial_submits;
    if (src->udp_async_inflight_max > dst->udp_async_inflight_max)
    {
        dst->udp_async_inflight_max = src->udp_async_inflight_max;
    }
    dst->rate_decreases += src->rate_decreases;
    dst->rate_increases += src->rate_increases;
    dst->frame_rate_downshifts += src->frame_rate_downshifts;
    dst->frame_rate_upshifts += src->frame_rate_upshifts;
    dst->dirty_region_probes += src->dirty_region_probes;
    dst->dirty_region_hits += src->dirty_region_hits;
    dst->dirty_budget_blocked += src->dirty_budget_blocked;
    dst->dirty_budget_blocked_full_refresh += src->dirty_budget_blocked_full_refresh;
    dst->partial_tile_sends += src->partial_tile_sends;
    dst->partial_tile_packets_sent += src->partial_tile_packets_sent;
    dst->dirty_detect_ns += src->dirty_detect_ns;
    dst->framebuffer_diff_ns += src->framebuffer_diff_ns;
    dst->framebuffer_diff_candidates += src->framebuffer_diff_candidates;
    dst->framebuffer_diff_changed += src->framebuffer_diff_changed;
    dst->framebuffer_diff_unchanged += src->framebuffer_diff_unchanged;
    dst->framebuffer_diff_full_refreshes += src->framebuffer_diff_full_refreshes;
    dst->dirty_region_select_ns += src->dirty_region_select_ns;
    dst->tile_encode_ns += src->tile_encode_ns;
    dst->summary_build_ns += src->summary_build_ns;
    dst->udp_send_ns += src->udp_send_ns;
    dst->encode_jobs_submitted += src->encode_jobs_submitted;
    dst->encode_jobs_completed += src->encode_jobs_completed;
    dst->encode_jobs_stale += src->encode_jobs_stale;
    dst->encode_worker_ns += src->encode_worker_ns;
    dst->encode_wait_ns += src->encode_wait_ns;
    dst->encode_batches += src->encode_batches;
    if (src->encode_batch_jobs_peak > dst->encode_batch_jobs_peak)
    {
        dst->encode_batch_jobs_peak = src->encode_batch_jobs_peak;
    }
    dst->encode_worker_threads += src->encode_worker_threads;
    dst->encode_thread_wakeups += src->encode_thread_wakeups;
    dst->dirty_tiles_stale_skipped += src->dirty_tiles_stale_skipped;
    dst->retx_tiles_superseded_by_fresh += src->retx_tiles_superseded_by_fresh;
    dst->dirty_queue_age_samples += src->dirty_queue_age_samples;
    dst->dirty_queue_age_sum_ns += src->dirty_queue_age_sum_ns;
    dst->retx_queue_age_samples += src->retx_queue_age_samples;
    dst->retx_queue_age_sum_ns += src->retx_queue_age_sum_ns;
    dst->retx_req_stale_generation += src->retx_req_stale_generation;
    dst->retx_req_upgraded_generation += src->retx_req_upgraded_generation;
}

static double wd_avg_ms(uint64_t sum_ns, uint64_t samples) {
    if (samples == 0)
    {
        return 0.0;
    }

    return (double)sum_ns / (double)samples / 1000000.0;
}

void wd_stream_sample_and_maybe_log_stats(struct wd_server* server, bool log_stats) {
    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    struct wd_stats s = net->stats;
    memset(&net->stats, 0, sizeof(net->stats));
    const bool video_owned_before = wd_stream_mode_video_owns_display(net->stream_policy.stream_mode);
    wd_stream_policy_update_health_locked(&net->stream_policy, &s);
    wd_stream_policy_update_mode_locked(&net->stream_policy, &s, server->total_tiles,
                                        net->video_stream_negotiated, net->video_tcp_fd >= 0,
                                        wd_video_encoder_available(net->video_encoder));
    const bool video_owned_after = wd_stream_mode_video_owns_display(net->stream_policy.stream_mode);
    if (video_owned_before != video_owned_after)
    {
        wd_stream_advance_content_epoch_locked(server, video_owned_after ? "video owns display" : "tiles own display");
    }
    uint64_t limited_udp_kib_per_second = net->stream_policy.limited_udp_bytes_per_second / 1024ull;
    uint16_t requested_capture_fps = net->stream_policy.requested_capture_fps;
    uint16_t adaptive_capture_fps = wd_stream_policy_effective_fps_locked(&net->stream_policy);
    uint16_t compositor_refresh_hz = (uint16_t)((server->output_refresh_mhz + 500u) / 1000u);
    uint16_t capture_pacing_fps =
        wd_stream_policy_capture_pacing_fps_locked(&net->stream_policy, compositor_refresh_hz);
    uint16_t client_present_cap_fps = requested_capture_fps != 0 ? requested_capture_fps : WD_DEFAULT_PARTIAL_FPS;
    bool client_render_visible = net->stream_policy.client_render_visible;
    enum wd_stream_mode stream_mode = net->stream_policy.stream_mode;
    uint16_t tile_width = server->tile_width;
    uint16_t tile_height = server->tile_height;
    bool input_channel_connected = net->input_tcp_fd >= 0;
    bool selection_channel_connected = net->selection_tcp_fd >= 0;
    bool video_channel_connected = net->video_tcp_fd >= 0;
    bool video_negotiated = net->video_stream_negotiated;
    bool video_encoder_available = wd_video_encoder_available(net->video_encoder);
    uint8_t video_mode = net->stream_policy.video_mode;
    uint8_t video_min_dirty_percent = net->stream_policy.video_min_dirty_percent;
    uint16_t video_enter_seconds = net->stream_policy.video_enter_seconds;
    uint8_t video_exit_dirty_percent = net->stream_policy.video_exit_dirty_percent;
    uint16_t video_exit_seconds = net->stream_policy.video_exit_seconds;
    uint32_t video_bitrate_kib = wd_stream_video_bitrate_kib_locked(&net->stream_policy);
    uint8_t compression_benchmark_mode = server->tile_compression_benchmark_mode;

    pthread_mutex_unlock(&net->lock);

    struct wd_stats_log_state* stats_log = &server->stats_log;
    wd_stats_accumulate(&stats_log->totals, &s);
    if (!log_stats)
    {
        return;
    }

    s = stats_log->totals;
    memset(&stats_log->totals, 0, sizeof(stats_log->totals));

    bool state_changed = !stats_log->have_prev_state ||
                         stats_log->prev_requested_capture_fps != requested_capture_fps ||
                         stats_log->prev_adaptive_capture_fps != adaptive_capture_fps ||
                         stats_log->prev_capture_pacing_fps != capture_pacing_fps ||
                         stats_log->prev_compositor_refresh_hz != compositor_refresh_hz ||
                         stats_log->prev_client_present_cap_fps != client_present_cap_fps ||
                         stats_log->prev_client_render_visible != client_render_visible ||
                         stats_log->prev_limited_kib != limited_udp_kib_per_second || stats_log->prev_tile_width != tile_width ||
                         stats_log->prev_tile_height != tile_height || stats_log->prev_input_channel != input_channel_connected ||
                         stats_log->prev_selection_channel != selection_channel_connected ||
                         stats_log->prev_video_channel != video_channel_connected ||
                         stats_log->prev_video_negotiated != video_negotiated ||
                         stats_log->prev_video_encoder != video_encoder_available ||
                         stats_log->prev_video_mode != video_mode ||
                         stats_log->prev_video_min_dirty_percent != video_min_dirty_percent ||
                         stats_log->prev_video_enter_seconds != video_enter_seconds ||
                         stats_log->prev_video_exit_dirty_percent != video_exit_dirty_percent ||
                         stats_log->prev_video_exit_seconds != video_exit_seconds ||
                         stats_log->prev_video_bitrate_kib != video_bitrate_kib ||
                         stats_log->prev_stream_mode != stream_mode;

    if (state_changed)
    {
        WD_LOG_DEBUG("state: requested_capture_fps=%u adaptive_capture_fps=%u capture_pacing_fps=%u compositor_refresh_hz=%u client_present_cap_fps=%u client_visible=%s stream_mode=%s owner=%s fresh_udp_tiles=%s tile_repair=%s video_mode=%s video_bitrate_kib=%u video_min_dirty_pct=%u video_enter_seconds=%u video_exit_dirty_pct=%u video_exit_seconds=%u udp_budget_kib_per_sec=%llu base_tile=%ux%u wire_tiles=128x64,64x64,32x32,16x16 tile_compression=%s input_channel=%s selection_channel=%s video_negotiated=%s video_channel=%s video_encoder=%s",
                     (unsigned)requested_capture_fps, (unsigned)adaptive_capture_fps, (unsigned)capture_pacing_fps,
                     (unsigned)compositor_refresh_hz, (unsigned)client_present_cap_fps,
                     client_render_visible ? "yes" : "no", wd_stream_mode_name(stream_mode),
                     wd_stream_mode_owner_name(stream_mode),
                     wd_stream_mode_video_owns_display(stream_mode) ? "paused" : "enabled",
                     wd_stream_mode_video_owns_display(stream_mode) ? "paused" : "enabled",
                     wd_video_mode_name(video_mode), (unsigned)video_bitrate_kib,
                     (unsigned)video_min_dirty_percent, (unsigned)video_enter_seconds,
                     (unsigned)video_exit_dirty_percent, (unsigned)video_exit_seconds,
                     (unsigned long long)limited_udp_kib_per_second, (unsigned)tile_width, (unsigned)tile_height,
                     wd_tile_compression_benchmark_mode_name(compression_benchmark_mode),
                     input_channel_connected ? "yes" : "no", selection_channel_connected ? "yes" : "no",
                     video_negotiated ? "yes" : "no", video_channel_connected ? "yes" : "no",
                     video_encoder_available ? "yes" : "no");

        stats_log->have_prev_state = true;
        stats_log->prev_requested_capture_fps = requested_capture_fps;
        stats_log->prev_adaptive_capture_fps = adaptive_capture_fps;
        stats_log->prev_capture_pacing_fps = capture_pacing_fps;
        stats_log->prev_compositor_refresh_hz = compositor_refresh_hz;
        stats_log->prev_client_present_cap_fps = client_present_cap_fps;
        stats_log->prev_client_render_visible = client_render_visible;
        stats_log->prev_limited_kib = limited_udp_kib_per_second;
        stats_log->prev_tile_width = tile_width;
        stats_log->prev_tile_height = tile_height;
        stats_log->prev_input_channel = input_channel_connected;
        stats_log->prev_selection_channel = selection_channel_connected;
        stats_log->prev_video_channel = video_channel_connected;
        stats_log->prev_video_negotiated = video_negotiated;
        stats_log->prev_video_encoder = video_encoder_available;
        stats_log->prev_video_mode = video_mode;
        stats_log->prev_video_min_dirty_percent = video_min_dirty_percent;
        stats_log->prev_video_enter_seconds = video_enter_seconds;
        stats_log->prev_video_exit_dirty_percent = video_exit_dirty_percent;
        stats_log->prev_video_exit_seconds = video_exit_seconds;
        stats_log->prev_video_bitrate_kib = video_bitrate_kib;
        stats_log->prev_stream_mode = stream_mode;
    }

    bool stream_mode_activity = s.stream_mode_frame_samples != 0 &&
                                (s.stream_mode_dirty_coverage_per_mille_sum != 0 ||
                                 s.stream_mode_pending_coverage_per_mille_sum != 0 ||
                                 s.stream_mode_budget_pressure_frames != 0 || adaptive_capture_fps < requested_capture_fps);
    if (stream_mode_activity)
    {
        const double total_tiles = server->total_tiles != 0 ? (double)server->total_tiles : 1.0;
        const uint64_t sample_count = s.stream_mode_frame_samples != 0 ? s.stream_mode_frame_samples : 1ull;
        const double dirty_avg_pct = wd_stream_coverage_pct(s.stream_mode_dirty_coverage_per_mille_sum / sample_count);
        const double dirty_peak_pct = wd_stream_coverage_pct(s.stream_mode_dirty_coverage_per_mille_peak);
        const double pending_avg_pct = wd_stream_coverage_pct(s.stream_mode_pending_coverage_per_mille_sum / sample_count);
        const double pending_peak_pct = wd_stream_coverage_pct(s.stream_mode_pending_coverage_per_mille_peak);
        const double budget_pressure_pct = (double)s.stream_mode_budget_pressure_frames * 100.0 / (double)sample_count;
        const double wire_avg_bytes = s.udp_tiles_sent ? (double)s.udp_bytes_sent / (double)s.udp_tiles_sent : 0.0;
        const double estimated_full_frame_mib = wire_avg_bytes * total_tiles / 1024.0 / 1024.0;
        const double estimated_budget_fps = wire_avg_bytes > 0.0
                                                ? ((double)limited_udp_kib_per_second * 1024.0) / (wire_avg_bytes * total_tiles)
                                                : 0.0;

        WD_LOG_DEBUG("stream-mode/min: capture_samples=%llu changed_samples=%llu full_refresh_samples=%llu dirty_avg_pct=%.1f dirty_peak_pct=%.1f pending_avg_pct=%.1f pending_peak_pct=%.1f budget_pressure_frames=%llu full_refresh_budget_pressure_frames=%llu budget_pressure_pct=%.1f video_mode=%s video_min_dirty_pct=%u video_enter_seconds=%u video_exit_dirty_pct=%u video_exit_seconds=%u est_tile_full_refresh_mib=%.2f est_full_refreshes_per_sec=%.1f",
                     (unsigned long long)s.stream_mode_frame_samples,
                     (unsigned long long)s.stream_mode_changed_frame_samples,
                     (unsigned long long)s.stream_mode_full_refresh_samples, dirty_avg_pct, dirty_peak_pct,
                     pending_avg_pct, pending_peak_pct,
                     (unsigned long long)s.stream_mode_budget_pressure_frames,
                     (unsigned long long)s.stream_mode_full_refresh_budget_pressure_frames, budget_pressure_pct,
                     wd_video_mode_name(video_mode), (unsigned)video_min_dirty_percent,
                     (unsigned)video_enter_seconds, (unsigned)video_exit_dirty_percent,
                     (unsigned)video_exit_seconds, estimated_full_frame_mib, estimated_budget_fps);
    }

    bool video_activity = s.dirty_tiles != 0 || s.dirty_tiles_stale_skipped != 0 || s.udp_tiles_sent != 0 ||
                          s.udp_fresh_tiles_sent != 0 || s.udp_retx_tiles_sent != 0 || s.udp_packets_sent != 0 ||
                          s.udp_bytes_sent != 0 || s.udp_send_pressure_drops != 0 || s.udp_async_send_failed != 0 ||
                          s.udp_async_queued != 0 || s.udp_async_completed != 0 ||
                          s.udp_async_completion_failed != 0 || s.udp_async_fallback_sync != 0 ||
                          s.tile_choice_compressed != 0 || s.tile_choice_uncompressed != 0 ||
                          s.compression_attempts != 0 || s.compression_entropy_skips != 0 ||
                          s.compression_adaptive_skips != 0 || s.compression_nonwins != 0 ||
                          s.compression_forced_choices != 0 ||
                          s.dirty_queue_age_samples != 0 || s.retx_queue_age_samples != 0 ||
                          s.dirty_region_probes != 0 || s.dirty_region_hits != 0 ||
                          s.dirty_budget_blocked != 0 || s.partial_tile_sends != 0 || s.dirty_detect_ns != 0 ||
                          s.framebuffer_diff_candidates != 0 || s.dirty_region_select_ns != 0 ||
                          s.tile_encode_ns != 0 || s.summary_build_ns != 0 || s.udp_send_ns != 0 || s.encode_jobs_submitted != 0 ||
                          s.encode_jobs_completed != 0 || s.encode_jobs_stale != 0 || s.encode_wait_ns != 0 || s.encode_batches != 0;
    if (video_activity)
    {
        uint64_t choices = s.tile_choice_compressed + s.tile_choice_uncompressed;
        WD_LOG_DEBUG("tile-stream/min: dirty=%llu stale_skip=%llu udp_tiles=%llu fresh=%llu retx=%llu pkts=%llu kib=%.1f wire_avg_B=%.1f comp_sent=%llu uncomp_sent=%llu comp_payload_avg_B=%.1f uncomp_payload_avg_B=%.1f choice_comp=%llu choice_uncomp=%llu choice_comp_payload_avg_B=%.1f choice_raw_payload_avg_B=%.1f choice_comp_wire_avg_B=%.1f choice_uncomp_wire_avg_B=%.1f choice_chosen_wire_avg_B=%.1f choice_saved_kib=%.1f pressure_drops=%llu async_queued=%llu async_completed=%llu async_failed=%llu async_completion_failed=%llu async_fallback=%llu async_inflight_max=%llu submit_calls=%llu partial_submits=%llu pkts_per_submit=%.2f zstd_mode=%s zstd_attempts=%llu zstd_wins=%llu zstd_nonwins=%llu zstd_forced=%llu zstd_entropy_skip=%llu zstd_adaptive_skip=%llu zstd_ms=%.2f zstd_saved_kib=%.1f dirty_q_avg_ms=%.2f retx_q_avg_ms=%.2f dirty_region_probes=%llu dirty_region_hits=%llu dirty_budget_blocked=%llu dirty_budget_blocked_full_refresh=%llu partial_tiles=%llu partial_pkts=%llu detect_ms=%.2f diff_candidates=%llu diff_changed=%llu diff_unchanged=%llu diff_full=%llu diff_ms=%.2f region_pick_ms=%.2f encode_ms=%.2f udp_send_ms=%.2f summary_ms=%.2f tile_sizes=128x64:%llu,64x64:%llu,32x32:%llu,16x16:%llu encode_jobs=%llu/%llu stale=%llu encode_wait_ms=%.2f encode_worker_ms=%.2f encode_batches=%llu encode_batch_peak=%llu encode_workers_avg=%.1f encode_wakeups=%llu",
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
                     (unsigned long long)s.udp_async_queued,
                     (unsigned long long)s.udp_async_completed,
                     (unsigned long long)s.udp_async_send_failed,
                     (unsigned long long)s.udp_async_completion_failed,
                     (unsigned long long)s.udp_async_fallback_sync,
                     (unsigned long long)s.udp_async_inflight_max,
                     (unsigned long long)s.udp_async_submit_calls,
                     (unsigned long long)s.udp_async_partial_submits,
                     s.udp_async_submit_calls ? (double)s.udp_async_queued / (double)s.udp_async_submit_calls : 0.0,
                     wd_tile_compression_benchmark_mode_name(compression_benchmark_mode),
                     (unsigned long long)s.compression_attempts,
                     (unsigned long long)s.compression_wins,
                     (unsigned long long)s.compression_nonwins,
                     (unsigned long long)s.compression_forced_choices,
                     (unsigned long long)s.compression_entropy_skips,
                     (unsigned long long)s.compression_adaptive_skips,
                     (double)s.compression_ns / 1000000.0,
                     (double)s.compression_saved_wire_bytes / 1024.0,
                     wd_avg_ms(s.dirty_queue_age_sum_ns, s.dirty_queue_age_samples),
                     wd_avg_ms(s.retx_queue_age_sum_ns, s.retx_queue_age_samples),
                     (unsigned long long)s.dirty_region_probes,
                     (unsigned long long)s.dirty_region_hits,
                     (unsigned long long)s.dirty_budget_blocked,
                     (unsigned long long)s.dirty_budget_blocked_full_refresh,
                     (unsigned long long)s.partial_tile_sends,
                     (unsigned long long)s.partial_tile_packets_sent,
                     (double)s.dirty_detect_ns / 1000000.0,
                     (unsigned long long)s.framebuffer_diff_candidates,
                     (unsigned long long)s.framebuffer_diff_changed,
                     (unsigned long long)s.framebuffer_diff_unchanged,
                     (unsigned long long)s.framebuffer_diff_full_refreshes,
                     (double)s.framebuffer_diff_ns / 1000000.0,
                     (double)s.dirty_region_select_ns / 1000000.0,
                     (double)s.tile_encode_ns / 1000000.0,
                     (double)s.udp_send_ns / 1000000.0,
                     (double)s.summary_build_ns / 1000000.0,
                     (unsigned long long)s.tile_size_128x64_sent,
                     (unsigned long long)s.tile_size_64x64_sent,
                     (unsigned long long)s.tile_size_32x32_sent,
                     (unsigned long long)s.tile_size_16x16_sent,
                     (unsigned long long)s.encode_jobs_completed,
                     (unsigned long long)s.encode_jobs_submitted,
                     (unsigned long long)s.encode_jobs_stale,
                     (double)s.encode_wait_ns / 1000000.0,
                     (double)s.encode_worker_ns / 1000000.0,
                     (unsigned long long)s.encode_batches,
                     (unsigned long long)s.encode_batch_jobs_peak,
                     s.encode_batches ? (double)s.encode_worker_threads / (double)s.encode_batches : 0.0,
                     (unsigned long long)s.encode_thread_wakeups);
    }

    if (s.server_frame_timer_samples != 0 || s.server_render_readback_samples != 0)
    {
        WD_LOG_DEBUG("server-loop/min: service_ticks=%llu tick_avg_ms=%.2f tick_max_ms=%.2f render_readback=%llu render_readback_avg_ms=%.2f render_readback_max_ms=%.2f scene_promote=%llu render_idle=%llu render_failed=%llu encode_avg_ms=%.2f",
                     (unsigned long long)s.server_frame_timer_samples,
                     s.server_frame_timer_samples ?
                         (double)s.server_frame_timer_sum_ns / (double)s.server_frame_timer_samples / 1000000.0 : 0.0,
                     (double)s.server_frame_timer_max_ns / 1000000.0,
                     (unsigned long long)s.server_render_readback_samples,
                     s.server_render_readback_samples ?
                         (double)s.server_render_readback_sum_ns / (double)s.server_render_readback_samples / 1000000.0 : 0.0,
                     (double)s.server_render_readback_max_ns / 1000000.0,
                     (unsigned long long)s.server_scene_damage_promotions,
                     (unsigned long long)s.server_render_idle_results,
                     (unsigned long long)s.server_render_failed_results,
                     s.video_frame_attempts ?
                         (double)s.video_encode_ns / (double)s.video_frame_attempts / 1000000.0 : 0.0);
    }

    bool video_stream_activity = s.video_frames_published != 0 || s.video_frames_superseded != 0 ||
                                 s.video_worker_stale_drops != 0 || s.video_tile_detection_skipped != 0 ||
                                 s.video_frame_attempts != 0 ||
                                 s.video_frames_tx != 0 || s.video_keyframe_attempts != 0 || s.video_keyframes_tx != 0 ||
                                 s.video_tcp_bytes_tx != 0 || s.video_encode_failed != 0 ||
                                 s.video_tcp_send_failed != 0 || s.video_keyframe_skipped_pending != 0 ||
                                 s.video_control_frames_tx != 0 || s.video_end_of_stream_tx != 0 ||
                                 s.video_resize_resets != 0 || s.video_resets != 0;
    if (video_stream_activity)
    {
        WD_LOG_DEBUG("video-stream/min: mode=%s configured_bitrate_kib=%u published=%llu superseded=%llu worker_stale=%llu tile_detect_skipped=%llu publish_copy_avg_ms=%.2f worker_queue_avg_ms=%.2f frame_attempts=%llu frames_tx=%llu keyframe_attempts=%llu keyframes_tx=%llu control_tx=%llu eos_tx=%llu tcp_kib=%.1f encode_ms=%.2f encode_failed=%llu tcp_send_failed=%llu skipped_pending=%llu resets=%llu resize_resets=%llu",
                     wd_video_mode_name(video_mode), (unsigned)video_bitrate_kib,
                     (unsigned long long)s.video_frames_published,
                     (unsigned long long)s.video_frames_superseded,
                     (unsigned long long)s.video_worker_stale_drops,
                     (unsigned long long)s.video_tile_detection_skipped,
                     s.video_publish_copy_samples ?
                         (double)s.video_publish_copy_ns / (double)s.video_publish_copy_samples / 1000000.0 : 0.0,
                     s.video_worker_queue_samples ?
                         (double)s.video_worker_queue_ns / (double)s.video_worker_queue_samples / 1000000.0 : 0.0,
                     (unsigned long long)s.video_frame_attempts,
                     (unsigned long long)s.video_frames_tx,
                     (unsigned long long)s.video_keyframe_attempts,
                     (unsigned long long)s.video_keyframes_tx,
                     (unsigned long long)s.video_control_frames_tx,
                     (unsigned long long)s.video_end_of_stream_tx,
                     (double)s.video_tcp_bytes_tx / 1024.0,
                     (double)s.video_encode_ns / 1000000.0,
                     (unsigned long long)s.video_encode_failed,
                     (unsigned long long)s.video_tcp_send_failed,
                     (unsigned long long)s.video_keyframe_skipped_pending,
                     (unsigned long long)s.video_resets,
                     (unsigned long long)s.video_resize_resets);
    }

    bool repair_activity = s.retx_req_rx != 0 || s.retx_tiles_req != 0 || s.retx_req_ignored_live != 0 ||
                           s.retx_req_stale_generation != 0 || s.retx_req_upgraded_generation != 0 ||
                           s.retx_tiles_superseded_by_fresh != 0 ||
                           s.tcp_summary_tx != 0 || s.tcp_summary_delta_tx != 0 ||
                           s.tcp_summary_delta_tiles != 0 || s.tcp_summary_coalesced != 0 ||
                           s.tcp_summary_repair_backoff != 0 || s.tcp_summary_budget_interval_ns != 0 ||
                           s.rate_decreases != 0 || s.rate_increases != 0 ||
                           s.frame_rate_downshifts != 0 || s.frame_rate_upshifts != 0;
    if (repair_activity)
    {
        WD_LOG_DEBUG("repair/min: summaries=%llu full=%llu delta=%llu delta_tiles=%llu summary_coalesced=%llu summary_interval_ms=%llu repair_backoff=%llu retx_req=%llu retx_tiles=%llu stale_drop=%llu stale_upgraded=%llu ignored_live=%llu superseded=%llu rate_down=%llu rate_up=%llu capture_down=%llu capture_up=%llu",
                     (unsigned long long)s.tcp_summary_tx, (unsigned long long)s.tcp_summary_full_tx,
                     (unsigned long long)s.tcp_summary_delta_tx, (unsigned long long)s.tcp_summary_delta_tiles,
                     (unsigned long long)s.tcp_summary_coalesced,
                     (unsigned long long)(s.tcp_summary_budget_interval_ns / 1000000ull),
                     (unsigned long long)s.tcp_summary_repair_backoff,
                     (unsigned long long)s.retx_req_rx, (unsigned long long)s.retx_tiles_req,
                     (unsigned long long)s.retx_req_stale_generation,
                     (unsigned long long)s.retx_req_upgraded_generation,
                     (unsigned long long)s.retx_req_ignored_live, (unsigned long long)s.retx_tiles_superseded_by_fresh,
                     (unsigned long long)s.rate_decreases, (unsigned long long)s.rate_increases,
                     (unsigned long long)s.frame_rate_downshifts, (unsigned long long)s.frame_rate_upshifts);
    }

    bool client_activity = s.client_tiles_completed != 0 || s.client_udp_bytes_rx != 0 || s.client_partial_tiles_timed_out != 0 ||
                           s.client_old_generation_tiles != 0 || s.client_retx_requests_tx != 0 ||
                           s.client_udp_interarrival_samples != 0 || s.client_render_frames != 0 ||
                           s.client_present_samples != 0 || s.client_input_present_samples != 0 ||
                           s.client_render_visible_reports != 0 || s.client_render_hidden_reports != 0;
    if (client_activity)
    {
        WD_LOG_DEBUG("client/min: reports=%llu visible=%llu hidden=%llu completed=%llu udp_kib=%.1f partial_timeouts=%llu old_gen=%llu retx_req_tx=%llu interarrival_avg_ms=%.2f jitter_avg_ms=%.2f max_gap_ms=%.2f remote_render_frames=%llu present_avg_ms=%.2f present_max_ms=%.2f input_present_avg_ms=%.2f",
                     (unsigned long long)s.client_stats_rx,
                     (unsigned long long)s.client_render_visible_reports,
                     (unsigned long long)s.client_render_hidden_reports,
                     (unsigned long long)s.client_tiles_completed,
                     (double)s.client_udp_bytes_rx / 1024.0, (unsigned long long)s.client_partial_tiles_timed_out,
                     (unsigned long long)s.client_old_generation_tiles, (unsigned long long)s.client_retx_requests_tx,
                     wd_avg_ms(s.client_udp_interarrival_sum_ns, s.client_udp_interarrival_samples),
                     wd_avg_ms(s.client_udp_interarrival_jitter_sum_ns, s.client_udp_interarrival_jitter_samples),
                     (double)s.client_udp_interarrival_max_ns / 1000000.0,
                     (unsigned long long)s.client_render_frames,
                     wd_avg_ms(s.client_present_sum_ns, s.client_present_samples),
                     (double)s.client_present_max_ns / 1000000.0,
                     wd_avg_ms(s.client_input_present_sum_ns, s.client_input_present_samples));
    }

    if (s.client_video_messages_rx != 0 || s.client_video_data_frames_rx != 0 ||
        s.client_video_frames_rx != 0 || s.client_video_frames_decoded != 0 ||
        s.client_video_frames_presented != 0 || s.client_video_decode_failed != 0 ||
        s.client_video_publish_failed != 0 || s.client_video_control_frames_rx != 0 ||
        s.client_video_invalid_frames_rx != 0 || s.client_video_stale_frames_dropped != 0 ||
        s.client_video_need_keyframe_drops != 0 || s.client_video_decoder_resets != 0)
    {
        WD_LOG_DEBUG("client-video/min: messages=%llu data=%llu legacy_rx=%llu decoded=%llu presented=%llu control=%llu invalid=%llu stale_drop=%llu kib=%.1f decode_avg_ms=%.2f present_age_avg_ms=%.2f decode_failed=%llu publish_failed=%llu need_keyframe_drops=%llu resets=%llu last_rx=%llu last_presented=%llu",
                     (unsigned long long)s.client_video_messages_rx,
                     (unsigned long long)s.client_video_data_frames_rx,
                     (unsigned long long)s.client_video_frames_rx,
                     (unsigned long long)s.client_video_frames_decoded,
                     (unsigned long long)s.client_video_frames_presented,
                     (unsigned long long)s.client_video_control_frames_rx,
                     (unsigned long long)s.client_video_invalid_frames_rx,
                     (unsigned long long)s.client_video_stale_frames_dropped,
                     (double)s.client_video_bytes_rx / 1024.0,
                     wd_avg_ms(s.client_video_decode_sum_ns, s.client_video_decode_samples),
                     wd_avg_ms(s.client_video_present_latency_sum_ns, s.client_video_present_latency_samples),
                     (unsigned long long)s.client_video_decode_failed,
                     (unsigned long long)s.client_video_publish_failed,
                     (unsigned long long)s.client_video_need_keyframe_drops,
                     (unsigned long long)s.client_video_decoder_resets,
                     (unsigned long long)s.client_video_last_frame_id_rx,
                     (unsigned long long)s.client_video_last_frame_id_presented);
    }

    bool control_activity = s.tcp_hello_rx != 0 || s.tcp_config_tx != 0 || s.tcp_config_applied_ack_rx != 0 || s.tcp_input_channel_rx != 0 ||
                            s.tcp_input_channel_accepted != 0 || s.tcp_input_channel_closed != 0 ||
                            s.tcp_selection_channel_rx != 0 || s.tcp_selection_channel_accepted != 0 ||
                            s.tcp_selection_channel_closed != 0 || s.tcp_video_channel_rx != 0 ||
                            s.tcp_video_channel_accepted != 0 || s.tcp_video_channel_closed != 0 ||
                            s.tcp_async_send_failed != 0 ||
                            s.tcp_async_queued != 0 || s.tcp_async_completed != 0 ||
                            s.tcp_async_completion_failed != 0 || s.tcp_async_queue_overflow != 0 || s.tcp_async_partial_resubmits != 0 ||
                            s.tcp_control_bytes_sent != 0 || s.tcp_control_bytes_refunded != 0 || s.tcp_budget_blocked != 0;
    if (control_activity)
    {
        WD_LOG_DEBUG("control/min: hello=%llu config=%llu config_ack=%llu config_ack_avg_ms=%.2f config_ack_max_ms=%.2f input_rx=%llu input_accepted=%llu input_closed=%llu selection_rx=%llu selection_accepted=%llu selection_closed=%llu video_rx=%llu video_accepted=%llu video_closed=%llu async_queued=%llu async_completed=%llu async_send_failed=%llu async_completion_failed=%llu async_overflow=%llu async_partial=%llu async_inflight_max=%llu tcp_kib=%.1f tcp_refund_kib=%.1f tcp_budget_blocked=%llu",
                     (unsigned long long)s.tcp_hello_rx, (unsigned long long)s.tcp_config_tx,
                     (unsigned long long)s.tcp_config_applied_ack_rx,
                     wd_avg_ms(s.tcp_config_apply_ack_sum_ns, s.tcp_config_apply_ack_samples),
                     (double)s.tcp_config_apply_ack_max_ns / 1000000.0,
                     (unsigned long long)s.tcp_input_channel_rx, (unsigned long long)s.tcp_input_channel_accepted,
                     (unsigned long long)s.tcp_input_channel_closed, (unsigned long long)s.tcp_selection_channel_rx,
                     (unsigned long long)s.tcp_selection_channel_accepted, (unsigned long long)s.tcp_selection_channel_closed,
                     (unsigned long long)s.tcp_video_channel_rx, (unsigned long long)s.tcp_video_channel_accepted,
                     (unsigned long long)s.tcp_video_channel_closed,
                     (unsigned long long)s.tcp_async_queued,
                     (unsigned long long)s.tcp_async_completed,
                     (unsigned long long)s.tcp_async_send_failed,
                     (unsigned long long)s.tcp_async_completion_failed,
                     (unsigned long long)s.tcp_async_queue_overflow,
                     (unsigned long long)s.tcp_async_partial_resubmits,
                     (unsigned long long)s.tcp_async_inflight_max,
                     (double)s.tcp_control_bytes_sent / 1024.0,
                     (double)s.tcp_control_bytes_refunded / 1024.0,
                     (unsigned long long)s.tcp_budget_blocked);
    }

    bool input_activity = s.key_events_rx != 0 || s.key_events_injected != 0 || s.key_events_dropped != 0 ||
                          s.key_state_duplicate_presses != 0 || s.key_state_release_without_press != 0 ||
                          s.keyboard_enter_events != 0 || s.pointer_events_rx != 0 || s.pointer_events_injected != 0 ||
                          s.pointer_events_dropped != 0 || s.pointer_button_grab_started != 0 ||
                          s.pointer_button_grab_ended != 0 || s.pointer_button_grab_cleared != 0 ||
                          s.pointer_button_grab_surface_destroyed != 0 || s.input_queue_latency_samples != 0 ||
                          s.input_wakeup_signals != 0 || s.input_wakeup_callbacks != 0 ||
                          s.input_wakeup_failures != 0 || s.input_to_summary_samples != 0 ||
                          s.input_to_first_fresh_tile_samples != 0 || s.input_correlation_delivery_failed != 0;
    if (input_activity)
    {
        WD_LOG_DEBUG("input/min: key_rx=%llu key_injected=%llu key_dropped=%llu dup_press=%llu release_without_press=%llu keyboard_enter=%llu pointer_rx=%llu pointer_injected=%llu pointer_dropped=%llu grabs_start=%llu grabs_end=%llu grabs_clear=%llu grab_surface_destroyed=%llu wake_signals=%llu wake_callbacks=%llu wake_events=%llu wake_coalesced=%llu wake_failures=%llu queue_avg_ms=%.2f input_to_summary_avg_ms=%.2f input_to_first_tile_avg_ms=%.2f input_delivery_failed=%llu",
                     (unsigned long long)s.key_events_rx, (unsigned long long)s.key_events_injected,
                     (unsigned long long)s.key_events_dropped, (unsigned long long)s.key_state_duplicate_presses,
                     (unsigned long long)s.key_state_release_without_press, (unsigned long long)s.keyboard_enter_events,
                     (unsigned long long)s.pointer_events_rx, (unsigned long long)s.pointer_events_injected,
                     (unsigned long long)s.pointer_events_dropped, (unsigned long long)s.pointer_button_grab_started,
                     (unsigned long long)s.pointer_button_grab_ended, (unsigned long long)s.pointer_button_grab_cleared,
                     (unsigned long long)s.pointer_button_grab_surface_destroyed,
                     (unsigned long long)s.input_wakeup_signals, (unsigned long long)s.input_wakeup_callbacks,
                     (unsigned long long)s.input_wakeup_events, (unsigned long long)s.input_wakeup_coalesced,
                     (unsigned long long)s.input_wakeup_failures,
                     wd_avg_ms(s.input_queue_latency_sum_ns, s.input_queue_latency_samples),
                     wd_avg_ms(s.input_to_summary_sum_ns, s.input_to_summary_samples),
                     wd_avg_ms(s.input_to_first_fresh_tile_sum_ns, s.input_to_first_fresh_tile_samples),
                     (unsigned long long)s.input_correlation_delivery_failed);
    }

    bool compositor_activity = s.xdg_move_invalid_serial != 0 || s.xdg_resize_invalid_serial != 0 ||
                               s.popup_explicit_scene_trees != 0 || s.popup_explicit_scene_tree_failures != 0 ||
                               s.cursor_shape_requests != 0 || s.cursor_shape_tx != 0 ||
                               s.cursor_shape_coalesced != 0 || s.cursor_set_cursor_requests != 0 ||
                               s.cursor_set_cursor_rejected != 0 || s.cursor_set_cursor_hidden != 0 ||
                               s.cursor_set_cursor_fallback != 0;
    if (compositor_activity)
    {
        WD_LOG_DEBUG("compositor/min: xdg_move_bad_serial=%llu xdg_resize_bad_serial=%llu popup_scene=%llu popup_scene_fail=%llu cursor_shape=%llu cursor_shape_tx=%llu cursor_shape_coalesced=%llu cursor_set=%llu cursor_reject=%llu cursor_hidden=%llu cursor_fallback=%llu",
                     (unsigned long long)s.xdg_move_invalid_serial, (unsigned long long)s.xdg_resize_invalid_serial,
                     (unsigned long long)s.popup_explicit_scene_trees, (unsigned long long)s.popup_explicit_scene_tree_failures,
                     (unsigned long long)s.cursor_shape_requests, (unsigned long long)s.cursor_shape_tx,
                     (unsigned long long)s.cursor_shape_coalesced, (unsigned long long)s.cursor_set_cursor_requests,
                     (unsigned long long)s.cursor_set_cursor_rejected, (unsigned long long)s.cursor_set_cursor_hidden,
                     (unsigned long long)s.cursor_set_cursor_fallback);
    }
}
