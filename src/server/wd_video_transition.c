#include "wd_video_transition.h"

enum wd_tile_recovery_action wd_tile_recovery_decide(bool refresh_sent, uint64_t client_tile_frames_presented, uint32_t wait_seconds,
                                                     uint32_t timeout_seconds) {
    if (!refresh_sent)
    {
        return WD_TILE_RECOVERY_WAIT;
    }
    if (client_tile_frames_presented != 0)
    {
        return WD_TILE_RECOVERY_COMPLETE_PRESENTED;
    }
    if (timeout_seconds != 0 && wait_seconds >= timeout_seconds)
    {
        return WD_TILE_RECOVERY_COMPLETE_TIMEOUT;
    }
    return WD_TILE_RECOVERY_WAIT;
}

bool wd_video_entry_allowed(bool recovery_active, uint32_t retry_cooldown_seconds) {
    return !recovery_active && retry_cooldown_seconds == 0;
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
    if (metrics->client_decode_failures != 0 || metrics->client_publish_failures != 0 || metrics->client_need_keyframe_drops != 0)
    {
        return WD_CLIENT_VIDEO_HEALTH_DECODE_FAILURE;
    }
    if (metrics->client_frames_presented != 0)
    {
        return WD_CLIENT_VIDEO_HEALTH_NORMAL;
    }
    if (metrics->client_frames_decoded != 0 && metrics->client_audio_video_sync_holds != 0 && metrics->client_queue_depth != 0)
    {
        return WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT;
    }
    if (metrics->client_frames_seen != 0 || metrics->client_frames_decoded != 0)
    {
        return WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL;
    }
    return WD_CLIENT_VIDEO_HEALTH_IDLE;
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
    case WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL:
        return "pipeline-stall";
    case WD_CLIENT_VIDEO_HEALTH_DECODE_FAILURE:
        return "decode-failure";
    default:
        return "unknown";
    }
}
