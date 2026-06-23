#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_video_entry_plan {
    uint64_t source_content_epoch;
    uint64_t frame_content_epoch;
    bool     commit_on_queue;
};

enum wd_tile_recovery_action {
    WD_TILE_RECOVERY_WAIT               = 0,
    WD_TILE_RECOVERY_COMPLETE_PRESENTED = 1,
    WD_TILE_RECOVERY_COMPLETE_TIMEOUT   = 2,
};

enum wd_tile_recovery_action wd_tile_recovery_decide(bool refresh_sent, uint64_t client_tile_frames_presented, uint32_t wait_seconds,
                                                     uint32_t timeout_seconds);
bool                         wd_video_entry_allowed(bool recovery_active, uint32_t retry_cooldown_seconds);

enum wd_client_video_health_class {
    WD_CLIENT_VIDEO_HEALTH_IDLE           = 0,
    WD_CLIENT_VIDEO_HEALTH_NORMAL         = 1,
    WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT     = 2,
    WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL = 3,
    WD_CLIENT_VIDEO_HEALTH_DECODE_FAILURE = 4,
};

struct wd_client_video_health_metrics {
    uint64_t server_frames_tx;
    uint64_t client_reports;
    uint64_t client_frames_seen;
    uint64_t client_frames_decoded;
    uint64_t client_frames_presented;
    uint64_t client_decode_failures;
    uint64_t client_publish_failures;
    uint64_t client_need_keyframe_drops;
    uint64_t client_audio_sync_holds;
    uint32_t client_queue_depth;
};

enum wd_client_video_health_class wd_client_video_health_classify(const struct wd_client_video_health_metrics* metrics);
const char*                       wd_client_video_health_name(enum wd_client_video_health_class health);

uint64_t                   wd_next_nonzero_epoch(uint64_t current_epoch);
struct wd_video_entry_plan wd_video_entry_plan_make(uint64_t current_epoch, bool waiting_for_first_keyframe, bool frame_is_keyframe);
bool wd_video_entry_plan_can_commit(const struct wd_video_entry_plan* plan, uint64_t current_epoch, bool queue_succeeded);

#ifdef __cplusplus
}
#endif
