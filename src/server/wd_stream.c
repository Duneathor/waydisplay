#include "waydisplay/wd_media_clock.h"
#include "waydisplay/wd_net.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "waydisplay/wd_zstd.h"
#include "wd_async_tcp.h"
#include "wd_async_udp.h"
#include "wd_async_udp_accounting.h"
#include "wd_audio_stream.h"
#include "wd_dirty_region_scheduler.h"
#include "wd_frame_pacing.h"
#include "wd_input_correlation.h"
#include "wd_server_internal.h"
#include "wd_server_compositor.h"
#include "wd_stream_pipeline_internal.h"
#include "wd_tile_policy.h"
#include "wd_video_encoder.h"
#include "wd_video_transition.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static void wd_stream_encoder_pool_destroy(struct wd_server* server);
static void wd_stream_encode_workspace_destroy(struct wd_server* server);
static void wd_detect_one_dirty_tile_into_queue_locked(struct wd_server* server, uint16_t tile_id);
static bool wd_stream_collect_wire_tile_base_ids(const struct wd_server* server, uint16_t tile_id, uint16_t tile_width,
                                                 uint16_t tile_height, uint16_t* out_ids, uint16_t* out_count, uint16_t max_count);

static void wd_stream_note_input_delivery_locked(struct wd_net_state* net, uint64_t input_sequence, uint64_t input_inject_ns,
                                                 uint64_t delivery_ns, bool success) {
    if (!net || input_sequence == 0)
    {
        return;
    }

    const struct wd_input_correlation_completion completion =
        wd_input_correlation_complete(net->input_correlation_inflight_sequence, net->last_input_sequence, input_sequence, success);
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
    if (completion.clear_pending || (net->input_correlation_inflight_sequence == 0 && net->last_input_sequence == input_sequence))
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

double wd_stream_coverage_pct(uint64_t per_mille) {
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

    uint64_t       cap = bytes_per_second / WD_STREAM_TOKEN_BURST_DIVISOR;
    const uint64_t max_uncompressed_wire_tile =
        (uint64_t)WD_WIRE_TILE_MAX_WIDTH * (uint64_t)WD_WIRE_TILE_MAX_HEIGHT * (uint64_t)WD_BYTES_PER_PIXEL +
        (uint64_t)WD_UDP_TILE_HEADER_MAX_SIZE;
    if (cap < max_uncompressed_wire_tile)
    {
        cap = max_uncompressed_wire_tile;
    }
    return cap ? cap : bytes_per_second;
}

static uint64_t wd_stream_clamp_link_rate(uint64_t bytes_per_second) {
    if (bytes_per_second == 0)
    {
        bytes_per_second = WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND;
    }

    if (bytes_per_second < WD_UDP_RATE_MIN_BYTES_PER_SECOND)
    {
        bytes_per_second = WD_UDP_RATE_MIN_BYTES_PER_SECOND;
    }

    if (bytes_per_second > WD_UDP_RATE_MAX_BYTES_PER_SECOND)
    {
        bytes_per_second = WD_UDP_RATE_MAX_BYTES_PER_SECOND;
    }

    return bytes_per_second;
}

static uint64_t wd_stream_link_rate_from_kib(uint32_t kib_per_second) {
    if (kib_per_second == 0)
    {
        return 0;
    }

    uint64_t bytes_per_second = (uint64_t)kib_per_second * 1024ull;
    if (bytes_per_second / 1024ull != (uint64_t)kib_per_second)
    {
        bytes_per_second = WD_UDP_RATE_MAX_BYTES_PER_SECOND;
    }

    return wd_stream_clamp_link_rate(bytes_per_second);
}

void wd_stream_policy_rebuild_bandwidth_plan_locked(struct wd_stream_policy* policy, enum wd_bandwidth_mode mode) {
    if (!policy)
    {
        return;
    }

    uint64_t link_rate = policy->recent_link_bytes_per_second;
    if (link_rate == 0)
    {
        link_rate = policy->safe_link_bytes_per_second;
    }
    if (link_rate == 0)
    {
        link_rate = WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND;
    }
    link_rate = wd_stream_clamp_link_rate(link_rate);

    const struct wd_bandwidth_plan plan = wd_bandwidth_plan_build(
        link_rate, mode, policy->bandwidth_audio_enabled, policy->bandwidth_audio_bitrate);

    policy->safe_link_bytes_per_second      = policy->safe_link_bytes_per_second != 0 ? policy->safe_link_bytes_per_second : link_rate;
    policy->recent_link_bytes_per_second    = link_rate;
    policy->tile_fresh_bytes_per_second     = plan.fresh_tile_bytes_per_second;
    policy->tile_repair_bytes_per_second    = plan.repair_bytes_per_second;
    policy->video_bytes_per_second          = plan.video_bytes_per_second;
    policy->control_bytes_per_second        = plan.control_bytes_per_second;
    policy->audio_cap_bytes_per_second      = plan.audio_cap_bytes_per_second;
    policy->audio_reserved_bytes_per_second = plan.audio_reserved_bytes_per_second;
    policy->overhead_bytes_per_second       = plan.overhead_bytes_per_second;

    if (mode == WD_BANDWIDTH_MODE_TILES)
    {
        const uint64_t tile_rate = wd_bandwidth_plan_media_bytes(&plan, WD_BANDWIDTH_MODE_TILES);
        policy->adaptive_tile_fresh_bytes_per_second = plan.fresh_tile_bytes_per_second;
        policy->tile_media_bytes_per_second = tile_rate;
        const uint64_t fresh_floor = plan.fresh_tile_bytes_per_second < WD_UDP_RATE_MIN_BYTES_PER_SECOND
                                         ? plan.fresh_tile_bytes_per_second
                                         : WD_UDP_RATE_MIN_BYTES_PER_SECOND;
        policy->tile_media_floor_bytes_per_second = plan.repair_bytes_per_second + fresh_floor;
        policy->tile_media_ceiling_bytes_per_second = tile_rate;
    }
}

static void wd_stream_policy_reset_tokens(struct wd_stream_policy* policy) {
    if (!policy)
    {
        return;
    }

    wd_frame_pacing_reset(&policy->frame_pacing);
    policy->last_video_frame_send_ns = 0;
    wd_bandwidth_bucket_reset(&policy->fresh_tile_bucket);
    wd_bandwidth_bucket_reset(&policy->repair_bucket);
    wd_bandwidth_bucket_reset(&policy->control_bucket);
}

void wd_stream_policy_set_defaults(struct wd_stream_policy* policy) {
    if (!policy)
    {
        return;
    }

    memset(policy, 0, sizeof(*policy));

    policy->requested_capture_fps             = WD_DEFAULT_CAPTURE_FPS;
    policy->adaptive_capture_fps              = WD_DEFAULT_CAPTURE_FPS;
    policy->stream_mode                       = WD_STREAM_MODE_TILES;
    policy->video_mode                        = WD_VIDEO_MODE_AUTO;
    policy->video_min_dirty_percent           = WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT;
    policy->video_enter_seconds               = WD_VIDEO_ENTER_SECONDS_DEFAULT;
    policy->video_exit_dirty_percent          = WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT;
    policy->video_exit_seconds                = WD_VIDEO_EXIT_SECONDS_DEFAULT;
    policy->video_bitrate_kib_per_second      = 0;
    policy->video_candidate_seconds           = 0;
    policy->tile_recovery_seconds             = 0;
    policy->video_client_failure_seconds      = 0;
    policy->tile_refresh_pending              = false;
    policy->tile_recovery_refresh_started     = false;
    policy->tile_recovery_refresh_sent        = false;
    policy->tile_recovery_wait_seconds        = 0;
    policy->video_retry_cooldown_seconds      = 0;
    policy->video_bootstrap_pending         = false;
    policy->video_bootstrap_refresh_started = false;
    policy->video_bootstrap_refresh_sent    = false;
    policy->video_bootstrap_wait_seconds    = 0;
    policy->video_bootstrap_content_epoch   = 0;
    policy->tile_recovery_content_epoch     = 0;
    policy->video_recovery_class            = WD_VIDEO_RECOVERY_NONE;
    policy->frame_rate_good_seconds           = 0;
    policy->safe_link_bytes_per_second        = WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND;
    policy->recent_link_bytes_per_second      = WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND;
    policy->bandwidth_audio_enabled           = false;
    policy->bandwidth_audio_bitrate           = 0;
    wd_stream_policy_rebuild_bandwidth_plan_locked(policy, WD_BANDWIDTH_MODE_TILES);
    policy->link_good_seconds                 = 0;
    policy->link_loss_seconds                 = 0;
    policy->multipacket_loss_cooldown_seconds = 0;
    policy->client_render_pressure_seconds    = 0;
    policy->client_render_visible             = true;
    wd_stream_policy_reset_tokens(policy);
}

void wd_stream_policy_apply_client_hello(struct wd_stream_policy* policy, const struct wd_client_hello_payload* hello) {
    if (!policy || !hello)
    {
        return;
    }

    const uint16_t fps = wd_frame_rate_normalize_client_request(hello->requested_capture_fps);

    policy->requested_capture_fps = fps;
    policy->adaptive_capture_fps  = fps;
    policy->stream_mode           = WD_STREAM_MODE_TILES;
    policy->video_mode            = hello->video_mode <= WD_VIDEO_MODE_FORCE ? hello->video_mode : WD_VIDEO_MODE_AUTO;
    policy->video_min_dirty_percent =
        hello->video_min_dirty_percent != 0 ? hello->video_min_dirty_percent : WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT;
    if (policy->video_min_dirty_percent > WD_VIDEO_MIN_DIRTY_PERCENT_MAX)
    {
        policy->video_min_dirty_percent = WD_VIDEO_MIN_DIRTY_PERCENT_MAX;
    }
    policy->video_enter_seconds = hello->video_enter_seconds != 0 ? hello->video_enter_seconds : WD_VIDEO_ENTER_SECONDS_DEFAULT;
    if (policy->video_enter_seconds > WD_VIDEO_ENTER_SECONDS_MAX)
    {
        policy->video_enter_seconds = WD_VIDEO_ENTER_SECONDS_MAX;
    }
    policy->video_exit_dirty_percent =
        hello->video_exit_dirty_percent != 0 ? hello->video_exit_dirty_percent : WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT;
    if (policy->video_exit_dirty_percent > WD_VIDEO_EXIT_DIRTY_PERCENT_MAX)
    {
        policy->video_exit_dirty_percent = WD_VIDEO_EXIT_DIRTY_PERCENT_MAX;
    }
    policy->video_exit_seconds = hello->video_exit_seconds != 0 ? hello->video_exit_seconds : WD_VIDEO_EXIT_SECONDS_DEFAULT;
    if (policy->video_exit_seconds > WD_VIDEO_EXIT_SECONDS_MAX)
    {
        policy->video_exit_seconds = WD_VIDEO_EXIT_SECONDS_MAX;
    }
    policy->video_bitrate_kib_per_second      = hello->video_bitrate_kib_per_second;
    policy->video_candidate_seconds           = 0;
    policy->tile_recovery_seconds             = 0;
    policy->video_client_failure_seconds      = 0;
    policy->tile_refresh_pending              = false;
    policy->tile_recovery_refresh_started     = false;
    policy->tile_recovery_refresh_sent        = false;
    policy->tile_recovery_wait_seconds        = 0;
    policy->video_retry_cooldown_seconds      = 0;
    policy->video_bootstrap_pending         = false;
    policy->video_bootstrap_refresh_started = false;
    policy->video_bootstrap_refresh_sent    = false;
    policy->video_bootstrap_wait_seconds    = 0;
    policy->video_bootstrap_content_epoch   = 0;
    policy->tile_recovery_content_epoch     = 0;
    policy->video_recovery_class            = WD_VIDEO_RECOVERY_NONE;
    policy->frame_rate_good_seconds           = 0;
    policy->link_good_seconds                 = 0;
    policy->link_loss_seconds                 = 0;
    policy->multipacket_loss_cooldown_seconds = 0;
    policy->client_render_pressure_seconds    = 0;
    policy->client_render_visible             = true;
    if (policy->safe_link_bytes_per_second == 0)
    {
        policy->safe_link_bytes_per_second = WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND;
    }
    if (policy->recent_link_bytes_per_second == 0)
    {
        policy->recent_link_bytes_per_second = policy->safe_link_bytes_per_second;
    }

    const uint64_t requested_link_cap = wd_stream_link_rate_from_kib(hello->udp_rate_cap_kib_per_second);
    if (requested_link_cap != 0)
    {
        if (policy->safe_link_bytes_per_second > requested_link_cap)
        {
            policy->safe_link_bytes_per_second = requested_link_cap;
        }
        if (policy->recent_link_bytes_per_second > requested_link_cap)
        {
            policy->recent_link_bytes_per_second = requested_link_cap;
        }
    }
    wd_stream_policy_rebuild_bandwidth_plan_locked(policy, WD_BANDWIDTH_MODE_TILES);

    policy->multipacket_loss_cooldown_seconds = 0;

    wd_stream_policy_reset_tokens(policy);
}

void wd_stream_policy_begin_session(struct wd_stream_policy* policy, const struct wd_client_hello_payload* hello,
                                    uint64_t bootstrap_content_epoch) {
    if (!policy || !hello || bootstrap_content_epoch == 0)
    {
        return;
    }
    wd_stream_policy_apply_client_hello(policy, hello);
    policy->tile_refresh_pending            = true;
    policy->video_bootstrap_pending         = true;
    policy->video_bootstrap_refresh_started = false;
    policy->video_bootstrap_refresh_sent    = false;
    policy->video_bootstrap_wait_seconds    = 0;
    policy->video_bootstrap_content_epoch   = bootstrap_content_epoch;
    policy->tile_recovery_content_epoch     = 0;
    policy->video_recovery_class            = WD_VIDEO_RECOVERY_NONE;
}

const char* wd_stream_mode_name(enum wd_stream_mode mode) {
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

bool wd_stream_mode_video_owns_display(enum wd_stream_mode mode) {
    return mode == WD_STREAM_MODE_VIDEO_ACTIVE;
}

const char* wd_stream_mode_owner_name(enum wd_stream_mode mode) {
    return wd_stream_mode_video_owns_display(mode) ? "video" : "tiles";
}

static void wd_stream_policy_restore_requested_capture_fps_locked(struct wd_stream_policy* policy, const char* reason) {
    if (!policy)
    {
        return;
    }

    (void)reason;

    if (policy->requested_capture_fps == 0)
    {
        policy->requested_capture_fps = WD_DEFAULT_CAPTURE_FPS;
    }

    uint16_t old_fps = policy->adaptive_capture_fps != 0 ? policy->adaptive_capture_fps : policy->requested_capture_fps;
    if (old_fps == policy->requested_capture_fps)
    {
        policy->adaptive_capture_fps    = policy->requested_capture_fps;
        policy->frame_rate_good_seconds = 0;
        return;
    }

    policy->adaptive_capture_fps           = policy->requested_capture_fps;
    policy->frame_rate_good_seconds        = 0;
    policy->client_render_pressure_seconds = 0;
    wd_frame_pacing_reset(&policy->frame_pacing);
    policy->last_video_frame_send_ns       = 0;

    WD_LOG_DEBUG("stream capture rate reset: %u -> %u fps due to %s", (unsigned)old_fps, (unsigned)policy->requested_capture_fps,
                 reason ? reason : "stream mode change");
}

void wd_stream_advance_content_epoch_locked(struct wd_server* server, const char* reason) {
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
    WD_LOG_DEBUG("stream content epoch: epoch=%llu reason=%s", (unsigned long long)net->content_epoch,
                 reason ? reason : "ownership transition");
}

void wd_stream_policy_set_mode_locked(struct wd_stream_policy* policy, enum wd_stream_mode mode,
                                             enum wd_video_recovery_class recovery_class, const char* reason,
                                             double dirty_avg_pct, double dirty_peak_pct, double budget_pressure_pct,
                                             bool video_channel_connected, bool video_encoder_available) {
    if (!policy || policy->stream_mode == mode)
    {
        return;
    }

    (void)reason;
    (void)dirty_avg_pct;
    (void)dirty_peak_pct;
    (void)budget_pressure_pct;
    (void)video_channel_connected;
    (void)video_encoder_available;

    enum wd_stream_mode old_mode = policy->stream_mode;
    const enum wd_bandwidth_mode old_bandwidth_mode =
        wd_stream_mode_uses_video_frames(old_mode) ? WD_BANDWIDTH_MODE_VIDEO : WD_BANDWIDTH_MODE_TILES;
    const enum wd_bandwidth_mode new_bandwidth_mode =
        wd_stream_mode_uses_video_frames(mode) ? WD_BANDWIDTH_MODE_VIDEO : WD_BANDWIDTH_MODE_TILES;
    policy->stream_mode = mode;
    wd_stream_policy_rebuild_bandwidth_plan_locked(policy, new_bandwidth_mode);
    if (old_bandwidth_mode != new_bandwidth_mode)
    {
        policy->link_good_seconds = 0;
        policy->link_loss_seconds = 0;
        policy->frame_rate_good_seconds = 0;
        policy->client_render_pressure_seconds = 0;
        wd_stream_policy_reset_tokens(policy);
        WD_LOG_INFO("bandwidth plan reset: mode=%s link=%llu KiB/s fresh=%llu KiB/s repair=%llu KiB/s "
                    "video=%llu KiB/s control=%llu KiB/s audio_need=%llu KiB/s overhead=%llu KiB/s",
                    new_bandwidth_mode == WD_BANDWIDTH_MODE_VIDEO ? "video" : "tiles",
                    (unsigned long long)(policy->recent_link_bytes_per_second / 1024ull),
                    (unsigned long long)(policy->adaptive_tile_fresh_bytes_per_second / 1024ull),
                    (unsigned long long)(policy->tile_repair_bytes_per_second / 1024ull),
                    (unsigned long long)(policy->video_bytes_per_second / 1024ull),
                    (unsigned long long)(policy->control_bytes_per_second / 1024ull),
                    (unsigned long long)(policy->audio_reserved_bytes_per_second / 1024ull),
                    (unsigned long long)(policy->overhead_bytes_per_second / 1024ull));
    }

    if (mode == WD_STREAM_MODE_TILE_RECOVERY && old_mode != WD_STREAM_MODE_TILE_RECOVERY)
    {
        policy->tile_refresh_pending          = true;
        policy->tile_recovery_refresh_started = false;
        policy->tile_recovery_refresh_sent    = false;
        policy->tile_recovery_wait_seconds    = 0;
        policy->video_client_failure_seconds  = 0;
        policy->tile_recovery_content_epoch    = 0;
        policy->video_recovery_class           = recovery_class;
    }
    else if (mode != WD_STREAM_MODE_TILE_RECOVERY)
    {
        policy->tile_recovery_refresh_started = false;
        policy->tile_recovery_refresh_sent    = false;
        policy->tile_recovery_wait_seconds    = 0;
    }

    if (wd_stream_mode_video_owns_display(old_mode) && !wd_stream_mode_video_owns_display(mode))
    {
        policy->tile_refresh_pending = true;
    }

    if (wd_stream_mode_uses_video_frames(mode) && !wd_stream_mode_uses_video_frames(old_mode))
    {
        policy->video_recovery_class = WD_VIDEO_RECOVERY_NONE;
        wd_stream_policy_restore_requested_capture_fps_locked(policy, "video mode entry");
    }

    WD_LOG_DEBUG("stream mode state: %s -> %s reason=%s dirty_avg_pct=%.1f dirty_peak_pct=%.1f budget_pressure_pct=%.1f video_channel=%s "
                 "video_encoder=%s bootstrap=%s bootstrap_epoch=%llu recovery_class=%u recovery_epoch=%llu recovery_wait=%u retry_cooldown=%u",
                 wd_stream_mode_name(old_mode), wd_stream_mode_name(mode), reason ? reason : "unspecified", dirty_avg_pct, dirty_peak_pct,
                 budget_pressure_pct, video_channel_connected ? "yes" : "no", video_encoder_available ? "yes" : "no",
                 policy->video_bootstrap_pending ? "pending" : "complete",
                 (unsigned long long)policy->video_bootstrap_content_epoch, (unsigned)policy->video_recovery_class,
                 (unsigned long long)policy->tile_recovery_content_epoch, policy->tile_recovery_wait_seconds,
                 policy->video_retry_cooldown_seconds);
}

void wd_stream_policy_update_mode_locked(struct wd_stream_policy* policy, const struct wd_stats* stats, uint16_t total_tiles,
                                                bool video_negotiated, bool video_channel_connected, bool video_encoder_available) {
    if (!policy || !stats || total_tiles == 0)
    {
        return;
    }

    (void)total_tiles;

    if (policy->stream_mode == WD_STREAM_MODE_TILE_RECOVERY)
    {
        policy->video_candidate_seconds = 0;
        return;
    }

    if (policy->video_bootstrap_pending)
    {
        policy->video_candidate_seconds = 0;
        const bool bootstrap_presented = policy->video_bootstrap_refresh_sent && policy->video_bootstrap_content_epoch != 0 &&
                                         stats->client_tile_content_epoch_presented >= policy->video_bootstrap_content_epoch;
        if (bootstrap_presented)
        {
            WD_LOG_DEBUG("video selection enabled after bootstrap presentation: epoch=%llu",
                         (unsigned long long)policy->video_bootstrap_content_epoch);
            policy->video_bootstrap_pending         = false;
            policy->video_bootstrap_refresh_started = false;
            policy->video_bootstrap_refresh_sent    = false;
            policy->video_bootstrap_wait_seconds    = 0;
        }
        else if (policy->video_bootstrap_refresh_sent && policy->video_bootstrap_wait_seconds < UINT32_MAX)
        {
            policy->video_bootstrap_wait_seconds++;
        }
        return;
    }

    const bool video_retry_cooldown_active = policy->video_retry_cooldown_seconds != 0;
    if (video_retry_cooldown_active)
    {
        policy->video_retry_cooldown_seconds--;
    }

    const uint64_t sample_count        = stats->stream_mode_frame_samples != 0 ? stats->stream_mode_frame_samples : 1ull;
    const double   dirty_avg_pct       = wd_stream_coverage_pct(stats->stream_mode_dirty_coverage_per_mille_sum / sample_count);
    const double   dirty_peak_pct      = wd_stream_coverage_pct(stats->stream_mode_dirty_coverage_per_mille_peak);
    const double   budget_pressure_pct = ((double)stats->stream_mode_budget_pressure_frames / (double)sample_count) * 100.0;

    const uint8_t min_dirty_pct =
        policy->video_min_dirty_percent != 0 ? policy->video_min_dirty_percent : WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT;
    const uint32_t fallback_wire_per_base =
        WD_BASE_TILE_WIDTH * WD_BASE_TILE_HEIGHT * WD_BYTES_PER_PIXEL + WD_UDP_TILE_HEADER_MAX_SIZE;
    const uint64_t estimated_tile_demand = wd_tile_estimate_demand_bytes_per_second(
        stats->stream_mode_frame_samples, stats->stream_mode_dirty_coverage_per_mille_sum,
        stats->tile_choice_chosen_wire_sum, stats->tile_choice_covered_base_tiles,
        total_tiles, policy->requested_capture_fps, fallback_wire_per_base);
    const struct wd_video_auto_entry_metrics entry_metrics = {
        .frame_samples                 = stats->stream_mode_frame_samples,
        .changed_frame_samples         = stats->stream_mode_changed_frame_samples,
        .dirty_coverage_per_mille_sum  = stats->stream_mode_dirty_coverage_per_mille_sum,
        .dirty_coverage_per_mille_peak = stats->stream_mode_dirty_coverage_per_mille_peak,
        .tile_wire_bytes               = stats->udp_fresh_bytes_sent,
        .estimated_tile_demand_bytes_per_second = estimated_tile_demand,
        .tile_budget_bytes_per_second  = policy->adaptive_tile_fresh_bytes_per_second,
        .send_pressure_events          = stats->udp_send_pressure_drops,
        .requested_capture_fps         = policy->requested_capture_fps,
        .adaptive_capture_fps          = policy->adaptive_capture_fps,
        .minimum_dirty_percent         = min_dirty_pct,
        .selection_suppressed          = stats->stream_mode_full_refresh_samples != 0,
    };
    const struct wd_video_auto_entry_result auto_entry = wd_video_auto_entry_evaluate(&entry_metrics);

    const uint8_t exit_dirty_pct =
        policy->video_exit_dirty_percent != 0 ? policy->video_exit_dirty_percent : WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT;
    const uint16_t exit_seconds = policy->video_exit_seconds != 0 ? policy->video_exit_seconds : WD_VIDEO_EXIT_SECONDS_DEFAULT;

    const bool low_dirty      = stats->stream_mode_frame_samples != 0 && dirty_avg_pct <= (double)exit_dirty_pct;
    const bool video_ready = wd_video_control_allows_entry(policy->video_mode, video_negotiated,
                                                            video_channel_connected, video_encoder_available);
    const bool video_forced   = policy->video_mode == WD_VIDEO_MODE_FORCE;
    const bool video_disabled = policy->video_mode == WD_VIDEO_MODE_OFF;
    const bool video_entry_candidate =
        !video_disabled && wd_video_entry_allowed(policy->video_bootstrap_pending, false, video_retry_cooldown_active ? 1u : 0u, video_forced,
                               (enum wd_video_recovery_class)policy->video_recovery_class) &&
        (video_forced || auto_entry.candidate);

    if (auto_entry.candidate && policy->stream_mode != WD_STREAM_MODE_VIDEO_ACTIVE)
    {
        WD_LOG_DEBUG("video auto candidate: changed_frames=%u%% dirty_avg=%u%% observed_fresh=%u%% predicted_fresh=%u%% send_pressure=%llu",
                     (unsigned)auto_entry.changed_frame_percent, (unsigned)auto_entry.average_dirty_percent,
                     (unsigned)auto_entry.tile_budget_percent, (unsigned)auto_entry.predicted_demand_percent,
                     (unsigned long long)stats->udp_send_pressure_drops);
    }

    if (video_disabled || !video_ready)
    {
        policy->video_candidate_seconds = 0;
        policy->tile_recovery_seconds   = 0;
        if (policy->stream_mode != WD_STREAM_MODE_TILES)
        {
            wd_stream_policy_set_mode_locked(
                policy, wd_stream_mode_video_owns_display(policy->stream_mode) ? WD_STREAM_MODE_TILE_RECOVERY : WD_STREAM_MODE_TILES,
                wd_stream_mode_video_owns_display(policy->stream_mode) ? WD_VIDEO_RECOVERY_FAILURE : WD_VIDEO_RECOVERY_NONE,
                video_disabled ? "video disabled" : "video unavailable", dirty_avg_pct, dirty_peak_pct, budget_pressure_pct,
                video_channel_connected, video_encoder_available);
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

        const uint16_t enter_seconds = policy->video_enter_seconds != 0 ? policy->video_enter_seconds : WD_VIDEO_ENTER_SECONDS_DEFAULT;
        if (video_forced || policy->video_candidate_seconds >= enter_seconds)
        {
            wd_stream_policy_set_mode_locked(policy, WD_STREAM_MODE_VIDEO_READY, WD_VIDEO_RECOVERY_NONE, video_forced ? "video forced" : "sustained tile cost",
                                             dirty_avg_pct, dirty_peak_pct, budget_pressure_pct, video_channel_connected,
                                             video_encoder_available);
        }
        else
        {
            wd_stream_policy_set_mode_locked(policy, WD_STREAM_MODE_VIDEO_CANDIDATE, WD_VIDEO_RECOVERY_NONE, "sustained tile cost observed", dirty_avg_pct,
                                             dirty_peak_pct, budget_pressure_pct, video_channel_connected, video_encoder_available);
        }
        return;
    }

    if (policy->stream_mode == WD_STREAM_MODE_TILES)
    {
        policy->video_candidate_seconds = 0;
        policy->tile_recovery_seconds   = 0;
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
        wd_stream_policy_set_mode_locked(policy, WD_STREAM_MODE_TILE_RECOVERY, WD_VIDEO_RECOVERY_PLANNED, "tile exit criteria stable", dirty_avg_pct, dirty_peak_pct,
                                         budget_pressure_pct, video_channel_connected, video_encoder_available);
        policy->tile_recovery_seconds = 0;
    }
}

void wd_stream_policy_set_link_rate(struct wd_stream_policy* policy, uint64_t bytes_per_second,
                                    bool audio_enabled, uint32_t audio_bitrate_bits_per_second) {
    if (!policy)
    {
        return;
    }

    const uint64_t rate = wd_stream_clamp_link_rate(bytes_per_second);
    policy->safe_link_bytes_per_second = rate;
    policy->recent_link_bytes_per_second = rate;
    policy->bandwidth_audio_enabled = audio_enabled;
    policy->bandwidth_audio_bitrate = audio_enabled ? audio_bitrate_bits_per_second : 0;
    policy->link_good_seconds = 0;
    policy->link_loss_seconds = 0;
    wd_stream_policy_rebuild_bandwidth_plan_locked(policy, WD_BANDWIDTH_MODE_TILES);
    wd_stream_policy_reset_tokens(policy);
}

static uint64_t wd_stream_policy_tile_media_rate_floor(const struct wd_stream_policy* policy) {
    uint64_t floor = policy ? policy->tile_media_floor_bytes_per_second : 0;
    if (floor == 0)
    {
        floor = WD_UDP_RATE_MIN_BYTES_PER_SECOND;
    }
    return wd_stream_clamp_link_rate(floor);
}

static uint64_t wd_stream_policy_tile_media_rate_ceiling(const struct wd_stream_policy* policy) {
    uint64_t ceiling = policy ? policy->tile_media_ceiling_bytes_per_second : 0;
    if (ceiling == 0)
    {
        ceiling = WD_UDP_RATE_MAX_BYTES_PER_SECOND;
    }
    return wd_stream_clamp_link_rate(ceiling);
}

static void wd_stream_policy_set_tile_media_rate_locked(struct wd_stream_policy* policy, uint64_t rate) {
    if (!policy)
    {
        return;
    }

    uint64_t floor   = wd_stream_policy_tile_media_rate_floor(policy);
    uint64_t ceiling = wd_stream_policy_tile_media_rate_ceiling(policy);

    rate = wd_stream_clamp_link_rate(rate);
    if (rate < floor)
    {
        rate = floor;
    }
    if (rate > ceiling)
    {
        rate = ceiling;
    }

    if (rate == policy->tile_media_bytes_per_second)
    {
        return;
    }

    policy->tile_media_bytes_per_second = rate;
    policy->adaptive_tile_fresh_bytes_per_second =
        rate > policy->tile_repair_bytes_per_second ? rate - policy->tile_repair_bytes_per_second : 0;
    wd_bandwidth_bucket_reset(&policy->fresh_tile_bucket);
}

uint16_t wd_stream_policy_effective_fps_locked(const struct wd_stream_policy* policy) {
    uint16_t fps = policy ? policy->adaptive_capture_fps : 0;
    if (fps == 0 && policy)
    {
        fps = policy->requested_capture_fps;
    }
    if (fps == 0)
    {
        fps = WD_DEFAULT_CAPTURE_FPS;
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

uint16_t wd_stream_policy_capture_pacing_fps_locked(const struct wd_stream_policy* policy, uint16_t output_refresh_hz) {
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

static bool wd_stream_client_packet_loss_sample(const struct wd_stats* stats) {
    if (!stats || stats->client_stats_rx == 0 || stats->client_udp_packets_rx < WD_STREAM_CLIENT_COMPLETION_MIN_PACKETS)
    {
        return false;
    }

    return stats->client_completed_packets * 100ull < stats->client_udp_packets_rx * (uint64_t)WD_STREAM_CLIENT_COMPLETION_LOSS_PERCENT;
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
    const uint64_t report_capacity = (uint64_t)effective_fps * (uint64_t)stats->client_stats_rx;
    const uint64_t render_demand =
        stats->stream_mode_changed_frame_samples < report_capacity ? stats->stream_mode_changed_frame_samples : report_capacity;

    if (render_demand != 0 && stats->client_render_frames * 100ull < render_demand * WD_STREAM_CLIENT_RENDER_FPS_PRESSURE_PERCENT)
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
    if (stats &&
        (stats->client_partial_tiles_timed_out != 0 || stats->client_retx_requests_tx != 0 || wd_stream_client_packet_loss_sample(stats)))
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

    (void)pressure_reason;

    if (policy->requested_capture_fps == 0)
    {
        policy->requested_capture_fps = WD_DEFAULT_CAPTURE_FPS;
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
        uint32_t new_fps          = ((uint32_t)old_fps * decrease_percent) / 100u;
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
            wd_frame_pacing_reset(&policy->frame_pacing);
            stats->frame_rate_downshifts++;
            WD_LOG_DEBUG("stream capture rate down: %u -> %u fps due to %s", old_fps, (unsigned)new_fps,
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
        policy->adaptive_capture_fps    = policy->requested_capture_fps;
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
    uint32_t new_fps     = percent_fps > (uint32_t)old_fps ? percent_fps : (uint32_t)old_fps + 1u;
    if (new_fps > policy->requested_capture_fps)
    {
        new_fps = policy->requested_capture_fps;
    }

    if ((uint16_t)new_fps != old_fps)
    {
        policy->adaptive_capture_fps = (uint16_t)new_fps;
        wd_frame_pacing_reset(&policy->frame_pacing);
        stats->frame_rate_upshifts++;
        WD_LOG_DEBUG("stream capture rate up: %u -> %u fps", old_fps, (unsigned)new_fps);
    }
}

static void wd_stream_policy_update_tile_media_rate_locked(struct wd_stream_policy* policy, struct wd_stats* stats, bool rate_pressure) {
    if (!policy || !stats)
    {
        return;
    }

    const bool useful_tile_activity = stats->udp_tiles_sent != 0 || stats->dirty_tiles != 0 || stats->client_tiles_completed != 0;
    uint64_t   old_rate             = wd_stream_clamp_link_rate(policy->tile_media_bytes_per_second);
    uint64_t   new_rate             = old_rate;

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

        wd_stream_policy_set_tile_media_rate_locked(policy, new_rate);
        if (policy->tile_media_bytes_per_second != old_rate)
        {
            stats->rate_decreases++;
            WD_LOG_DEBUG("stream tile-media budget down: %llu -> %llu KiB/s due to UDP send pressure", (unsigned long long)(old_rate / 1024ull),
                         (unsigned long long)(policy->tile_media_bytes_per_second / 1024ull));
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

    wd_stream_policy_set_tile_media_rate_locked(policy, new_rate);
    if (policy->tile_media_bytes_per_second != old_rate)
    {
        stats->rate_increases++;
        WD_LOG_DEBUG("stream tile-media budget up: %llu -> %llu KiB/s", (unsigned long long)(old_rate / 1024ull),
                     (unsigned long long)(policy->tile_media_bytes_per_second / 1024ull));
    }
}

void wd_stream_policy_update_health_locked(struct wd_stream_policy* policy, struct wd_stats* stats) {
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

    if (policy->stream_mode == WD_STREAM_MODE_TILE_RECOVERY)
    {
        const enum wd_tile_recovery_action recovery_action =
            wd_tile_recovery_decide(policy->tile_recovery_refresh_sent, policy->tile_recovery_content_epoch,
                                    stats->client_tile_content_epoch_presented, policy->tile_recovery_wait_seconds,
                                    WD_STREAM_TILE_RECOVERY_TIMEOUT_SECONDS);
        if (recovery_action != WD_TILE_RECOVERY_WAIT)
        {
            policy->video_retry_cooldown_seconds = WD_STREAM_VIDEO_RETRY_COOLDOWN_SECONDS;
            if (recovery_action == WD_TILE_RECOVERY_COMPLETE_TIMEOUT)
            {
                policy->video_recovery_class = WD_VIDEO_RECOVERY_FAILURE;
            }
            wd_stream_policy_set_mode_locked(policy, WD_STREAM_MODE_TILES, WD_VIDEO_RECOVERY_NONE,
                                             recovery_action == WD_TILE_RECOVERY_COMPLETE_PRESENTED ? "client presented recovery tiles"
                                                                                                    : "tile recovery presentation timeout",
                                             0.0, 0.0, 0.0, true, true);
        }
        else if (policy->tile_recovery_refresh_sent && policy->tile_recovery_wait_seconds < UINT32_MAX)
        {
            policy->tile_recovery_wait_seconds++;
        }
        policy->video_client_failure_seconds = 0;
        return;
    }

    const bool send_pressure = stats->udp_send_pressure_drops != 0;
    /*
     * dirty_budget_blocked means the sender had fresh dirty work ready, but
     * the current fresh-tile allocation could not admit the next tile.
     * That is different from socket send pressure: the link may be healthy,
     * but the requested FPS is too high for the available stream budget and
     * frame size.  Treat it as frame-rate pressure so the sender accumulates
     * more byte tokens per output frame instead of visually dribbling partial
     * frame coverage at the nominal FPS.  Keep byte-budget adaptation tied to
     * real UDP send pressure below.
     */
    const uint64_t normal_dirty_budget_blocked = stats->dirty_budget_blocked > stats->dirty_budget_blocked_full_refresh
                                                     ? stats->dirty_budget_blocked - stats->dirty_budget_blocked_full_refresh
                                                     : 0;
    const bool budget_frame_pressure = normal_dirty_budget_blocked != 0 && (stats->dirty_tiles != 0 || stats->udp_fresh_tiles_sent != 0);
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
    const bool client_packet_loss             = wd_stream_client_packet_loss_sample(stats);
    const bool client_tile_repair             = stats->client_partial_tiles_timed_out != 0 || stats->client_retx_requests_tx != 0;

    const bool video_frame_mode = wd_stream_mode_uses_video_frames(policy->stream_mode);

    if (video_frame_mode)
    {
        const struct wd_client_video_health_metrics video_health_metrics = {
            .server_frames_tx           = stats->video_frames_tx,
            .client_reports             = stats->client_stats_rx,
            .client_frames_seen         = stats->client_video_data_frames_rx + stats->client_video_frames_rx,
            .client_frames_decoded      = stats->client_video_frames_decoded,
            .client_frames_presented    = stats->client_video_frames_presented,
            .client_decode_failures     = stats->client_video_decode_failed,
            .client_publish_failures    = stats->client_video_publish_failed,
            .client_need_keyframe_drops = stats->client_video_need_keyframe_drops,
            .client_audio_video_sync_holds    = stats->client_audio_video_sync_holds,
            .client_decode_queue_drops         = stats->client_video_decode_queue_drops,
            .client_audio_video_startup_timeouts = stats->client_audio_video_startup_timeouts,
            .client_audio_video_startup_hold_ms  = stats->client_audio_video_startup_hold_ms,
            .client_audio_playback_state         = stats->client_audio_playback_state,
            .client_queue_depth         = stats->client_video_queue_depth,
            .client_queue_depth_max     = stats->client_video_queue_depth_max,
        };
        const enum wd_client_video_health_class video_health = wd_client_video_health_classify(&video_health_metrics);
        const bool                              client_video_failure =
            video_health == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL || video_health == WD_CLIENT_VIDEO_HEALTH_DECODE_FAILURE;

        if (client_video_failure)
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

        if (policy->video_client_failure_seconds >= WD_STREAM_VIDEO_CLIENT_FAILURE_SECONDS)
        {
            wd_stream_policy_set_mode_locked(policy, WD_STREAM_MODE_TILE_RECOVERY, WD_VIDEO_RECOVERY_FAILURE,
                                             video_health == WD_CLIENT_VIDEO_HEALTH_DECODE_FAILURE
                                                 ? "client video decode failure"
                                                 : "client video presentation pipeline stalled",
                                             0.0, 0.0, 0.0, true, true);
            policy->video_client_failure_seconds = 0;
            return;
        }

        policy->multipacket_loss_cooldown_seconds = 0;
        policy->link_loss_seconds                 = 0;
        policy->link_good_seconds                 = 0;
        policy->client_render_pressure_seconds    = 0;
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
        const bool  stream_frame_pressure = send_pressure || budget_frame_pressure;
        const bool  tile_frame_pressure   = stream_frame_pressure;
        const char* pressure_reason       = NULL;
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

        wd_stream_policy_update_frame_rate_locked(policy, stats, tile_frame_pressure || client_render_pressure, tile_frame_pressure,
                                                  pressure_reason);
    }
    wd_stream_policy_update_tile_media_rate_locked(policy, stats, send_pressure);
}

uint32_t wd_stream_frame_service_interval_ms(struct wd_server* server) {
    if (!server)
    {
        return WD_SERVER_FRAME_SERVICE_MAX_INTERVAL_MS;
    }

    pthread_mutex_lock(&server->net.lock);
    const uint16_t output_refresh_hz = (uint16_t)((server->output_refresh_mhz + 500u) / 1000u);
    const uint16_t fps = wd_stream_policy_capture_pacing_fps_locked(&server->net.stream_policy, output_refresh_hz);
    pthread_mutex_unlock(&server->net.lock);

    return wd_frame_service_interval_ms(fps, WD_SERVER_FRAME_SERVICE_MIN_INTERVAL_MS, WD_SERVER_FRAME_SERVICE_MAX_INTERVAL_MS);
}

bool wd_stream_policy_should_render_now(struct wd_server* server, uint64_t now_ns) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    bool                     client_connected = net->client_connected;
    struct wd_stream_policy* policy           = &net->stream_policy;

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

    const uint16_t output_refresh_hz = (uint16_t)((server->output_refresh_mhz + 500u) / 1000u);
    uint16_t       fps               = wd_stream_policy_capture_pacing_fps_locked(policy, output_refresh_hz);
    if (wd_frame_pacing_due(&policy->frame_pacing, now_ns, fps))
    {
        should = true;
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

    /* Scene damage and framebuffer-shadow ownership belong exclusively to
     * the Wayland/compositor thread. Callers running on other threads request
     * a compositor-side refresh separately. */
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
    server->damage_all_tiles         = true;
    server->damage_tile_count        = 0;
    server->framebuffer_shadow_valid = false;

    if (!wd_stream_video_worker_init(server))
    {
        WD_LOG_ERROR("failed to create video encoder worker");
        wd_stream_destroy(server);
        return false;
    }
    if (!wd_stream_frame_worker_init(server))
    {
        WD_LOG_ERROR("failed to create stream frame worker");
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

    wd_stream_frame_worker_destroy(server);
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
    server->net.dirty_region_count  = 0;
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
    server->damage_tiles      = NULL;
    server->damage_all_tiles  = false;
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

    if (!wd_server_set_tile_size(server, tile_width, tile_height) ||
        !wd_server_set_geometry(server, server->display_width, server->display_height))
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
        !server->net.dirty_queued || !server->net.dirty_queue_enqueued_ns || !server->net.retransmit_queue ||
        !server->net.retransmit_queued || !server->net.retransmit_queue_enqueued_ns || !server->net.retransmit_requested_generation ||
        !server->net.summary_dirty_tiles || !server->net.summary_dirty_queue)
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

    server->net.dirty_queue_read       = 0;
    server->net.dirty_queue_write      = 0;
    server->net.dirty_queue_count      = 0;
    server->net.retransmit_queue_count = 0;
    server->net.summary_dirty_count    = 0;
    server->last_summary_ns            = 0;
    server->last_delta_summary_ns      = 0;
    server->scene_dirty                = true;

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

    uint64_t drops               = net->udp_send_pressure_drops;
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

        net->summary_dirty_tiles[tile_id]                    = true;
        net->summary_dirty_queue[net->summary_dirty_count++] = tile_id;
    }
}

static uint32_t wd_stream_tile_wire_bytes_for_payload(uint32_t payload_size, uint16_t udp_payload_target, uint64_t input_sequence,
                                                      bool compressed_payload) {
    (void)compressed_payload;
    udp_payload_target = wd_tile_normalize_udp_payload_target(udp_payload_target, WD_UDP_PAYLOAD_TARGET, WD_UDP_TILE_PAYLOAD_MAX);
    return wd_tile_wire_bytes_for_payload(payload_size, udp_payload_target, WD_UDP_TILE_HEADER_MIN_SIZE,
                                          input_sequence ? WD_UDP_TILE_HEADER_MAX_SIZE : WD_UDP_TILE_HEADER_MIN_SIZE);
}

static bool wd_stream_use_compressed_tile_payload(uint32_t compressed_size, uint32_t uncompressed_size, uint16_t udp_payload_target,
                                                  uint64_t input_sequence) {
    udp_payload_target = wd_tile_normalize_udp_payload_target(udp_payload_target, WD_UDP_PAYLOAD_TARGET, WD_UDP_TILE_PAYLOAD_MAX);
    return wd_tile_compression_is_worthwhile(compressed_size, uncompressed_size, udp_payload_target, WD_UDP_TILE_HEADER_MIN_SIZE,
                                             input_sequence ? WD_UDP_TILE_HEADER_MAX_SIZE : WD_UDP_TILE_HEADER_MIN_SIZE,
                                             WD_STREAM_TILE_COMPRESSION_MIN_SAVINGS_BYTES, WD_STREAM_TILE_COMPRESSION_MIN_SAVINGS_PERCENT);
}

static void wd_stream_note_tile_choice_locked(struct wd_net_state* net, uint32_t compressed_size, uint32_t uncompressed_size,
                                              uint16_t udp_payload_target, uint64_t input_sequence, bool compressed_payload,
                                              uint16_t tile_width, uint16_t tile_height, uint16_t covered_base_tiles) {
    if (!net)
    {
        return;
    }

    uint32_t compressed_wire   = wd_stream_tile_wire_bytes_for_payload(compressed_size, udp_payload_target, input_sequence, true);
    uint32_t uncompressed_wire = wd_stream_tile_wire_bytes_for_payload(uncompressed_size, udp_payload_target, input_sequence, false);
    uint32_t chosen_wire       = compressed_payload ? compressed_wire : uncompressed_wire;
    uint32_t alternate_wire    = compressed_payload ? uncompressed_wire : compressed_wire;

    if (compressed_payload)
    {
        net->stats.tile_choice_compressed++;
    }
    else
    {
        net->stats.tile_choice_uncompressed++;
    }

    if (tile_width == WD_TILE_SIZE_LARGE_WIDTH && tile_height == WD_TILE_SIZE_LARGE_HEIGHT)
    {
        net->stats.tile_size_128x64_sent++;
    }
    else if (tile_width == WD_TILE_SIZE_MEDIUM_WIDTH && tile_height == WD_TILE_SIZE_MEDIUM_HEIGHT)
    {
        net->stats.tile_size_64x64_sent++;
    }
    else if (tile_width == WD_TILE_SIZE_SMALL_WIDTH && tile_height == WD_TILE_SIZE_SMALL_HEIGHT)
    {
        net->stats.tile_size_32x32_sent++;
    }
    else if (tile_width == WD_TILE_SIZE_BASE_WIDTH && tile_height == WD_TILE_SIZE_BASE_HEIGHT)
    {
        net->stats.tile_size_16x16_sent++;
    }

    net->stats.tile_choice_compressed_payload_sum += compressed_size;
    net->stats.tile_choice_uncompressed_payload_sum += uncompressed_size;
    net->stats.tile_choice_compressed_wire_sum += compressed_wire;
    net->stats.tile_choice_uncompressed_wire_sum += uncompressed_wire;
    net->stats.tile_choice_chosen_wire_sum += chosen_wire;
    net->stats.tile_choice_covered_base_tiles += covered_base_tiles;
    if (alternate_wire > chosen_wire)
    {
        net->stats.tile_choice_saved_wire_sum += alternate_wire - chosen_wire;
    }
}

enum wd_stream_bandwidth_class {
    WD_STREAM_BANDWIDTH_FRESH = 0,
    WD_STREAM_BANDWIDTH_REPAIR,
    WD_STREAM_BANDWIDTH_CONTROL,
};

static struct wd_bandwidth_bucket* wd_stream_bandwidth_bucket(struct wd_stream_policy* policy,
                                                               enum wd_stream_bandwidth_class traffic_class) {
    if (!policy)
    {
        return NULL;
    }
    switch (traffic_class)
    {
    case WD_STREAM_BANDWIDTH_FRESH:
        return &policy->fresh_tile_bucket;
    case WD_STREAM_BANDWIDTH_REPAIR:
        return &policy->repair_bucket;
    case WD_STREAM_BANDWIDTH_CONTROL:
        return &policy->control_bucket;
    default:
        return NULL;
    }
}

static uint64_t wd_stream_bandwidth_rate(const struct wd_stream_policy* policy,
                                         enum wd_stream_bandwidth_class traffic_class) {
    if (!policy)
    {
        return 0;
    }
    switch (traffic_class)
    {
    case WD_STREAM_BANDWIDTH_FRESH:
        return policy->adaptive_tile_fresh_bytes_per_second;
    case WD_STREAM_BANDWIDTH_REPAIR:
        return policy->tile_repair_bytes_per_second;
    case WD_STREAM_BANDWIDTH_CONTROL:
        return policy->control_bytes_per_second;
    default:
        return 0;
    }
}

static uint64_t wd_stream_class_budget_locked(struct wd_stream_policy* policy,
                                               enum wd_stream_bandwidth_class traffic_class,
                                               enum wd_stream_bandwidth_class borrow_class, bool allow_borrow,
                                               uint64_t now_ns) {
    struct wd_bandwidth_bucket* bucket = wd_stream_bandwidth_bucket(policy, traffic_class);
    const uint64_t rate = wd_stream_bandwidth_rate(policy, traffic_class);
    const uint64_t available = wd_bandwidth_bucket_available(bucket, rate, wd_stream_byte_burst_cap_for_rate(rate), now_ns);
    if (!allow_borrow)
    {
        return available;
    }

    struct wd_bandwidth_bucket* borrow_bucket = wd_stream_bandwidth_bucket(policy, borrow_class);
    const uint64_t borrow_rate = wd_stream_bandwidth_rate(policy, borrow_class);
    const uint64_t borrowed = wd_bandwidth_bucket_available(
        borrow_bucket, borrow_rate, wd_stream_byte_burst_cap_for_rate(borrow_rate), now_ns);
    return UINT64_MAX - available < borrowed ? UINT64_MAX : available + borrowed;
}

static void wd_stream_class_consume_locked(struct wd_stream_policy* policy,
                                           enum wd_stream_bandwidth_class traffic_class,
                                           enum wd_stream_bandwidth_class borrow_class, bool allow_borrow,
                                           uint64_t bytes) {
    struct wd_bandwidth_bucket* bucket = wd_stream_bandwidth_bucket(policy, traffic_class);
    const uint64_t consumed = wd_bandwidth_bucket_consume(bucket, bytes);
    if (allow_borrow && consumed < bytes)
    {
        (void)wd_bandwidth_bucket_consume(wd_stream_bandwidth_bucket(policy, borrow_class), bytes - consumed);
    }
}

static void wd_stream_class_refund_locked(struct wd_stream_policy* policy,
                                          enum wd_stream_bandwidth_class traffic_class, uint64_t bytes) {
    const uint64_t rate = wd_stream_bandwidth_rate(policy, traffic_class);
    wd_bandwidth_bucket_refund(wd_stream_bandwidth_bucket(policy, traffic_class), bytes,
                               wd_stream_byte_burst_cap_for_rate(rate));
}

static uint64_t wd_stream_tile_byte_budget_locked(struct wd_net_state* net, bool repair, uint64_t now_ns) {
    if (!net)
    {
        return 0;
    }
    const bool allow_borrow = repair ? net->dirty_queue_count == 0 : net->retransmit_queue_count == 0;
    return wd_stream_class_budget_locked(&net->stream_policy,
                                         repair ? WD_STREAM_BANDWIDTH_REPAIR : WD_STREAM_BANDWIDTH_FRESH,
                                         repair ? WD_STREAM_BANDWIDTH_FRESH : WD_STREAM_BANDWIDTH_REPAIR,
                                         allow_borrow, now_ns);
}

static void wd_stream_consume_tile_bytes_locked(struct wd_net_state* net, bool repair, uint64_t bytes) {
    if (!net || bytes == 0)
    {
        return;
    }
    const bool allow_borrow = repair ? net->dirty_queue_count == 0 : net->retransmit_queue_count == 0;
    wd_stream_class_consume_locked(&net->stream_policy,
                                   repair ? WD_STREAM_BANDWIDTH_REPAIR : WD_STREAM_BANDWIDTH_FRESH,
                                   repair ? WD_STREAM_BANDWIDTH_FRESH : WD_STREAM_BANDWIDTH_REPAIR,
                                   allow_borrow, bytes);
}

bool wd_stream_try_consume_tcp_control_budget_locked(struct wd_net_state* net, uint32_t bytes, uint64_t now_ns) {
    if (!net || bytes == 0)
    {
        return true;
    }

    uint64_t budget = wd_stream_class_budget_locked(&net->stream_policy, WD_STREAM_BANDWIDTH_CONTROL,
                                                       WD_STREAM_BANDWIDTH_CONTROL, false, now_ns);
    if (budget < (uint64_t)bytes)
    {
        net->stats.tcp_budget_blocked++;
        return false;
    }

    wd_stream_class_consume_locked(&net->stream_policy, WD_STREAM_BANDWIDTH_CONTROL,
                                   WD_STREAM_BANDWIDTH_CONTROL, false, bytes);
    net->stats.tcp_control_bytes_sent += bytes;
    return true;
}

void wd_stream_account_tcp_control_bytes_locked(struct wd_net_state* net, uint32_t bytes) {
    if (!net || bytes == 0)
    {
        return;
    }

    wd_stream_class_consume_locked(&net->stream_policy, WD_STREAM_BANDWIDTH_CONTROL,
                                   WD_STREAM_BANDWIDTH_CONTROL, false, bytes);
    net->stats.tcp_control_bytes_sent += bytes;
}

static void wd_stream_refund_tcp_control_budget_locked(struct wd_net_state* net, uint64_t bytes) {
    if (!net || bytes == 0)
    {
        return;
    }

    wd_stream_class_refund_locked(&net->stream_policy, WD_STREAM_BANDWIDTH_CONTROL, bytes);
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
    struct wd_server*               server;
    struct wd_tile_delivery_status  status;
    struct wd_stream_epoch_identity epoch;
    uint64_t                        generation;
    uint64_t                        input_sequence;
    uint64_t                        input_inject_ns;
    uint8_t*                        encoded_payload;
    uint16_t                        covered_base_ids[WD_WIRE_TILE_MAX_BASE_TILES];
    uint16_t                        covered_base_count;
};

static void wd_stream_finish_udp_tile_delivery(struct wd_udp_tile_delivery* delivery, bool failed) {
    if (!delivery)
    {
        return;
    }
    if (delivery->server)
    {
        struct wd_server*                     server  = delivery->server;
        struct wd_net_state*                  net     = &server->net;
        const struct wd_stream_epoch_identity current = {
            .connection_epoch       = net->connection_epoch,
            .config_epoch           = net->config_epoch,
            .content_epoch          = net->content_epoch,
            .framebuffer_generation = server->framebuffer_generation,
        };
        if (wd_stream_epoch_identity_equal(&delivery->epoch, &current))
        {
            if (delivery->input_sequence != 0)
            {
                wd_stream_note_input_delivery_locked(net, delivery->input_sequence, delivery->input_inject_ns, wd_now_ns(), !failed);
            }
            if (failed)
            {
                for (uint16_t i = 0; i < delivery->covered_base_count; ++i)
                {
                    const uint16_t tile_id = delivery->covered_base_ids[i];
                    if (tile_id < server->total_tiles && net->tiles && net->tiles[tile_id].generation <= delivery->generation)
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
    bool                         failed   = false;
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
    bool final_failed       = false;
    if (wd_tile_delivery_status_seal(&delivery->status, &final_failed))
    {
        wd_stream_finish_udp_tile_delivery(delivery, final_failed);
    }
}

static bool wd_stream_send_tile_payload_sized_locked(struct wd_server* server, uint16_t tile_id, uint16_t tile_width, uint16_t tile_height,
                                                     uint64_t generation, uint64_t input_sequence, uint8_t** tile_payload_io,
                                                     uint32_t tile_payload_size, bool compressed_payload,
                                                     struct wd_udp_tile_send_result* result) {
    struct wd_net_state* net          = &server->net;
    uint8_t*             tile_payload = tile_payload_io ? *tile_payload_io : NULL;

    wd_stream_init_send_result(result);

    const uint16_t packet_tiles_x     = wd_tiles_for_width_with_tile(server->display_width, tile_width);
    const uint16_t packet_tiles_y     = wd_tiles_for_height_with_tile(server->display_height, tile_height);
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

    struct wd_udp_tile_delivery* delivery = calloc(1, sizeof(*delivery));
    if (!delivery)
    {
        return false;
    }
    delivery->server                       = server;
    delivery->epoch.connection_epoch       = net->connection_epoch;
    delivery->epoch.config_epoch           = net->config_epoch;
    delivery->epoch.content_epoch          = net->content_epoch;
    delivery->epoch.framebuffer_generation = server->framebuffer_generation;
    delivery->generation                   = generation;
    if (!wd_stream_collect_wire_tile_base_ids(server, tile_id, tile_width, tile_height, delivery->covered_base_ids,
                                              &delivery->covered_base_count,
                                              (uint16_t)(sizeof(delivery->covered_base_ids) / sizeof(delivery->covered_base_ids[0]))))
    {
        free(delivery);
        return false;
    }
    delivery->encoded_payload = tile_payload;
    if (tile_payload_io)
    {
        *tile_payload_io = NULL;
    }

    const uint16_t udp_payload_target =
        wd_tile_normalize_udp_payload_target(net->udp_payload_target, WD_UDP_PAYLOAD_TARGET, WD_UDP_TILE_PAYLOAD_MAX);
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
        delivery->input_sequence                 = input_sequence;
        delivery->input_inject_ns                = net->last_input_inject_ns;
        net->input_correlation_inflight_sequence = input_sequence;
    }
    const uint64_t udp_send_start_ns  = wd_now_ns();
    uint16_t       packets_sent       = 0;
    bool           fatal_send_failure = false;

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
        header.session_id       = net->session_id;
        header.connection_token = net->connection_token;
        header.content_epoch    = net->content_epoch;
        header.flags            = compressed_payload ? WD_UDP_TILE_FLAG_COMPRESSED : 0;
        if (packet_id == 0 && input_sequence != 0)
        {
            header.flags |= WD_UDP_TILE_FLAG_INPUT_SEQUENCE;
            header.input_sequence = input_sequence;
        }
        header.tile_size         = tile_size;
        header.tile_pkt_id       = (uint8_t)packet_id;
        header.tile_id           = tile_id;
        header.tile_pkt_count    = (uint8_t)packet_count;
        header.payload_size      = payload_size;
        header.tile_payload_size = (uint16_t)tile_payload_size;
        header.tile_generation   = generation;
        header.header_size       = wd_udp_tile_header_size_for_flags(header.flags);
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
        const enum wd_async_udp_send_status async_status = wd_async_udp_send_packet(
            net->udp_tx, net->udp_fd, &net->client_udp_addr, header_buf, header_size, (uint8_t*)tile_payload + offset, payload_size,
            delivery ? wd_stream_udp_tile_packet_completion : NULL, delivery);
        const bool async_queued = async_status == WD_ASYNC_UDP_SEND_QUEUED;
        if (!async_queued && delivery)
        {
            bool ignored_failed = false;
            (void)wd_tile_delivery_status_complete(&delivery->status, async_status == WD_ASYNC_UDP_SEND_FAILED, &ignored_failed);
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
            net->stats.udp_async_send_failed++;
            fatal_send_failure = true;
            break;
        }

        packets_sent++;
        if (result)
        {
            result->any_packet_sent = true;
            result->packets_sent    = packets_sent;
            result->bytes_sent += packet_wire_size;
        }

        net->stats.udp_packets_sent++;
        net->stats.udp_bytes_sent += (uint64_t)packet_wire_size;
    }

    const bool delivery_incomplete = fatal_send_failure || packets_sent != packet_count;
    wd_stream_seal_udp_tile_delivery(delivery, delivery_incomplete);
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
    net->retransmit_queued[tile_id]                      = true;
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

/* Consume the compositor-owned damage snapshot without touching the live
 * damage accumulator. The compositor may record the next frame while this
 * worker processes the previous one. */
static uint16_t wd_stream_take_video_damage_sample_locked(const struct wd_server* server,
                                                          const struct wd_stream_damage_view* damage) {
    if (!server || !damage)
    {
        return 0;
    }

    uint32_t dirty_tiles = 0;
    if (damage->all_tiles || !damage->tiles)
    {
        dirty_tiles = server->total_tiles;
    }
    else
    {
        dirty_tiles = damage->tile_count;
        if (dirty_tiles > server->total_tiles)
        {
            dirty_tiles = server->total_tiles;
        }
    }

    return (uint16_t)dirty_tiles;
}

static bool wd_stream_has_queued_tile_work_locked(const struct wd_server* server) {
    if (!server)
    {
        return false;
    }

    const struct wd_net_state* net = &server->net;
    return net->dirty_queue_count != 0 || net->dirty_region_count != 0 || net->retransmit_queue_count != 0 || net->summary_dirty_count != 0;
}

static bool wd_stream_has_queued_framebuffer_work_locked(const struct wd_server* server) {
    if (!server)
    {
        return false;
    }

    const struct wd_net_state* net = &server->net;
    return net->dirty_queue_count != 0 || net->dirty_region_count != 0 || net->retransmit_queue_count != 0;
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
    net->dirty_queue_read  = 0;
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

static bool wd_framebuffer_tile_changed_and_update_shadow(struct wd_server* server, uint16_t tile_id, bool force_changed) {
    if (!server || !server->framebuffer_xrgb8888 || !server->framebuffer_shadow_xrgb8888 || tile_id >= server->total_base_tiles ||
        server->base_tiles_x == 0)
    {
        return true;
    }

    const uint32_t tile_x = (uint32_t)(tile_id % server->base_tiles_x) * server->base_tile_width;
    const uint32_t tile_y = (uint32_t)(tile_id / server->base_tiles_x) * server->base_tile_height;
    if (tile_x >= server->display_width || tile_y >= server->display_height)
    {
        return false;
    }

    uint32_t width  = server->base_tile_width;
    uint32_t height = server->base_tile_height;
    if (tile_x + width > server->display_width)
    {
        width = server->display_width - tile_x;
    }
    if (tile_y + height > server->display_height)
    {
        height = server->display_height - tile_y;
    }

    bool changed = force_changed;
    if (!changed)
    {
        for (uint32_t row = 0; row < height; ++row)
        {
            const uint32_t* current = server->framebuffer_xrgb8888 + (size_t)(tile_y + row) * server->display_width + tile_x;
            const uint32_t* shadow  = server->framebuffer_shadow_xrgb8888 + (size_t)(tile_y + row) * server->display_width + tile_x;
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
            const uint32_t* current = server->framebuffer_xrgb8888 + (size_t)(tile_y + row) * server->display_width + tile_x;
            uint32_t*       shadow  = server->framebuffer_shadow_xrgb8888 + (size_t)(tile_y + row) * server->display_width + tile_x;
            memcpy(shadow, current, (size_t)width * sizeof(*current));
        }
    }

    return changed;
}

bool wd_stream_frame_force_full_refresh(struct wd_server* server) {
    if (!server)
    {
        return false;
    }

    pthread_mutex_lock(&server->net.lock);
    const bool force = server->net.stream_policy.tile_refresh_pending ||
                       (server->net.stream_policy.stream_mode == WD_STREAM_MODE_TILE_RECOVERY &&
                        !server->net.stream_policy.tile_recovery_refresh_started);
    pthread_mutex_unlock(&server->net.lock);
    return force;
}

bool wd_stream_analyze_frame(struct wd_server* server, const struct wd_stream_damage_view* damage, bool force_full_refresh,
                             bool* changed_tiles, uint32_t changed_capacity, struct wd_stream_frame_analysis* analysis) {
    if (!server || !changed_tiles || !analysis || changed_capacity < server->total_base_tiles)
    {
        return false;
    }

    memset(analysis, 0, sizeof(*analysis));
    memset(changed_tiles, 0, (size_t)server->total_base_tiles * sizeof(*changed_tiles));
    const uint64_t diff_start_ns = wd_now_ns();
    const uint32_t limit = server->total_base_tiles < server->total_tiles ? server->total_base_tiles : server->total_tiles;
    const bool shadow_valid = server->framebuffer_shadow_valid && !force_full_refresh;
    const bool full_candidate_pass = !shadow_valid || !damage || damage->all_tiles || !damage->tiles;

    for (uint32_t tile_id = 0; tile_id < limit; ++tile_id)
    {
        if (!full_candidate_pass && !damage->tiles[tile_id])
        {
            continue;
        }

        analysis->candidate_count++;
        if (wd_framebuffer_tile_changed_and_update_shadow(server, (uint16_t)tile_id, !shadow_valid))
        {
            changed_tiles[tile_id] = true;
            analysis->changed_tile_count++;
        }
        else
        {
            analysis->unchanged_count++;
        }
    }

    if (server->framebuffer_shadow_xrgb8888)
    {
        server->framebuffer_shadow_valid = true;
    }
    analysis->changed_tiles = changed_tiles;
    analysis->full_refresh  = !shadow_valid;
    analysis->diff_ns       = wd_now_ns() - diff_start_ns;
    return true;
}

static uint16_t wd_stream_apply_frame_analysis_locked(struct wd_server* server,
                                                       const struct wd_stream_frame_analysis* analysis) {
    if (!server || !analysis || !analysis->changed_tiles)
    {
        return 0;
    }

    struct wd_net_state* net = &server->net;
    net->stats.framebuffer_diff_candidates += analysis->candidate_count;
    net->stats.framebuffer_diff_changed += analysis->changed_tile_count;
    net->stats.framebuffer_diff_unchanged += analysis->unchanged_count;
    net->stats.framebuffer_diff_ns += analysis->diff_ns;
    if (analysis->full_refresh)
    {
        net->stats.framebuffer_diff_full_refreshes++;
    }

    const uint32_t limit = server->total_base_tiles < server->total_tiles ? server->total_base_tiles : server->total_tiles;
    for (uint32_t tile_id = 0; tile_id < limit; ++tile_id)
    {
        if (analysis->changed_tiles[tile_id])
        {
            wd_detect_one_dirty_tile_into_queue_locked(server, (uint16_t)tile_id);
        }
    }
    return analysis->changed_tile_count;
}

static void wd_stream_note_mode_frame_locked(struct wd_net_state* net, uint16_t dirty_tiles, uint16_t pending_tiles, uint32_t total_tiles,
                                             bool budget_pressure, bool full_refresh) {
    if (!net)
    {
        return;
    }

    if (net->stream_policy.video_bootstrap_pending)
    {
        net->stats.stream_mode_bootstrap_suppressed_samples++;
        return;
    }

    if (full_refresh)
    {
        net->stats.stream_mode_full_refresh_samples++;
        if (budget_pressure)
        {
            net->stats.stream_mode_full_refresh_budget_pressure_frames++;
        }
        return;
    }

    const uint64_t dirty_coverage   = wd_stream_coverage_per_mille(dirty_tiles, total_tiles);
    const uint64_t pending_coverage = wd_stream_coverage_per_mille(pending_tiles, total_tiles);

    net->stats.stream_mode_frame_samples++;
    if (dirty_tiles != 0)
    {
        net->stats.stream_mode_changed_frame_samples++;
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
    }
}

struct wd_wire_tile_candidate {
    uint16_t width;
    uint16_t height;
    uint16_t tile_id;
    uint16_t covered_base_ids[WD_WIRE_TILE_MAX_BASE_TILES];
    uint16_t covered_base_count;
    uint32_t uncompressed_size;
    uint32_t compressed_size;
    uint32_t wire_size;
    bool     compressed_payload;
};

struct wd_parallel_encode_result {
    bool                          valid;
    bool                          budget_blocked;
    uint16_t                      top_region_id;
    struct wd_wire_tile_candidate candidate;
    uint64_t                      covered_dirty_epochs[WD_WIRE_TILE_MAX_BASE_TILES];
    uint8_t*                      payload;
    uint32_t                      payload_size;
    uint64_t                      worker_encode_ns;
    uint64_t                      framebuffer_generation;
};

struct wd_parallel_encode_job {
    struct wd_server*                 server;
    const uint32_t*                   framebuffer_xrgb8888;
    uint32_t                          display_width;
    uint32_t                          display_height;
    uint16_t                          tile_width;
    uint16_t                          tile_height;
    uint16_t                          tiles_x;
    uint16_t                          tiles_y;
    uint16_t                          total_tiles;
    uint16_t                          top_region_id;
    uint64_t                          input_sequence;
    uint64_t                          remaining_byte_budget;
    uint64_t                          framebuffer_generation;
    uint16_t                          udp_payload_target;
    uint8_t                           compression_benchmark_mode;
    bool                              network_happy;
    const bool*                       dirty_snapshot;
    const uint64_t*                   dirty_epoch_snapshot;
    uint64_t                          compression_attempts;
    uint64_t                          compression_wins;
    uint64_t                          compression_entropy_skips;
    uint64_t                          compression_adaptive_skips;
    uint64_t                          compression_nonwins;
    uint64_t                          compression_forced_choices;
    uint64_t                          compression_ns;
    uint64_t                          compression_saved_wire_bytes;
    struct wd_parallel_encode_result* result;
    uint16_t                          result_capacity;
    uint16_t                          result_count;
};

struct wd_encoder_worker_state {
    struct wd_encoder_pool*            pool;
    uint16_t                           worker_index;
    pthread_t                          thread;
    uint8_t*                           tile_bytes;
    uint8_t*                           compressed_tile;
    size_t                             compressed_capacity;
    struct wd_zstd_compressor*         compressor;
    struct wd_tile_compression_advisor compression_advisors[WD_SUPPORTED_TILE_SIZE_COUNT];
};

struct wd_parallel_encode_batch {
    struct wd_parallel_encode_job* jobs;
    uint16_t                       job_count;
    uint16_t                       next_job;
    uint16_t                       completed_jobs;
    uint64_t                       worker_encode_ns;
    bool                           active;
};

struct wd_encoder_pool {
    pthread_mutex_t                  lock;
    pthread_cond_t                   work_cond;
    pthread_cond_t                   done_cond;
    bool                             running;
    uint16_t                         thread_count;
    struct wd_parallel_encode_batch* batch;
    struct wd_encoder_worker_state   workers[WD_STREAM_ENCODER_MAX_THREADS];
};

struct wd_encode_workspace {
    uint16_t                          tile_capacity;
    uint16_t                          batch_capacity;
    bool*                             tile_snapshot;
    uint64_t*                         epoch_snapshot;
    uint16_t*                         regions;
    struct wd_parallel_encode_job*    jobs;
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

static struct wd_encode_workspace* wd_stream_encode_workspace_ensure(struct wd_server* server, uint16_t tile_capacity,
                                                                     uint16_t batch_capacity) {
    if (!server || tile_capacity == 0 || batch_capacity == 0)
    {
        return NULL;
    }

    struct wd_encode_workspace* workspace = server->net.encode_workspace;
    if (!workspace || workspace->tile_capacity < tile_capacity || workspace->batch_capacity < batch_capacity)
    {
        const uint16_t next_tile_capacity =
            workspace && workspace->tile_capacity > tile_capacity ? workspace->tile_capacity : tile_capacity;
        const uint16_t next_batch_capacity =
            workspace && workspace->batch_capacity > batch_capacity ? workspace->batch_capacity : batch_capacity;
        struct wd_encode_workspace* next = calloc(1, sizeof(*next));
        if (!next)
        {
            return NULL;
        }
        next->tile_capacity  = next_tile_capacity;
        next->batch_capacity = next_batch_capacity;
        next->tile_snapshot  = calloc(next_tile_capacity, sizeof(*next->tile_snapshot));
        next->epoch_snapshot = calloc(next_tile_capacity, sizeof(*next->epoch_snapshot));
        next->regions        = calloc(next_tile_capacity, sizeof(*next->regions));
        next->jobs           = calloc(next_batch_capacity, sizeof(*next->jobs));
        next->results        = calloc((size_t)next_batch_capacity * WD_STREAM_ENCODER_MAX_RESULTS_PER_JOB, sizeof(*next->results));
        if (!next->tile_snapshot || !next->epoch_snapshot || !next->regions || !next->jobs || !next->results)
        {
            wd_stream_encode_workspace_free(next);
            return NULL;
        }
        wd_stream_encode_workspace_free(workspace);
        server->net.encode_workspace = next;
        workspace                    = next;
    }

    memset(workspace->jobs, 0, (size_t)batch_capacity * sizeof(*workspace->jobs));
    memset(workspace->results, 0, (size_t)batch_capacity * WD_STREAM_ENCODER_MAX_RESULTS_PER_JOB * sizeof(*workspace->results));
    return workspace;
}

static void*    wd_stream_encoder_worker_main(void* data);
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
    if (cpu_count > (long)WD_STREAM_ENCODER_MAX_THREADS + (long)WD_STREAM_ENCODER_RESERVED_CPUS)
    {
        return WD_STREAM_ENCODER_MAX_THREADS;
    }
    return (uint16_t)(cpu_count - (long)WD_STREAM_ENCODER_RESERVED_CPUS);
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
    const size_t   compressed_capacity = wd_zstd_compress_bound(max_wire_tile_bytes);
    pool->thread_count                 = wd_stream_encoder_thread_count();
    if (pool->thread_count == 0)
    {
        pool->thread_count = 1;
    }
    pool->running            = true;
    server->net.encoder_pool = pool;

    for (uint16_t i = 0; i < pool->thread_count; ++i)
    {
        pool->workers[i].pool                = pool;
        pool->workers[i].worker_index        = i;
        pool->workers[i].compressed_capacity = compressed_capacity;
        pool->workers[i].tile_bytes          = malloc(max_wire_tile_bytes);
        pool->workers[i].compressed_tile     = malloc(compressed_capacity);
        pool->workers[i].compressor          = wd_zstd_compressor_create();
        if (!pool->workers[i].tile_bytes || !pool->workers[i].compressed_tile || !pool->workers[i].compressor ||
            pthread_create(&pool->workers[i].thread, NULL, wd_stream_encoder_worker_main, &pool->workers[i]) != 0)
        {
            free(pool->workers[i].tile_bytes);
            pool->workers[i].tile_bytes = NULL;
            free(pool->workers[i].compressed_tile);
            pool->workers[i].compressed_tile = NULL;
            wd_zstd_compressor_destroy(pool->workers[i].compressor);
            pool->workers[i].compressor = NULL;
            pool->thread_count          = i;
            wd_stream_encoder_pool_destroy(server);
            return false;
        }
    }

    return true;
}

static bool wd_stream_encoder_pool_run(struct wd_server* server, struct wd_parallel_encode_batch* batch, uint16_t* out_worker_threads) {
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
    batch->next_job         = 0;
    batch->completed_jobs   = 0;
    batch->worker_encode_ns = 0;
    batch->active           = true;
    pool->batch             = batch;
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
                                                 uint16_t tile_height, uint16_t* out_ids, uint16_t* out_count, uint16_t max_count) {
    if (!server || !out_ids || !out_count || tile_width == 0 || tile_height == 0 || server->tile_width == 0 || server->tile_height == 0)
    {
        return false;
    }

    *out_count             = 0;
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

static bool wd_stream_wire_tile_for_pixel(const struct wd_server* server, uint32_t x, uint32_t y, uint16_t tile_width, uint16_t tile_height,
                                          uint16_t* out_tile_id) {
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
    const uint32_t x  = bx * server->tile_width;
    const uint32_t y  = by * server->tile_height;
    return wd_stream_wire_tile_for_pixel(server, x, y, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT, out_region_id);
}

static struct wd_dirty_region_scheduler* wd_stream_dirty_region_scheduler_locked(struct wd_server* server) {
    if (!server)
    {
        return NULL;
    }
    if (!server->net.dirty_region_scheduler)
    {
        const uint16_t regions_x     = wd_tiles_for_width_with_tile(server->display_width, WD_WIRE_TILE_MAX_WIDTH);
        const uint16_t regions_total = wd_total_tiles_for_size_with_tile(server->display_width, server->display_height,
                                                                         WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT);
        server->net.dirty_region_scheduler =
            wd_dirty_region_scheduler_create(regions_total, regions_x, WD_STREAM_DIRTY_REGION_STARVATION_NS);
    }
    return server->net.dirty_region_scheduler;
}

static void wd_stream_sync_dirty_region_count_locked(struct wd_server* server) {
    if (server)
    {
        server->net.dirty_region_count = wd_dirty_region_scheduler_count(server->net.dirty_region_scheduler);
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

    uint16_t ids[WD_WIRE_TILE_MAX_BASE_TILES];
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
                                                          uint64_t remaining_byte_budget, bool network_happy, bool prefer_one_packet) {
    if (!server || !candidate || candidate->wire_size == 0 || candidate->wire_size > remaining_byte_budget)
    {
        return false;
    }

    const uint32_t one_packet_budget = (uint32_t)server->net.udp_payload_target + WD_UDP_TILE_HEADER_MAX_SIZE;
    const bool     is_max_tile       = candidate->width == WD_WIRE_TILE_MAX_WIDTH && candidate->height == WD_WIRE_TILE_MAX_HEIGHT;
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

static bool wd_stream_candidate_allowed_for_job(const struct wd_parallel_encode_job* job, const struct wd_wire_tile_candidate* candidate) {
    if (!job || !candidate || candidate->wire_size == 0 || candidate->wire_size > job->remaining_byte_budget)
    {
        return false;
    }

    const uint32_t one_packet_budget = (uint32_t)job->udp_payload_target + WD_UDP_TILE_HEADER_MAX_SIZE;
    const bool     is_max_tile       = candidate->width == WD_WIRE_TILE_MAX_WIDTH && candidate->height == WD_WIRE_TILE_MAX_HEIGHT;
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

static bool wd_stream_job_collect_wire_tile_base_ids(const struct wd_parallel_encode_job* job, uint16_t tile_id, uint16_t tile_width,
                                                     uint16_t tile_height, uint16_t* out_ids, uint16_t* out_count, uint16_t max_count) {
    if (!job || !out_ids || !out_count || tile_width == 0 || tile_height == 0 || job->tile_width == 0 || job->tile_height == 0)
    {
        return false;
    }

    *out_count             = 0;
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

static bool wd_stream_job_wire_tile_for_pixel(const struct wd_parallel_encode_job* job, uint32_t x, uint32_t y, uint16_t tile_width,
                                              uint16_t tile_height, uint16_t* out_tile_id) {
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

static bool wd_stream_snapshot_region_has_dirty(const struct wd_parallel_encode_job* job, const bool* dirty_snapshot, uint16_t wire_tile_id,
                                                uint16_t tile_width, uint16_t tile_height, uint16_t* out_ids, uint16_t* out_count,
                                                uint16_t max_count) {
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
    if (tile_width == WD_TILE_SIZE_LARGE_WIDTH && tile_height == WD_TILE_SIZE_LARGE_HEIGHT)
    {
        return 0;
    }
    if (tile_width == WD_TILE_SIZE_MEDIUM_WIDTH && tile_height == WD_TILE_SIZE_MEDIUM_HEIGHT)
    {
        return 1;
    }
    if (tile_width == WD_TILE_SIZE_SMALL_WIDTH && tile_height == WD_TILE_SIZE_SMALL_HEIGHT)
    {
        return 2;
    }
    return 3;
}

static bool wd_stream_try_encode_candidate_for_snapshot(struct wd_parallel_encode_job* job, struct wd_encoder_worker_state* worker,
                                                        uint16_t wire_tile_id, uint16_t tile_width, uint16_t tile_height,
                                                        bool allow_compression, uint8_t* tile_bytes, uint8_t* compressed_tile,
                                                        size_t compressed_capacity, struct wd_wire_tile_candidate* out,
                                                        uint64_t* out_epochs) {
    if (!job || !worker || !job->server || !job->framebuffer_xrgb8888 || !tile_bytes || !compressed_tile || !out || !out_epochs)
    {
        return false;
    }

    uint16_t covered_count = 0;
    uint16_t covered_ids[WD_WIRE_TILE_MAX_BASE_TILES];
    if (!wd_stream_job_collect_wire_tile_base_ids(job, wire_tile_id, tile_width, tile_height, covered_ids, &covered_count,
                                                  (uint16_t)(sizeof(covered_ids) / sizeof(covered_ids[0]))))
    {
        return false;
    }

    const uint32_t uncompressed_size = (uint32_t)tile_width * (uint32_t)tile_height * WD_BYTES_PER_PIXEL;
    const uint16_t wire_tiles_x      = wd_tiles_for_width_with_tile(job->display_width, tile_width);
    const uint16_t wire_total_tiles  = wd_total_tiles_for_size_with_tile(job->display_width, job->display_height, tile_width, tile_height);
    if (!wd_extract_tile_xrgb8888_for_tile(job->framebuffer_xrgb8888, job->display_width, job->display_height, wire_tiles_x,
                                           wire_total_tiles, wire_tile_id, tile_width, tile_height, tile_bytes))
    {
        return false;
    }

    uint32_t compressed_size    = 0;
    bool     compressed_payload = false;
    if (allow_compression)
    {
        const uint8_t                       benchmark_mode = job->compression_benchmark_mode;
        const bool                          entropy_ok     = wd_tile_xrgb_payload_may_compress(tile_bytes, uncompressed_size);
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
            const bool     compressed = wd_zstd_compress_with_context(worker->compressor, tile_bytes, uncompressed_size, compressed_tile,
                                                                      compressed_capacity, WD_ZSTD_LEVEL, &compressed_size);
            job->compression_ns += wd_now_ns() - compression_start_ns;
            const bool worthwhile = compressed && wd_stream_use_compressed_tile_payload(compressed_size, uncompressed_size,
                                                                                        job->udp_payload_target, job->input_sequence);
            compressed_payload    = wd_tile_compression_benchmark_choose_compressed(benchmark_mode, compressed, worthwhile);

            if (benchmark_mode == WD_TILE_COMPRESSION_BENCH_AUTO)
            {
                wd_tile_compression_advisor_record(advisor, worthwhile);
            }
            if (worthwhile)
            {
                job->compression_wins++;
                const uint32_t compressed_wire =
                    wd_stream_tile_wire_bytes_for_payload(compressed_size, job->udp_payload_target, job->input_sequence, true);
                const uint32_t uncompressed_wire =
                    wd_stream_tile_wire_bytes_for_payload(uncompressed_size, job->udp_payload_target, job->input_sequence, false);
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
    out->width   = tile_width;
    out->height  = tile_height;
    out->tile_id = wire_tile_id;
    memcpy(out->covered_base_ids, covered_ids, (size_t)covered_count * sizeof(covered_ids[0]));
    out->covered_base_count = covered_count;
    out->uncompressed_size  = uncompressed_size;
    out->compressed_size    = compressed_size;
    out->wire_size = wd_stream_tile_wire_bytes_for_payload(payload_size, job->udp_payload_target, job->input_sequence, compressed_payload);
    out->compressed_payload = compressed_payload;
    for (uint16_t i = 0; i < covered_count; ++i)
    {
        out_epochs[i] = job->dirty_epoch_snapshot ? job->dirty_epoch_snapshot[covered_ids[i]] : 0;
    }
    return true;
}

static bool wd_stream_append_snapshot_result(struct wd_parallel_encode_job* job, const struct wd_wire_tile_candidate* candidate,
                                             const uint64_t* covered_epochs, const uint8_t* tile_bytes, const uint8_t* compressed_tile) {
    if (!job || !candidate || !covered_epochs || !tile_bytes || !compressed_tile || !job->result ||
        job->result_count >= job->result_capacity)
    {
        return false;
    }

    const uint8_t* payload                   = candidate->compressed_payload ? compressed_tile : tile_bytes;
    const uint32_t payload_size              = candidate->compressed_payload ? candidate->compressed_size : candidate->uncompressed_size;
    struct wd_parallel_encode_result* result = &job->result[job->result_count];
    memset(result, 0, sizeof(*result));
    result->top_region_id          = job->top_region_id;
    result->framebuffer_generation = job->framebuffer_generation;
    result->payload                = malloc(payload_size);
    if (!result->payload)
    {
        return false;
    }

    memcpy(result->payload, payload, payload_size);
    result->payload_size = payload_size;
    result->candidate    = *candidate;
    memcpy(result->covered_dirty_epochs, covered_epochs, (size_t)candidate->covered_base_count * sizeof(covered_epochs[0]));
    result->valid = true;
    job->result_count++;
    return true;
}

static bool wd_stream_encode_region_recursive_snapshot(struct wd_parallel_encode_job* job, struct wd_encoder_worker_state* worker,
                                                       uint16_t wire_tile_id, uint16_t tile_width, uint16_t tile_height,
                                                       uint8_t* tile_bytes, uint8_t* compressed_tile, size_t compressed_capacity,
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
    uint16_t covered_ids[WD_WIRE_TILE_MAX_BASE_TILES];
    if (!wd_stream_snapshot_region_has_dirty(job, job->dirty_snapshot, wire_tile_id, tile_width, tile_height, covered_ids, &covered_count,
                                             (uint16_t)(sizeof(covered_ids) / sizeof(covered_ids[0]))))
    {
        return false;
    }

    const bool                    is_base_tile      = tile_width == job->tile_width && tile_height == job->tile_height;
    const bool                    allow_compression = true;
    struct wd_wire_tile_candidate candidate;
    uint64_t                      candidate_epochs[WD_WIRE_TILE_MAX_BASE_TILES] = {0};
    memset(&candidate, 0, sizeof(candidate));
    if (wd_stream_try_encode_candidate_for_snapshot(job, worker, wire_tile_id, tile_width, tile_height, allow_compression, tile_bytes,
                                                    compressed_tile, compressed_capacity, &candidate, candidate_epochs) &&
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
    const uint32_t start_x        = wd_tile_start_x_for_tile(wire_tile_id, parent_tiles_x, tile_width);
    const uint32_t start_y        = wd_tile_start_y_for_tile(wire_tile_id, parent_tiles_x, tile_height);
    uint16_t       child_width    = 0;
    uint16_t       child_height   = 0;
    uint16_t       child_count    = 0;
    uint16_t       child_ids[4];

    if (tile_width == WD_WIRE_TILE_MAX_WIDTH && tile_height == WD_WIRE_TILE_MAX_HEIGHT)
    {
        child_width          = WD_TILE_SIZE_MEDIUM_WIDTH;
        child_height         = WD_TILE_SIZE_MEDIUM_HEIGHT;
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
    else if (tile_width == WD_TILE_SIZE_MEDIUM_WIDTH && tile_height == WD_TILE_SIZE_MEDIUM_HEIGHT)
    {
        child_width          = WD_TILE_SIZE_SMALL_WIDTH;
        child_height         = WD_TILE_SIZE_SMALL_HEIGHT;
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
    else if (tile_width == WD_TILE_SIZE_SMALL_WIDTH && tile_height == WD_TILE_SIZE_SMALL_HEIGHT)
    {
        child_width          = job->tile_width;
        child_height         = job->tile_height;
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

    bool any_encoded        = false;
    bool any_budget_blocked = false;
    for (uint16_t i = 0; i < child_count; ++i)
    {
        bool child_budget_blocked = false;
        if (wd_stream_encode_region_recursive_snapshot(job, worker, child_ids[i], child_width, child_height, tile_bytes, compressed_tile,
                                                       compressed_capacity, &child_budget_blocked))
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

static void wd_stream_parallel_encode_one_job(struct wd_parallel_encode_job* job, struct wd_encoder_worker_state* worker) {
    if (!job || !worker || !job->result || !job->server || !worker->tile_bytes || !worker->compressed_tile)
    {
        return;
    }
    for (uint16_t i = 0; i < job->result_capacity; ++i)
    {
        memset(&job->result[i], 0, sizeof(job->result[i]));
        job->result[i].top_region_id          = job->top_region_id;
        job->result[i].framebuffer_generation = job->framebuffer_generation;
    }
    job->result_count = 0;

    const uint64_t start_ns       = wd_now_ns();
    bool           budget_blocked = false;
    if (!wd_stream_encode_region_recursive_snapshot(job, worker, job->top_region_id, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT,
                                                    worker->tile_bytes, worker->compressed_tile, worker->compressed_capacity,
                                                    &budget_blocked))
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
        uint16_t                         index = batch->next_job++;
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

    struct wd_net_state* net       = &server->net;
    uint16_t             out_count = 0;
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
    tile->generation           = generation;
    tile->timestamp_ns         = timestamp_ns;
    tile->input_sequence       = input_sequence;
    return true;
}

static void wd_stream_init_encode_job_locked(struct wd_parallel_encode_job* job, struct wd_server* server, uint16_t top_region_id,
                                             uint64_t input_sequence, uint64_t remaining_byte_budget, bool network_happy,
                                             const bool* dirty_snapshot, const uint64_t* epoch_snapshot,
                                             struct wd_parallel_encode_result* result, uint16_t result_capacity) {
    if (!job || !server)
    {
        return;
    }

    memset(job, 0, sizeof(*job));
    job->server                     = server;
    job->framebuffer_xrgb8888       = server->framebuffer_xrgb8888;
    job->display_width              = server->display_width;
    job->display_height             = server->display_height;
    job->tile_width                 = server->tile_width;
    job->tile_height                = server->tile_height;
    job->tiles_x                    = server->tiles_x;
    job->tiles_y                    = server->tiles_y;
    job->total_tiles                = server->total_tiles;
    job->top_region_id              = top_region_id;
    job->input_sequence             = input_sequence;
    job->remaining_byte_budget      = remaining_byte_budget;
    job->framebuffer_generation     = server->framebuffer_generation;
    job->udp_payload_target         = server->net.udp_payload_target;
    job->compression_benchmark_mode = server->tile_compression_benchmark_mode;
    job->network_happy              = network_happy;
    job->dirty_snapshot             = dirty_snapshot;
    job->dirty_epoch_snapshot       = epoch_snapshot;
    job->result                     = result;
    job->result_capacity            = result_capacity;
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

static uint64_t wd_stream_next_generation_for_result_locked(const struct wd_server*                 server,
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

    uint16_t ids[WD_WIRE_TILE_MAX_BASE_TILES];
    uint16_t count = 0;
    if (wd_stream_collect_wire_tile_base_ids(server, top_region_id, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT, ids, &count,
                                             (uint16_t)(sizeof(ids) / sizeof(ids[0]))) &&
        count > 0)
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

    struct wd_net_state* net            = &server->net;
    uint16_t             worker_threads = 0;
    const uint64_t       wait_start_ns  = wd_now_ns();
    net->encoder_batch_active           = true;
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
        uint64_t token_budget = wd_stream_tile_byte_budget_locked(net, true, now);
        if (token_budget == 0)
        {
            break;
        }

        const uint16_t              workspace_batch_capacity = wd_stream_low_latency_batch_capacity(server, net->retransmit_queue_count);
        struct wd_encode_workspace* workspace = wd_stream_encode_workspace_ensure(server, server->total_tiles, workspace_batch_capacity);
        if (!workspace)
        {
            break;
        }
        bool*     retx_snapshot  = workspace->tile_snapshot;
        uint64_t* epoch_snapshot = workspace->epoch_snapshot;
        uint16_t* regions        = workspace->regions;
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
        const bool     network_happy       = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);

        uint16_t batch_capacity = wd_stream_low_latency_batch_capacity(server, region_count);
        if (batch_capacity == 0)
        {
            break;
        }

        struct wd_parallel_encode_job*    jobs    = workspace->jobs;
        struct wd_parallel_encode_result* results = workspace->results;

        uint16_t job_count = 0;
        for (uint16_t i = 0; i < region_count && job_count < batch_capacity; ++i)
        {
            wd_stream_init_encode_job_locked(
                &jobs[job_count], server, regions[i], retx_input_sequence, token_budget, network_happy, retx_snapshot, epoch_snapshot,
                &results[(size_t)job_count * WD_STREAM_ENCODER_MAX_RESULTS_PER_JOB], (uint16_t)WD_STREAM_ENCODER_MAX_RESULTS_PER_JOB);
            job_count++;
        }

        if (job_count == 0)
        {
            break;
        }

        struct wd_parallel_encode_batch batch;
        memset(&batch, 0, sizeof(batch));
        batch.jobs      = jobs;
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

                const uint64_t send_now              = wd_now_ns();
                uint64_t       current_budget        = wd_stream_tile_byte_budget_locked(net, true, send_now);
                const bool     current_network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
                if (!wd_stream_candidate_allowed_for_region_locked(server, &result->candidate, current_budget, current_network_happy,
                                                                   retx_input_sequence != 0))
                {
                    wd_stream_free_encode_result_payload(result);
                    stop_sending = true;
                    break;
                }

                const uint64_t next_generation = wd_stream_next_generation_for_result_locked(server, result);

                struct wd_udp_tile_send_result send_result;
                if (!wd_stream_send_tile_payload_sized_locked(
                        server, result->candidate.tile_id, result->candidate.width, result->candidate.height, next_generation,
                        retx_input_sequence, &result->payload, result->payload_size, result->candidate.compressed_payload, &send_result))
                {
                    wd_stream_free_encode_result_payload(result);
                    continue;
                }

                if (!send_result.all_packets_sent)
                {
                    if (send_result.any_packet_sent)
                    {
                        wd_stream_consume_tile_bytes_locked(net, true, send_result.bytes_sent);
                    }
                    wd_stream_free_encode_result_payload(result);
                    stop_sending = true;
                    break;
                }

                wd_stream_note_tile_choice_locked(net, result->candidate.compressed_size, result->candidate.uncompressed_size,
                                                  net->udp_payload_target, retx_input_sequence, result->candidate.compressed_payload,
                                                  result->candidate.width, result->candidate.height,
                                                  result->candidate.covered_base_count);

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
                net->stats.udp_retx_bytes_sent += send_result.bytes_sent;
                wd_stream_consume_tile_bytes_locked(net, true, send_result.bytes_sent);
                wd_stream_free_encode_result_payload(result);

                if (send_result.send_blocked)
                {
                    stop_sending = true;
                    break;
                }
            }
        }

        wd_stream_free_encode_result_payloads(results, (uint16_t)(job_count * WD_STREAM_ENCODER_MAX_RESULTS_PER_JOB));
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

static bool wd_stream_send_tiles(struct wd_server* server, bool detect_new_damage,
                                 const struct wd_stream_damage_view* damage,
                                 const struct wd_stream_frame_analysis* analysis,
                                 struct wd_stream_video_snapshot* video_snapshot) {
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

    if (net->stream_policy.tile_refresh_pending ||
        (net->stream_policy.stream_mode == WD_STREAM_MODE_TILE_RECOVERY && !net->stream_policy.tile_recovery_refresh_started))
    {
        if (detect_new_damage && (!analysis || !analysis->full_refresh))
        {
            /* The controller may request recovery after this frame was
             * analyzed. Do not consume that transition with a partial diff;
             * request one compositor-owned full-refresh frame instead. */
            wd_server_request_full_refresh(server);
            pthread_mutex_unlock(&net->lock);
            return true;
        }
        if (!detect_new_damage)
        {
            /* A service-only pass has no captured scene snapshot to turn into
             * the required full refresh. Keep the transition pending and ask
             * the compositor for a frame; only that frame may start recovery. */
            wd_server_request_full_refresh(server);
            pthread_mutex_unlock(&net->lock);
            return true;
        }

        const bool recovering_from_video = net->stream_policy.stream_mode == WD_STREAM_MODE_TILE_RECOVERY;
        const bool bootstrap_refresh = net->stream_policy.video_bootstrap_pending &&
                                       !net->stream_policy.video_bootstrap_refresh_started;
        if (recovering_from_video && net->video_tcp_fd >= 0 && net->video_tx)
        {
            (void)wd_stream_queue_video_control_frame_locked(server, WD_VIDEO_FRAME_END_OF_STREAM);
        }
        wd_stream_collapse_tile_queues_for_video_locked(server);
        wd_stream_invalidate_all_tiles_locked(server);
        /* The frame analysis was prepared before entering the network
         * critical section and already forced every base tile through the
         * stable framebuffer diff for this recovery frame. */
        net->stream_policy.tile_refresh_pending          = false;
        net->stream_policy.tile_recovery_refresh_started = recovering_from_video;
        net->stream_policy.tile_recovery_refresh_sent    = false;
        net->stream_policy.tile_recovery_wait_seconds    = 0;
        if (recovering_from_video)
        {
            net->stream_policy.tile_recovery_content_epoch = net->content_epoch;
        }
        if (bootstrap_refresh)
        {
            net->stream_policy.video_bootstrap_refresh_started = true;
            net->stream_policy.video_bootstrap_refresh_sent    = false;
            net->stream_policy.video_bootstrap_wait_seconds    = 0;
            net->stream_policy.video_bootstrap_content_epoch   = net->content_epoch;
        }
        WD_LOG_INFO("stream mode ownership: owner=tiles refresh=%s epoch=%llu video_eos=%s",
                    recovering_from_video ? "recovery" : (bootstrap_refresh ? "bootstrap" : "full"),
                    (unsigned long long)net->content_epoch, recovering_from_video ? "sent" : "no");
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
        if (net->stream_policy.video_mode != WD_VIDEO_MODE_FORCE)
        {
            const uint16_t dirty_frame_tiles = wd_stream_take_video_damage_sample_locked(server, damage);
            wd_stream_note_mode_frame_locked(net, dirty_frame_tiles, 0, server->total_tiles, false, false);
        }

        net->stats.video_tile_detection_skipped++;
        (void)wd_stream_try_publish_video_snapshot_locked(server, now, video_snapshot);

        /* Queues should normally already be empty after video ownership was
         * established. Only pay the memset cost if transition-era work remains. */
        if (wd_stream_has_queued_tile_work_locked(server))
        {
            wd_stream_collapse_tile_queues_for_video_locked(server);
        }

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
    uint16_t       dirty_frame_tiles                   = 0;
    const uint16_t pending_tiles_at_frame_start        = net->dirty_queue_count;
    const uint64_t dirty_budget_blocked_at_frame_start = net->stats.dirty_budget_blocked;
    const uint64_t full_refresh_at_frame_start         = net->stats.framebuffer_diff_full_refreshes;

    if (detect_new_damage)
    {
        const uint64_t dirty_detect_start_ns = wd_now_ns();
        dirty_frame_tiles                    = wd_stream_apply_frame_analysis_locked(server, analysis);
        net->stats.dirty_detect_ns += wd_now_ns() - dirty_detect_start_ns;
        (void)wd_stream_try_publish_video_snapshot_locked(server, now, video_snapshot);
    }

    const bool client_loss = wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
    if (client_loss && net->retransmit_queue_count > 0)
    {
        wd_stream_send_retransmits_locked(server, now);
    }

    while (net->dirty_region_count > 0)
    {
        const uint64_t remaining_byte_budget = wd_stream_tile_byte_budget_locked(net, false, now);
        const uint64_t tile_input_sequence   = wd_input_correlation_select(net->input_since_last_fresh_tile, net->last_input_sequence,
                                                                           net->input_correlation_inflight_sequence);
        /* Only require enough tokens for the smallest guaranteed-progress
         * fallback. The encoder may produce a highly compressed 128x64 tile;
         * gating on the 32 KiB uncompressed maximum needlessly delays terminal
         * and desktop updates while tokens accumulate. */
        const uint32_t base_uncompressed_bytes = (uint32_t)server->tile_width * (uint32_t)server->tile_height * WD_BYTES_PER_PIXEL;
        const uint32_t minimum_wire_bytes =
            wd_stream_tile_wire_bytes_for_payload(base_uncompressed_bytes, net->udp_payload_target, tile_input_sequence, false);
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

        struct wd_encode_workspace* workspace = wd_stream_encode_workspace_ensure(server, server->total_tiles, batch_capacity);
        if (!workspace)
        {
            break;
        }
        bool*                             dirty_snapshot = workspace->tile_snapshot;
        uint64_t*                         epoch_snapshot = workspace->epoch_snapshot;
        struct wd_parallel_encode_job*    jobs           = workspace->jobs;
        struct wd_parallel_encode_result* results        = workspace->results;

        memcpy(dirty_snapshot, net->dirty_queued, (size_t)server->total_tiles * sizeof(*dirty_snapshot));
        if (net->dirty_epochs)
        {
            memcpy(epoch_snapshot, net->dirty_epochs, (size_t)server->total_tiles * sizeof(*epoch_snapshot));
        }
        else
        {
            memset(epoch_snapshot, 0, (size_t)server->total_tiles * sizeof(*epoch_snapshot));
        }

        const bool     network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
        const uint16_t top_regions_x = wd_tiles_for_width_with_tile(server->display_width, WD_WIRE_TILE_MAX_WIDTH);

        uint16_t job_count = 0;
        while (job_count < batch_capacity && net->dirty_region_count > 0)
        {
            const uint64_t select_start_ns = wd_now_ns();
            const uint64_t select_now_ns   = wd_now_ns();
            (void)top_regions_x;
            uint16_t top_id = 0;
            if (!net->dirty_region_scheduler ||
                !wd_dirty_region_scheduler_take(net->dirty_region_scheduler, net->dirty_region_cursor, select_now_ns, &top_id))
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
            wd_stream_init_encode_job_locked(
                &jobs[job_count], server, top_id, job_input_sequence, remaining_byte_budget, network_happy, dirty_snapshot, epoch_snapshot,
                &results[(size_t)job_count * WD_STREAM_ENCODER_MAX_RESULTS_PER_JOB], (uint16_t)WD_STREAM_ENCODER_MAX_RESULTS_PER_JOB);
            job_count++;
        }

        if (job_count == 0)
        {
            break;
        }

        net->stats.encode_jobs_submitted += job_count;

        struct wd_parallel_encode_batch batch;
        memset(&batch, 0, sizeof(batch));
        batch.jobs      = jobs;
        batch.job_count = job_count;

        const bool encoded = wd_stream_run_encode_batch_locked(server, &batch);
        if (!encoded)
        {
            for (uint16_t ri = 0; ri < job_count; ++ri)
            {
                const uint16_t top_id = jobs[ri].top_region_id;
                if (wd_stream_top_region_still_dirty_locked(server, top_id))
                {
                    uint16_t ids[WD_WIRE_TILE_MAX_BASE_TILES];
                    uint16_t count = 0;
                    if (wd_stream_collect_wire_tile_base_ids(server, top_id, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT, ids, &count,
                                                             (uint16_t)(sizeof(ids) / sizeof(ids[0]))) &&
                        count > 0)
                    {
                        wd_stream_mark_dirty_top_region_locked(server, ids[0]);
                    }
                }
            }
            wd_stream_free_encode_result_payloads(results, (uint16_t)(job_count * WD_STREAM_ENCODER_MAX_RESULTS_PER_JOB));
            break;
        }

        bool     stop_sending           = false;
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

                const uint64_t send_now              = wd_now_ns();
                const uint64_t send_input_sequence   = pending_input_sequence;
                const uint64_t current_budget        = wd_stream_tile_byte_budget_locked(net, false, send_now);
                const bool     current_network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
                if (!wd_stream_candidate_allowed_for_region_locked(server, &result->candidate, current_budget, current_network_happy,
                                                                   send_input_sequence != 0))
                {
                    net->stats.dirty_budget_blocked++;
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    stop_sending = true;
                    break;
                }

                const uint64_t next_generation = wd_stream_next_generation_for_result_locked(server, result);

                struct wd_udp_tile_send_result send_result;
                if (!wd_stream_send_tile_payload_sized_locked(
                        server, result->candidate.tile_id, result->candidate.width, result->candidate.height, next_generation,
                        send_input_sequence, &result->payload, result->payload_size, result->candidate.compressed_payload, &send_result))
                {
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    continue;
                }

                if (!send_result.all_packets_sent)
                {
                    if (send_result.any_packet_sent)
                    {
                        wd_stream_consume_tile_bytes_locked(net, false, send_result.bytes_sent);
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
                                                  net->udp_payload_target, send_input_sequence, result->candidate.compressed_payload,
                                                  result->candidate.width, result->candidate.height,
                                                  result->candidate.covered_base_count);

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
                net->stats.udp_fresh_bytes_sent += send_result.bytes_sent;
                net->stats.encode_jobs_completed++;
                wd_stream_consume_tile_bytes_locked(net, false, send_result.bytes_sent);
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

        wd_stream_free_encode_result_payloads(results, (uint16_t)(job_count * WD_STREAM_ENCODER_MAX_RESULTS_PER_JOB));
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
        const bool full_refresh_this_frame    = net->stats.framebuffer_diff_full_refreshes != full_refresh_at_frame_start;
        if (budget_pressure_this_frame && full_refresh_this_frame)
        {
            const uint64_t blocked_now = net->stats.dirty_budget_blocked - dirty_budget_blocked_at_frame_start;
            net->stats.dirty_budget_blocked_full_refresh += blocked_now;
        }
        wd_stream_note_mode_frame_locked(net, dirty_frame_tiles, pending_tiles_at_frame_start, server->total_tiles,
                                         budget_pressure_this_frame, full_refresh_this_frame);

    }

    if (net->stream_policy.stream_mode == WD_STREAM_MODE_TILE_RECOVERY && net->stream_policy.tile_recovery_refresh_started &&
        !net->stream_policy.tile_recovery_refresh_sent && !wd_stream_has_queued_framebuffer_work_locked(server))
    {
        net->stream_policy.tile_recovery_refresh_sent = true;
        net->stream_policy.tile_recovery_wait_seconds = 0;
        WD_LOG_INFO("tile recovery refresh transmitted; waiting for client presentation epoch=%llu",
                    (unsigned long long)net->stream_policy.tile_recovery_content_epoch);
    }
    if (net->stream_policy.video_bootstrap_pending && net->stream_policy.video_bootstrap_refresh_started &&
        !net->stream_policy.video_bootstrap_refresh_sent && !wd_stream_has_queued_framebuffer_work_locked(server))
    {
        net->stream_policy.video_bootstrap_refresh_sent = true;
        net->stream_policy.video_bootstrap_wait_seconds = 0;
        WD_LOG_INFO("bootstrap refresh transmitted; waiting for client presentation epoch=%llu",
                    (unsigned long long)net->stream_policy.video_bootstrap_content_epoch);
    }

    pthread_mutex_unlock(&net->lock);

    return true;
}

bool wd_stream_process_frame(struct wd_server* server, const struct wd_stream_damage_view* damage,
                             const struct wd_stream_frame_analysis* analysis, struct wd_stream_video_snapshot* video_snapshot) {
    return wd_stream_send_tiles(server, true, damage, analysis, video_snapshot);
}

bool wd_stream_process_queued_work(struct wd_server* server) {
    return wd_stream_send_tiles(server, false, NULL, NULL, NULL);
}

struct wd_summary_completion_entry {
    uint16_t tile_id;
    uint64_t tile_generation;
};

struct wd_summary_completion {
    struct wd_server*                  server;
    bool                               full_summary;
    bool                               async_pending;
    bool                               input_since_last_summary;
    uint64_t                           server_timestamp_ns;
    uint64_t                           last_input_inject_ns;
    uint64_t                           summary_epoch;
    uint64_t                           budget_bytes;
    uint16_t                           entry_count;
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
        struct wd_server*    server = completion->server;
        struct wd_net_state* net    = &server->net;

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
    uint16_t       requested_tile_count   = full_summary ? server->total_tiles : net->summary_dirty_count;
    if (requested_tile_count == 0)
    {
        return true;
    }

    size_t payload_capacity =
        sizeof(struct wd_tile_summary_payload_header) + (size_t)requested_tile_count * sizeof(struct wd_tile_generation_entry);

    uint8_t* payload = malloc(payload_capacity);
    if (!payload)
    {
        return false;
    }

    struct wd_tile_generation_entry* entries = (struct wd_tile_generation_entry*)(payload + sizeof(struct wd_tile_summary_payload_header));

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

    const uint64_t frame_size = (uint64_t)WD_TCP_HEADER_WIRE_SIZE + (uint64_t)payload_size;
    if (frame_size > UINT32_MAX || !wd_stream_try_consume_tcp_control_budget_locked(net, (uint32_t)frame_size, header.server_timestamp_ns))
    {
        free(payload);
        return false;
    }

    struct wd_summary_completion* completion = calloc(1, sizeof(*completion) + (size_t)entry_count * sizeof(completion->entries[0]));
    if (!completion)
    {
        wd_stream_refund_tcp_control_budget_locked(net, frame_size);
        free(payload);
        return false;
    }
    completion->server                   = server;
    completion->full_summary             = full_summary;
    completion->input_since_last_summary = net->input_since_last_summary;
    completion->server_timestamp_ns      = header.server_timestamp_ns;
    completion->last_input_inject_ns     = net->last_input_inject_ns;
    completion->summary_epoch            = net->summary_epoch;
    completion->budget_bytes             = frame_size;
    completion->entry_count              = entry_count;
    for (uint16_t i = 0; i < entry_count; ++i)
    {
        completion->entries[i].tile_id         = entries[i].tile_id;
        completion->entries[i].tile_generation = entries[i].tile_generation;
    }

    const bool ok = wd_async_tcp_send_message_ex(net->control_tx, net->tcp_fd, WD_MSG_TILE_GENERATION_SUMMARY, payload,
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
    else
    {
        net->stats.tcp_async_send_failed++;
        if (net->tcp_fd >= 0)
        {
            (void)shutdown(net->tcp_fd, SHUT_RDWR);
        }
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
