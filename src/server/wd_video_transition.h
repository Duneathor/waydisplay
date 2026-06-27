#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "waydisplay/wd_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

struct wd_video_entry_plan {
    uint64_t source_content_epoch;
    uint64_t frame_content_epoch;
    bool     commit_on_queue;
};

enum wd_video_recovery_class {
    WD_VIDEO_RECOVERY_NONE    = 0,
    WD_VIDEO_RECOVERY_PLANNED = 1,
    WD_VIDEO_RECOVERY_FAILURE = 2,
};


enum wd_video_recovery_action {
    WD_VIDEO_RECOVERY_ACTION_WAIT = 0,
    WD_VIDEO_RECOVERY_ACTION_PRESENTED = 1,
    WD_VIDEO_RECOVERY_ACTION_RETRY_KEYFRAME = 2,
    WD_VIDEO_RECOVERY_ACTION_FALLBACK_TILES = 3,
};

enum wd_video_recovery_action wd_video_recovery_decide(bool keyframe_queued, uint64_t recovery_frame_id,
                                                       uint64_t presented_frame_id, uint32_t wait_seconds,
                                                       uint32_t timeout_seconds, uint32_t attempts,
                                                       uint32_t maximum_attempts);

enum wd_tile_recovery_action {
    WD_TILE_RECOVERY_WAIT               = 0,
    WD_TILE_RECOVERY_COMPLETE_PRESENTED = 1,
    WD_TILE_RECOVERY_COMPLETE_TIMEOUT   = 2,
};

enum wd_tile_recovery_action wd_tile_recovery_decide(bool refresh_sent, uint64_t required_content_epoch,
                                                     uint64_t presented_content_epoch, uint32_t wait_seconds,
                                                     uint32_t timeout_seconds);
bool wd_video_entry_allowed(bool bootstrap_pending, bool recovery_active, uint32_t retry_cooldown_seconds, bool video_forced,
                            enum wd_video_recovery_class recovery_class);
bool wd_video_control_allows_entry(uint8_t requested_mode, bool video_negotiated, bool video_channel_connected,
                                   bool video_encoder_available);

enum wd_client_video_health_class {
    WD_CLIENT_VIDEO_HEALTH_IDLE           = 0,
    WD_CLIENT_VIDEO_HEALTH_NORMAL         = 1,
    WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT        = 2,
    WD_CLIENT_VIDEO_HEALTH_DECODER_OVERLOADED = 3,
    WD_CLIENT_VIDEO_HEALTH_AWAITING_KEYFRAME  = 4,
    WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL     = 5,
    WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE       = 6,
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
    uint64_t client_audio_video_sync_holds;
    uint64_t client_decode_queue_drops;
    uint64_t client_audio_video_startup_timeouts;
    uint32_t client_audio_video_startup_hold_ms;
    uint8_t  client_audio_playback_state;
    uint32_t client_decode_queue_depth;
    uint32_t client_decode_queue_depth_max;
    uint16_t client_decode_queue_capacity;
    uint32_t client_queue_depth;
    uint32_t client_queue_depth_max;
    uint32_t client_audio_video_sync_hold_current_ms;
};

enum wd_client_video_health_class wd_client_video_health_classify(const struct wd_client_video_health_metrics* metrics);
const char*                       wd_client_video_health_name(enum wd_client_video_health_class health);

struct wd_video_health_streak {
    enum wd_client_video_health_class health;
    uint32_t                          seconds;
};

struct wd_video_health_streak wd_video_health_streak_update(
    struct wd_video_health_streak current, enum wd_client_video_health_class sample);

enum wd_video_feedback_action {
    WD_VIDEO_FEEDBACK_ACTION_NONE             = 0,
    WD_VIDEO_FEEDBACK_ACTION_RECOVER_IN_VIDEO = 1,
    WD_VIDEO_FEEDBACK_ACTION_FALLBACK_TILES   = 2,
};

enum wd_video_feedback_action wd_video_feedback_action_classify(uint32_t flags);

uint16_t                          wd_video_safe_decode_fps(uint64_t average_decode_ns, uint16_t requested_fps,
                                                          uint32_t headroom_percent);

uint64_t                   wd_next_nonzero_epoch(uint64_t current_epoch);
struct wd_video_entry_plan wd_video_entry_plan_make(uint64_t current_epoch, bool waiting_for_first_keyframe, bool frame_is_keyframe);
bool wd_video_entry_plan_can_commit(const struct wd_video_entry_plan* plan, uint64_t current_epoch, bool queue_succeeded);

#ifdef __cplusplus
}
#endif
