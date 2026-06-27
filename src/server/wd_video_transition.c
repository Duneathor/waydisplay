#include "wd_video_transition.h"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"


enum wd_video_recovery_action wd_video_recovery_decide(bool keyframe_queued, uint64_t recovery_frame_id,
                                                       uint64_t presented_frame_id, uint32_t wait_seconds,
                                                       uint32_t timeout_seconds, uint32_t attempts,
                                                       uint32_t maximum_attempts) {
    if (keyframe_queued && recovery_frame_id != 0 && presented_frame_id >= recovery_frame_id)
    {
        return WD_VIDEO_RECOVERY_ACTION_PRESENTED;
    }
    if (timeout_seconds == 0 || wait_seconds < timeout_seconds)
    {
        return WD_VIDEO_RECOVERY_ACTION_WAIT;
    }
    if (attempts < maximum_attempts)
    {
        return WD_VIDEO_RECOVERY_ACTION_RETRY_KEYFRAME;
    }
    return WD_VIDEO_RECOVERY_ACTION_FALLBACK_TILES;
}

enum wd_tile_recovery_action wd_tile_recovery_decide(bool refresh_sent, uint64_t required_content_epoch,
                                                     uint64_t presented_content_epoch, uint32_t wait_seconds,
                                                     uint32_t timeout_seconds) {
    if (!refresh_sent || required_content_epoch == 0)
    {
        return WD_TILE_RECOVERY_WAIT;
    }
    if (presented_content_epoch >= required_content_epoch)
    {
        return WD_TILE_RECOVERY_COMPLETE_PRESENTED;
    }
    if (timeout_seconds != 0 && wait_seconds >= timeout_seconds)
    {
        return WD_TILE_RECOVERY_COMPLETE_TIMEOUT;
    }
    return WD_TILE_RECOVERY_WAIT;
}

enum wd_tile_recovery_generation_action wd_tile_recovery_generation_decide(
    bool refresh_started, bool refresh_sent, uint64_t recovery_framebuffer_generation,
    uint64_t current_framebuffer_generation, bool recovery_queues_empty) {
    if (!refresh_started || refresh_sent || recovery_framebuffer_generation == 0)
    {
        return WD_TILE_RECOVERY_GENERATION_WAIT;
    }
    if (recovery_framebuffer_generation != current_framebuffer_generation)
    {
        return WD_TILE_RECOVERY_GENERATION_STALE;
    }
    return recovery_queues_empty ? WD_TILE_RECOVERY_GENERATION_TRANSMITTED : WD_TILE_RECOVERY_GENERATION_WAIT;
}

bool wd_video_entry_allowed(bool bootstrap_pending, bool recovery_active, uint32_t retry_cooldown_seconds, bool video_forced,
                            enum wd_video_recovery_class recovery_class) {
    if (bootstrap_pending || recovery_active)
    {
        return false;
    }
    if (retry_cooldown_seconds == 0)
    {
        return true;
    }
    return video_forced && recovery_class == WD_VIDEO_RECOVERY_PLANNED;
}

bool wd_video_control_allows_entry(uint8_t requested_mode, bool video_negotiated, bool video_channel_connected,
                                   bool video_encoder_available) {
    return requested_mode != WD_VIDEO_MODE_OFF && requested_mode <= WD_VIDEO_MODE_FORCE && video_negotiated &&
           video_channel_connected && video_encoder_available;
}

enum wd_planned_video_resume_action wd_planned_video_resume_decide(
    bool resume_requested, bool recovery_active, bool bootstrap_pending, uint8_t requested_mode,
    bool video_negotiated, bool video_channel_connected, bool video_encoder_available) {
    if (!resume_requested)
    {
        return WD_PLANNED_VIDEO_RESUME_WAIT;
    }
    if (!wd_video_control_allows_entry(requested_mode, video_negotiated, video_channel_connected,
                                       video_encoder_available))
    {
        return WD_PLANNED_VIDEO_RESUME_CLEAR;
    }
    if (recovery_active || bootstrap_pending)
    {
        return WD_PLANNED_VIDEO_RESUME_WAIT;
    }
    return WD_PLANNED_VIDEO_RESUME_ENTER;
}

uint64_t wd_next_nonzero_epoch(uint64_t current_epoch) {
    current_epoch++;
    return current_epoch == 0 ? 1 : current_epoch;
}

