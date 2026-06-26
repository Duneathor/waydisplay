#pragma once

#include "wd_server_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* Internal contracts between the stream orchestrator and its video stage.
 * Callers hold server->net.lock unless a function name says otherwise. */

struct wd_stream_damage_view {
    const bool* tiles;
    bool        all_tiles;
    uint32_t    tile_count;
};

bool wd_stream_frame_worker_init(struct wd_server* server);
void wd_stream_frame_worker_destroy(struct wd_server* server);
bool wd_stream_frame_worker_idle(struct wd_server* server);
bool wd_stream_frame_worker_submit(struct wd_server* server);
void wd_stream_frame_worker_request_service(struct wd_server* server);
bool wd_stream_process_frame(struct wd_server* server, const struct wd_stream_damage_view* damage);
bool wd_stream_process_queued_work(struct wd_server* server);
void wd_stream_controller_tick(struct wd_server* server);
double      wd_stream_coverage_pct(uint64_t per_mille);
const char* wd_stream_mode_name(enum wd_stream_mode mode);
const char* wd_stream_mode_owner_name(enum wd_stream_mode mode);
uint16_t    wd_stream_policy_capture_pacing_fps_locked(const struct wd_stream_policy* policy, uint16_t output_refresh_hz);
uint32_t    wd_stream_frame_service_interval_ms(struct wd_server* server);
void        wd_stream_policy_update_health_locked(struct wd_stream_policy* policy, struct wd_stats* stats);
void        wd_stream_policy_update_mode_locked(struct wd_stream_policy* policy, const struct wd_stats* stats, uint16_t total_tiles,
                                                 bool video_negotiated, bool video_channel_connected,
                                                 bool video_encoder_available);
bool     wd_stream_mode_video_owns_display(enum wd_stream_mode mode);
uint16_t wd_stream_policy_effective_fps_locked(const struct wd_stream_policy* policy);
void     wd_stream_advance_content_epoch_locked(struct wd_server* server, const char* reason);
void     wd_stream_policy_set_mode_locked(struct wd_stream_policy* policy, enum wd_stream_mode mode, const char* reason,
                                          double dirty_avg_pct, double dirty_peak_pct, double budget_pressure_pct,
                                          bool video_channel_connected, bool video_encoder_available);

bool     wd_stream_video_worker_init(struct wd_server* server);
void     wd_stream_video_worker_destroy(struct wd_server* server);
bool     wd_stream_queue_video_control_frame_locked(struct wd_server* server, uint16_t flags);
bool     wd_stream_try_publish_video_frame_locked(struct wd_server* server, uint64_t now_ns);
uint32_t wd_stream_video_bitrate_kib_locked(const struct wd_stream_policy* policy);