struct wd_video_entry_plan wd_video_entry_plan_make(uint64_t current_epoch, bool waiting_for_first_keyframe, bool frame_is_keyframe) {
    struct wd_video_entry_plan plan = {
        .source_content_epoch = current_epoch,
        .frame_content_epoch  = current_epoch,
        .commit_on_queue      = false,
    };
    if (waiting_for_first_keyframe && frame_is_keyframe)
    {
        plan.frame_content_epoch = wd_next_nonzero_epoch(current_epoch);
        plan.commit_on_queue     = true;
    }
    return plan;
}

bool wd_video_entry_plan_can_commit(const struct wd_video_entry_plan* plan, uint64_t current_epoch, bool queue_succeeded) {
    return plan && plan->commit_on_queue && queue_succeeded && plan->source_content_epoch == current_epoch &&
           plan->frame_content_epoch == wd_next_nonzero_epoch(current_epoch);
}

enum wd_client_video_health_class wd_client_video_health_classify(const struct wd_client_video_health_metrics* metrics) {
    if (!metrics || metrics->server_frames_tx == 0 || metrics->client_reports == 0)
    {
        return WD_CLIENT_VIDEO_HEALTH_IDLE;
    }
    if (metrics->client_decode_failures != 0 || metrics->client_publish_failures != 0)
    {
        return WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE;
    }
    if (metrics->client_decode_queue_drops != 0 ||
        (metrics->client_decode_queue_capacity != 0 &&
         metrics->client_decode_queue_depth_max >= metrics->client_decode_queue_capacity))
    {
        return WD_CLIENT_VIDEO_HEALTH_DECODER_OVERLOADED;
    }
    if (metrics->client_need_keyframe_drops != 0)
    {
        return WD_CLIENT_VIDEO_HEALTH_AWAITING_KEYFRAME;
    }
    if (metrics->client_frames_presented != 0)
    {
        return WD_CLIENT_VIDEO_HEALTH_NORMAL;
    }
    if (metrics->client_frames_decoded != 0 && metrics->client_audio_video_sync_holds != 0 &&
        (metrics->client_audio_playback_state == WD_CLIENT_AUDIO_PLAYBACK_BUFFERING ||
         metrics->client_audio_playback_state == WD_CLIENT_AUDIO_PLAYBACK_PLAYING) &&
        metrics->client_audio_video_startup_hold_ms <= WD_CLIENT_AUDIO_VIDEO_STARTUP_HOLD_MAX_MS &&
        metrics->client_audio_video_sync_hold_current_ms != 0 &&
        metrics->client_audio_video_sync_hold_current_ms <= WD_CLIENT_AUDIO_VIDEO_PLAYING_HOLD_MAX_MS &&
        (metrics->client_queue_depth != 0 || metrics->client_queue_depth_max != 0))
    {
        return WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT;
    }
    if (metrics->client_frames_seen != 0 || metrics->client_frames_decoded != 0)
    {
        return WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL;
    }
    return WD_CLIENT_VIDEO_HEALTH_IDLE;
}

struct wd_video_health_streak wd_video_health_streak_update(
    struct wd_video_health_streak current, enum wd_client_video_health_class sample) {
    if (sample != WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL && sample != WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE)
    {
        return (struct wd_video_health_streak){.health = WD_CLIENT_VIDEO_HEALTH_IDLE, .seconds = 0};
    }
    if (current.health != sample)
    {
        return (struct wd_video_health_streak){.health = sample, .seconds = 1};
    }
    if (current.seconds != UINT32_MAX)
    {
        current.seconds++;
    }
    return current;
}

enum wd_video_feedback_action wd_video_feedback_action_classify(uint32_t flags) {
    if ((flags & (WD_VIDEO_FEEDBACK_DECODE_FAILURE | WD_VIDEO_FEEDBACK_PUBLISH_FAILURE)) != 0)
    {
        return WD_VIDEO_FEEDBACK_ACTION_FALLBACK_TILES;
    }
    if ((flags & (WD_VIDEO_FEEDBACK_DECODE_OVERLOAD | WD_VIDEO_FEEDBACK_NEEDS_KEYFRAME)) != 0)
    {
        return WD_VIDEO_FEEDBACK_ACTION_RECOVER_IN_VIDEO;
    }
    return WD_VIDEO_FEEDBACK_ACTION_NONE;
}

const char* wd_client_video_health_name(enum wd_client_video_health_class health) {
    switch (health)
    {
    case WD_CLIENT_VIDEO_HEALTH_IDLE:
        return "idle";
    case WD_CLIENT_VIDEO_HEALTH_NORMAL:
        return "normal";
    case WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT:
        return "audio-wait";
    case WD_CLIENT_VIDEO_HEALTH_DECODER_OVERLOADED:
        return "decoder-overloaded";
    case WD_CLIENT_VIDEO_HEALTH_AWAITING_KEYFRAME:
        return "awaiting-keyframe";
    case WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL:
        return "pipeline-stall";
    case WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE:
        return "hard-failure";
    default:
        return "unknown";
    }
}

uint16_t wd_video_safe_decode_fps(uint64_t average_decode_ns, uint16_t requested_fps, uint32_t headroom_percent) {
    if (requested_fps == 0)
    {
        return 0;
    }
    if (average_decode_ns == 0 || headroom_percent == 0 || headroom_percent > 100u)
    {
        return requested_fps;
    }
    const uint64_t numerator = 1000000000ull * (uint64_t)headroom_percent;
    uint64_t fps = numerator / (average_decode_ns * 100ull);
    if (fps == 0)
    {
        fps = 1;
    }
    if (fps > requested_fps)
    {
        fps = requested_fps;
    }
    return (uint16_t)fps;
}

uint64_t wd_video_decode_ewma_update(uint64_t current_ewma_ns, uint64_t sample_ns,
                                     uint32_t new_sample_numerator, uint32_t denominator) {
    if (sample_ns == 0)
    {
        return current_ewma_ns;
    }
    if (current_ewma_ns == 0 || denominator == 0 || new_sample_numerator == 0 ||
        new_sample_numerator >= denominator)
    {
        return sample_ns;
    }
    const uint64_t old_weight = denominator - new_sample_numerator;
    return (current_ewma_ns * old_weight + sample_ns * new_sample_numerator) / denominator;
}

uint16_t wd_video_cadence_downshift_target(uint16_t current_fps, uint16_t requested_fps,
                                           uint16_t safe_decode_fps, bool hard_overload,
                                           uint16_t minimum_fps, uint16_t deadband_fps,
                                           uint32_t overload_percent) {
    if (current_fps == 0)
    {
        current_fps = requested_fps;
    }
    if (minimum_fps == 0)
    {
        minimum_fps = 1;
    }
    uint32_t target = current_fps;
    if (hard_overload)
    {
        if (overload_percent == 0 || overload_percent >= 100)
        {
            overload_percent = 75;
        }
        target = (uint32_t)current_fps * overload_percent / 100u;
        if (safe_decode_fps != 0 && safe_decode_fps < target)
        {
            target = safe_decode_fps;
        }
    }
    else if (safe_decode_fps != 0 && current_fps > safe_decode_fps + deadband_fps)
    {
        target = safe_decode_fps;
    }

    if (target < minimum_fps)
    {
        target = minimum_fps;
    }
    if (requested_fps != 0 && target > requested_fps)
    {
        target = requested_fps;
    }
    if (target >= current_fps)
    {
        return current_fps;
    }
    return (uint16_t)target;
}

uint16_t wd_video_cadence_upshift_target(uint16_t current_fps, uint16_t requested_fps,
                                         uint16_t safe_decode_fps, uint16_t deadband_fps,
                                         uint16_t increase_step) {
    if (current_fps == 0 || requested_fps == 0 || current_fps >= requested_fps)
    {
        return current_fps;
    }
    if (safe_decode_fps != 0 && safe_decode_fps < requested_fps &&
        (uint32_t)current_fps + deadband_fps >= safe_decode_fps)
    {
        return current_fps;
    }
    if (increase_step == 0)
    {
        increase_step = 1;
    }
    uint32_t target = (uint32_t)current_fps + increase_step;
    if (safe_decode_fps != 0 && target > safe_decode_fps)
    {
        target = safe_decode_fps;
    }
    if (target > requested_fps)
    {
        target = requested_fps;
    }
    return (uint16_t)target;
}
