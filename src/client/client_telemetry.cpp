#include "client_telemetry.hpp"

#include "audio_playback.hpp"
#include "client_net.hpp"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_time.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace waydisplay {
namespace {

void client_stats_accumulate(ClientStatsSnapshot& dst, const ClientStatsSnapshot& src) {
    dst.udp_packets += src.udp_packets;
    dst.udp_bytes += src.udp_bytes;
    dst.udp_interarrival_samples += src.udp_interarrival_samples;
    dst.udp_interarrival_sum_ns += src.udp_interarrival_sum_ns;
    dst.udp_jitter_samples += src.udp_jitter_samples;
    dst.udp_jitter_sum_ns += src.udp_jitter_sum_ns;
    dst.udp_interarrival_max_ns = std::max(dst.udp_interarrival_max_ns, src.udp_interarrival_max_ns);
    dst.invalid += src.invalid;
    dst.invalid_short += src.invalid_short;
    dst.invalid_header += src.invalid_header;
    dst.invalid_geometry += src.invalid_geometry;
    dst.invalid_fragment += src.invalid_fragment;
    dst.invalid_blit += src.invalid_blit;
    dst.invalid_dirty_grid += src.invalid_dirty_grid;
    dst.ignored_probe += src.ignored_probe;
    dst.stale_session += src.stale_session;
    dst.stale_epoch += src.stale_epoch;
    dst.old_gen += src.old_gen;
    dst.completed += src.completed;
    dst.completed_compressed += src.completed_compressed;
    dst.completed_packets += src.completed_packets;
    dst.partial_timeouts += src.partial_timeouts;
    dst.partial_missing_packets += src.partial_missing_packets;
    dst.partial_retx_queued += src.partial_retx_queued;
    dst.retx_response_samples += src.retx_response_samples;
    dst.retx_response_sum_ns += src.retx_response_sum_ns;
    dst.timeout_updates += src.timeout_updates;
    dst.summaries += src.summaries;
    dst.retx += src.retx;
    dst.summary_retx_queued += src.summary_retx_queued;
    dst.summary_retx_deferred += src.summary_retx_deferred;
    dst.summary_retx_throttled += src.summary_retx_throttled;
    dst.summary_retx_stale_dropped += src.summary_retx_stale_dropped;
    dst.summary_retx_pressure_dropped += src.summary_retx_pressure_dropped;
    dst.summary_promote_passes += src.summary_promote_passes;
    dst.summary_promote_scanned += src.summary_promote_scanned;
    dst.summary_promote_candidates += src.summary_promote_candidates;
    dst.summary_to_retx_samples += src.summary_to_retx_samples;
    dst.summary_to_retx_sum_ns += src.summary_to_retx_sum_ns;
    dst.keys += src.keys;
    dst.pointer += src.pointer;
    dst.input_events += src.input_events;
    dst.input_channel_events += src.input_channel_events;
    dst.selection_channel_events += src.selection_channel_events;
    dst.tcp_async_queued += src.tcp_async_queued;
    dst.tcp_async_completed += src.tcp_async_completed;
    dst.tcp_async_failed += src.tcp_async_failed;
    dst.tcp_async_overflow += src.tcp_async_overflow;
    dst.tcp_async_partial += src.tcp_async_partial;
    dst.tcp_async_coalesced += src.tcp_async_coalesced;
    dst.tcp_async_inflight_max = std::max(dst.tcp_async_inflight_max, src.tcp_async_inflight_max);
    dst.video_frames_rx += src.video_frames_rx;
    dst.video_bytes_rx += src.video_bytes_rx;
    dst.video_frames_decoded += src.video_frames_decoded;
    dst.video_frames_presented += src.video_frames_presented;
    dst.video_decode_failed += src.video_decode_failed;
    dst.video_publish_failed += src.video_publish_failed;
    dst.video_control_frames_rx += src.video_control_frames_rx;
    dst.video_need_keyframe_drops += src.video_need_keyframe_drops;
    dst.video_decoder_resets += src.video_decoder_resets;
    dst.video_decode_samples += src.video_decode_samples;
    dst.video_decode_sum_ns += src.video_decode_sum_ns;
    dst.video_messages_rx += src.video_messages_rx;
    dst.video_data_frames_rx += src.video_data_frames_rx;
    dst.video_invalid_frames_rx += src.video_invalid_frames_rx;
    dst.video_stale_frames_dropped += src.video_stale_frames_dropped;
    dst.video_last_frame_id_rx        = std::max(dst.video_last_frame_id_rx, src.video_last_frame_id_rx);
    dst.video_last_frame_id_decoded   = std::max(dst.video_last_frame_id_decoded, src.video_last_frame_id_decoded);
    dst.video_last_frame_id_presented = std::max(dst.video_last_frame_id_presented, src.video_last_frame_id_presented);
    dst.video_present_latency_samples += src.video_present_latency_samples;
    dst.video_present_latency_sum_ns += src.video_present_latency_sum_ns;
    dst.audio_video_sync_holds += src.audio_video_sync_holds;
    dst.audio_video_sync_drops += src.audio_video_sync_drops;
    dst.audio_video_startup_timeouts += src.audio_video_startup_timeouts;
    dst.audio_video_startup_hold_ms = src.audio_video_startup_hold_ms;
    dst.audio_playback_state = src.audio_playback_state;
    dst.video_queue_overflow_drops += src.video_queue_overflow_drops;
    dst.video_decode_queue_drops += src.video_decode_queue_drops;
    dst.video_decode_queue_depth   = src.video_decode_queue_depth;
    dst.video_decode_queue_depth_max = std::max(dst.video_decode_queue_depth_max, src.video_decode_queue_depth_max);
    dst.video_decode_queue_capacity = src.video_decode_queue_capacity;
    dst.video_decoder_phase = src.video_decoder_phase;
    dst.video_waiting_keyframe = src.video_waiting_keyframe;
    dst.audio_video_sync_hold_current_ms = src.audio_video_sync_hold_current_ms;
    dst.audio_video_sync_hold_max_ms = std::max(dst.audio_video_sync_hold_max_ms, src.audio_video_sync_hold_max_ms);
    dst.video_queue_depth         = src.video_queue_depth;
    dst.video_queue_depth_max     = std::max(dst.video_queue_depth_max, src.video_queue_depth_max);
    dst.video_oldest_pts_usec     = src.video_oldest_pts_usec;
    dst.audio_video_delta_samples = src.audio_video_delta_samples;
    dst.tile_frames_presented += src.tile_frames_presented;
    dst.tile_content_epoch_presented  = std::max(dst.tile_content_epoch_presented, src.tile_content_epoch_presented);
    dst.video_content_epoch_presented = std::max(dst.video_content_epoch_presented, src.video_content_epoch_presented);
    dst.audio_messages_rx += src.audio_messages_rx;
    dst.audio_packets_rx += src.audio_packets_rx;
    dst.audio_bytes_rx += src.audio_bytes_rx;
    dst.audio_decode_failed += src.audio_decode_failed;
    dst.audio_decode_queue_drops += src.audio_decode_queue_drops;
    dst.audio_discontinuities += src.audio_discontinuities;
    dst.audio_late_drops += src.audio_late_drops;
    dst.audio_underflows += src.audio_underflows;
    dst.udp_async_posted += src.udp_async_posted;
    dst.udp_async_retired += src.udp_async_retired;
    dst.udp_async_completed += src.udp_async_completed;
    dst.udp_async_failed += src.udp_async_failed;
    dst.udp_async_submit_failed += src.udp_async_submit_failed;
    dst.udp_async_cancels += src.udp_async_cancels;
    dst.udp_async_inflight_current = src.udp_async_inflight_current;
    dst.udp_async_prepared_current = src.udp_async_prepared_current;
    dst.udp_async_inflight_max     = std::max(dst.udp_async_inflight_max, src.udp_async_inflight_max);
    dst.udp_async_drained_on_reconfigure += src.udp_async_drained_on_reconfigure;
    dst.udp_async_cancelled_on_reconfigure += src.udp_async_cancelled_on_reconfigure;
    dst.udp_async_receiver_generations += src.udp_async_receiver_generations;
    dst.udp_async_accounting_errors += src.udp_async_accounting_errors;
    dst.tile_assembly_samples += src.tile_assembly_samples;
    dst.tile_assembly_sum_ns += src.tile_assembly_sum_ns;
    dst.tile_present_samples += src.tile_present_samples;
    dst.tile_present_sum_ns += src.tile_present_sum_ns;
    dst.input_to_present_samples += src.input_to_present_samples;
    dst.input_to_present_sum_ns += src.input_to_present_sum_ns;
    dst.input_seq_present_samples += src.input_seq_present_samples;
    dst.input_seq_present_sum_ns += src.input_seq_present_sum_ns;
    dst.sdl_render_frames += src.sdl_render_frames;
    dst.sdl_remote_frames += src.sdl_remote_frames;
    dst.sdl_empty_remote_wakeups += src.sdl_empty_remote_wakeups;
    dst.sdl_texture_full_uploads += src.sdl_texture_full_uploads;
    dst.sdl_texture_partial_uploads += src.sdl_texture_partial_uploads;
    dst.sdl_texture_dirty_rects += src.sdl_texture_dirty_rects;
    dst.sdl_texture_source_dirty_rects += src.sdl_texture_source_dirty_rects;
    dst.sdl_texture_coalesced_dirty_rects += src.sdl_texture_coalesced_dirty_rects;
    dst.sdl_texture_bounds_uploads += src.sdl_texture_bounds_uploads;
    dst.sdl_texture_cost_full_uploads += src.sdl_texture_cost_full_uploads;
    dst.sdl_texture_lock_calls += src.sdl_texture_lock_calls;
    dst.sdl_texture_update_calls += src.sdl_texture_update_calls;
    dst.sdl_texture_model_update_call_ns = src.sdl_texture_model_update_call_ns;
    dst.sdl_texture_model_lock_call_ns   = src.sdl_texture_model_lock_call_ns;
    dst.sdl_texture_model_pixel_cost_q16 = src.sdl_texture_model_pixel_cost_q16;
    dst.sdl_texture_source_pixels += src.sdl_texture_source_pixels;
    dst.sdl_texture_upload_pixels += src.sdl_texture_upload_pixels;
    dst.sdl_texture_upload_samples += src.sdl_texture_upload_samples;
    dst.sdl_texture_upload_sum_ns += src.sdl_texture_upload_sum_ns;
    dst.sdl_texture_upload_max_ns = std::max(dst.sdl_texture_upload_max_ns, src.sdl_texture_upload_max_ns);
    dst.framebuffer_snapshot_pixels += src.framebuffer_snapshot_pixels;
    dst.framebuffer_snapshot_samples += src.framebuffer_snapshot_samples;
    dst.framebuffer_snapshot_sum_ns += src.framebuffer_snapshot_sum_ns;
    dst.framebuffer_snapshot_max_ns = std::max(dst.framebuffer_snapshot_max_ns, src.framebuffer_snapshot_max_ns);
    dst.framebuffer_direct_uploads += src.framebuffer_direct_uploads;
    dst.framebuffer_staged_uploads += src.framebuffer_staged_uploads;
    dst.framebuffer_lock_wait_samples += src.framebuffer_lock_wait_samples;
    dst.framebuffer_lock_wait_sum_ns += src.framebuffer_lock_wait_sum_ns;
    dst.framebuffer_lock_wait_max_ns = std::max(dst.framebuffer_lock_wait_max_ns, src.framebuffer_lock_wait_max_ns);
    dst.framebuffer_lock_hold_samples += src.framebuffer_lock_hold_samples;
    dst.framebuffer_lock_hold_sum_ns += src.framebuffer_lock_hold_sum_ns;
    dst.framebuffer_lock_hold_max_ns = std::max(dst.framebuffer_lock_hold_max_ns, src.framebuffer_lock_hold_max_ns);
    dst.sdl_video_texture_uploads += src.sdl_video_texture_uploads;
    dst.sdl_video_texture_upload_pixels += src.sdl_video_texture_upload_pixels;
    dst.sdl_present_samples += src.sdl_present_samples;
    dst.sdl_present_sum_ns += src.sdl_present_sum_ns;
    dst.sdl_present_max_ns = std::max(dst.sdl_present_max_ns, src.sdl_present_max_ns);
}

uint64_t take_stat(std::atomic<uint64_t>& value) {
    return value.exchange(0, std::memory_order_relaxed);
}

void update_udp_gap_pressure(ClientState& state, uint64_t max_gap_ns, uint64_t interarrival_samples, uint64_t udp_packets) {
    uint64_t current = state.udp_gap_pressure_ns.load(std::memory_order_relaxed);
    uint64_t target  = 0;

    if (udp_packets >= WD_CLIENT_RUNTIME_GAP_MIN_SAMPLES && interarrival_samples >= WD_CLIENT_RUNTIME_GAP_MIN_SAMPLES &&
        max_gap_ns >= WD_LINK_RUNTIME_GAP_PRESSURE_MIN_NS)
    {
        target = max_gap_ns;
    }

    if (target > current)
    {
        state.udp_gap_pressure_ns.store(target, std::memory_order_relaxed);
        return;
    }

    if (current == 0)
    {
        return;
    }

    uint64_t decayed = (current * (uint64_t)WD_LINK_RUNTIME_GAP_PRESSURE_DECAY_PERCENT) / 100ull;
    if (decayed < WD_LINK_RUNTIME_GAP_PRESSURE_MIN_NS)
    {
        decayed = 0;
    }
    state.udp_gap_pressure_ns.store(decayed, std::memory_order_relaxed);
}

double avg_ms(uint64_t sum_ns, uint64_t samples) {
    if (samples == 0)
    {
        return 0.0;
    }

    return static_cast<double>(sum_ns) / static_cast<double>(samples) / 1000000.0;
}

void log_client_stats_snapshot(ClientState& state, const ClientStatsSnapshot& logged) {
    const uint64_t udp_packets                        = logged.udp_packets;
    const uint64_t udp_bytes                          = logged.udp_bytes;
    const uint64_t udp_interarrival_samples           = logged.udp_interarrival_samples;
    const uint64_t udp_interarrival_sum_ns            = logged.udp_interarrival_sum_ns;
    const uint64_t udp_jitter_samples                 = logged.udp_jitter_samples;
    const uint64_t udp_jitter_sum_ns                  = logged.udp_jitter_sum_ns;
    const uint64_t udp_interarrival_max_ns            = logged.udp_interarrival_max_ns;
    const uint64_t invalid                            = logged.invalid;
    const uint64_t ignored_probe                      = logged.ignored_probe;
    const uint64_t stale_session                      = logged.stale_session;
    const uint64_t old_gen                            = logged.old_gen;
    const uint64_t completed                          = logged.completed;
    const uint64_t completed_compressed               = logged.completed_compressed;
    const uint64_t completed_packets                  = logged.completed_packets;
    const uint64_t partial_timeouts                   = logged.partial_timeouts;
    const uint64_t partial_missing_packets            = logged.partial_missing_packets;
    const uint64_t partial_retx_queued                = logged.partial_retx_queued;
    const uint64_t retx_response_samples              = logged.retx_response_samples;
    const uint64_t retx_response_sum_ns               = logged.retx_response_sum_ns;
    const uint64_t timeout_updates                    = logged.timeout_updates;
    const uint64_t summaries                          = logged.summaries;
    const uint64_t retx                               = logged.retx;
    const uint64_t summary_retx_queued                = logged.summary_retx_queued;
    const uint64_t summary_retx_deferred              = logged.summary_retx_deferred;
    const uint64_t summary_retx_throttled             = logged.summary_retx_throttled;
    const uint64_t summary_retx_stale_dropped         = logged.summary_retx_stale_dropped;
    const uint64_t summary_retx_pressure_dropped      = logged.summary_retx_pressure_dropped;
    const uint64_t summary_promote_passes             = logged.summary_promote_passes;
    const uint64_t summary_promote_scanned            = logged.summary_promote_scanned;
    const uint64_t summary_promote_candidates         = logged.summary_promote_candidates;
    const uint64_t summary_to_retx_samples            = logged.summary_to_retx_samples;
    const uint64_t summary_to_retx_sum_ns             = logged.summary_to_retx_sum_ns;
    const uint64_t keys                               = logged.keys;
    const uint64_t pointer                            = logged.pointer;
    const uint64_t input_events                       = logged.input_events;
    const uint64_t input_channel_events               = logged.input_channel_events;
    const uint64_t selection_channel_events           = logged.selection_channel_events;
    const uint64_t tcp_async_queued                   = logged.tcp_async_queued;
    const uint64_t tcp_async_completed                = logged.tcp_async_completed;
    const uint64_t tcp_async_failed                   = logged.tcp_async_failed;
    const uint64_t tcp_async_overflow                 = logged.tcp_async_overflow;
    const uint64_t tcp_async_partial                  = logged.tcp_async_partial;
    const uint64_t tcp_async_coalesced                = logged.tcp_async_coalesced;
    const uint64_t tcp_async_inflight_max             = logged.tcp_async_inflight_max;
    const uint64_t video_frames_rx                    = logged.video_frames_rx;
    const uint64_t video_bytes_rx                     = logged.video_bytes_rx;
    const uint64_t video_frames_decoded               = logged.video_frames_decoded;
    const uint64_t video_frames_presented             = logged.video_frames_presented;
    const uint64_t video_decode_failed                = logged.video_decode_failed;
    const uint64_t video_publish_failed               = logged.video_publish_failed;
    const uint64_t video_control_frames_rx            = logged.video_control_frames_rx;
    const uint64_t video_need_keyframe_drops          = logged.video_need_keyframe_drops;
    const uint64_t video_decoder_resets               = logged.video_decoder_resets;
    const uint64_t video_decode_samples               = logged.video_decode_samples;
    const uint64_t video_decode_sum_ns                = logged.video_decode_sum_ns;
    const uint64_t video_messages_rx                  = logged.video_messages_rx;
    const uint64_t video_data_frames_rx               = logged.video_data_frames_rx;
    const uint64_t video_invalid_frames_rx            = logged.video_invalid_frames_rx;
    const uint64_t video_stale_frames_dropped         = logged.video_stale_frames_dropped;
    const uint64_t video_last_frame_id_rx             = logged.video_last_frame_id_rx;
    const uint64_t video_last_frame_id_decoded        = logged.video_last_frame_id_decoded;
    const uint64_t video_last_frame_id_presented      = logged.video_last_frame_id_presented;
    const uint64_t video_present_latency_samples      = logged.video_present_latency_samples;
    const uint64_t video_present_latency_sum_ns       = logged.video_present_latency_sum_ns;
    const uint64_t audio_video_sync_holds             = logged.audio_video_sync_holds;
    const uint64_t audio_video_sync_drops             = logged.audio_video_sync_drops;
    const uint64_t audio_video_startup_timeouts        = logged.audio_video_startup_timeouts;
    const uint32_t audio_video_startup_hold_ms         = logged.audio_video_startup_hold_ms;
    const uint8_t  audio_playback_state                = logged.audio_playback_state;
    const uint64_t video_queue_overflow_drops         = logged.video_queue_overflow_drops;
    const uint64_t video_decode_queue_drops           = logged.video_decode_queue_drops;
    const uint32_t video_decode_queue_depth           = logged.video_decode_queue_depth;
    const uint32_t video_decode_queue_depth_max       = logged.video_decode_queue_depth_max;
    const uint16_t video_decode_queue_capacity        = logged.video_decode_queue_capacity;
    const uint8_t  video_decoder_phase                = logged.video_decoder_phase;
    const uint8_t  video_waiting_keyframe             = logged.video_waiting_keyframe;
    const uint32_t audio_video_sync_hold_current_ms   = logged.audio_video_sync_hold_current_ms;
    const uint32_t audio_video_sync_hold_max_ms       = logged.audio_video_sync_hold_max_ms;
    const uint32_t video_queue_depth                  = logged.video_queue_depth;
    const uint32_t video_queue_depth_max              = logged.video_queue_depth_max;
    const uint64_t video_oldest_pts_usec              = logged.video_oldest_pts_usec;
    const int64_t  audio_video_delta_samples          = logged.audio_video_delta_samples;
    const uint64_t tile_frames_presented              = logged.tile_frames_presented;
    const uint64_t audio_messages_rx                  = logged.audio_messages_rx;
    const uint64_t audio_packets_rx                   = logged.audio_packets_rx;
    const uint64_t audio_bytes_rx                     = logged.audio_bytes_rx;
    const uint64_t audio_decode_failed                = logged.audio_decode_failed;
    const uint64_t audio_decode_queue_drops           = logged.audio_decode_queue_drops;
    const uint64_t audio_discontinuities              = logged.audio_discontinuities;
    const uint64_t audio_late_drops                   = logged.audio_late_drops;
    const uint64_t audio_underflows                   = logged.audio_underflows;
    const uint64_t udp_async_posted                   = logged.udp_async_posted;
    const uint64_t udp_async_retired                  = logged.udp_async_retired;
    const uint64_t udp_async_completed                = logged.udp_async_completed;
    const uint64_t udp_async_failed                   = logged.udp_async_failed;
    const uint64_t udp_async_submit_failed            = logged.udp_async_submit_failed;
    const uint64_t udp_async_cancels                  = logged.udp_async_cancels;
    const uint64_t udp_async_inflight_current         = logged.udp_async_inflight_current;
    const uint64_t udp_async_prepared_current         = logged.udp_async_prepared_current;
    const uint64_t udp_async_inflight_max             = logged.udp_async_inflight_max;
    const uint64_t udp_async_drained_on_reconfigure   = logged.udp_async_drained_on_reconfigure;
    const uint64_t udp_async_cancelled_on_reconfigure = logged.udp_async_cancelled_on_reconfigure;
    const uint64_t udp_async_receiver_generations     = logged.udp_async_receiver_generations;
    const uint64_t udp_async_accounting_errors        = logged.udp_async_accounting_errors;
    const uint64_t tile_assembly_samples              = logged.tile_assembly_samples;
    const uint64_t tile_assembly_sum_ns               = logged.tile_assembly_sum_ns;
    const uint64_t tile_present_samples               = logged.tile_present_samples;
    const uint64_t tile_present_sum_ns                = logged.tile_present_sum_ns;
    const uint64_t input_to_present_samples           = logged.input_to_present_samples;
    const uint64_t input_to_present_sum_ns            = logged.input_to_present_sum_ns;
    const uint64_t input_seq_present_samples          = logged.input_seq_present_samples;
    const uint64_t input_seq_present_sum_ns           = logged.input_seq_present_sum_ns;
    const uint64_t sdl_render_frames                  = logged.sdl_render_frames;
    const uint64_t sdl_remote_frames                  = logged.sdl_remote_frames;
    const uint64_t sdl_empty_remote_wakeups           = logged.sdl_empty_remote_wakeups;
    const uint64_t sdl_texture_full_uploads           = logged.sdl_texture_full_uploads;
    const uint64_t sdl_texture_partial_uploads        = logged.sdl_texture_partial_uploads;
    const uint64_t sdl_texture_dirty_rects            = logged.sdl_texture_dirty_rects;
    const uint64_t sdl_texture_source_dirty_rects     = logged.sdl_texture_source_dirty_rects;
    const uint64_t sdl_texture_coalesced_dirty_rects  = logged.sdl_texture_coalesced_dirty_rects;
    const uint64_t sdl_texture_bounds_uploads         = logged.sdl_texture_bounds_uploads;
    const uint64_t sdl_texture_cost_full_uploads      = logged.sdl_texture_cost_full_uploads;
    const uint64_t sdl_texture_lock_calls             = logged.sdl_texture_lock_calls;
    const uint64_t sdl_texture_update_calls           = logged.sdl_texture_update_calls;
    const uint64_t sdl_texture_model_update_call_ns   = logged.sdl_texture_model_update_call_ns;
    const uint64_t sdl_texture_model_lock_call_ns     = logged.sdl_texture_model_lock_call_ns;
    const uint64_t sdl_texture_model_pixel_cost_q16   = logged.sdl_texture_model_pixel_cost_q16;
    const uint64_t sdl_texture_source_pixels          = logged.sdl_texture_source_pixels;
    const uint64_t sdl_texture_upload_pixels          = logged.sdl_texture_upload_pixels;
    const uint64_t sdl_texture_upload_samples         = logged.sdl_texture_upload_samples;
    const uint64_t sdl_texture_upload_sum_ns          = logged.sdl_texture_upload_sum_ns;
    const uint64_t sdl_texture_upload_max_ns          = logged.sdl_texture_upload_max_ns;
    const uint64_t framebuffer_snapshot_pixels        = logged.framebuffer_snapshot_pixels;
    const uint64_t framebuffer_snapshot_samples       = logged.framebuffer_snapshot_samples;
    const uint64_t framebuffer_snapshot_sum_ns        = logged.framebuffer_snapshot_sum_ns;
    const uint64_t framebuffer_snapshot_max_ns        = logged.framebuffer_snapshot_max_ns;
    const uint64_t framebuffer_direct_uploads         = logged.framebuffer_direct_uploads;
    const uint64_t framebuffer_staged_uploads         = logged.framebuffer_staged_uploads;
    const uint64_t framebuffer_lock_wait_samples      = logged.framebuffer_lock_wait_samples;
    const uint64_t framebuffer_lock_wait_sum_ns       = logged.framebuffer_lock_wait_sum_ns;
    const uint64_t framebuffer_lock_wait_max_ns       = logged.framebuffer_lock_wait_max_ns;
    const uint64_t framebuffer_lock_hold_samples      = logged.framebuffer_lock_hold_samples;
    const uint64_t framebuffer_lock_hold_sum_ns       = logged.framebuffer_lock_hold_sum_ns;
    const uint64_t framebuffer_lock_hold_max_ns       = logged.framebuffer_lock_hold_max_ns;
    const uint64_t sdl_video_texture_uploads          = logged.sdl_video_texture_uploads;
    const uint64_t sdl_video_texture_upload_pixels    = logged.sdl_video_texture_upload_pixels;
    const uint64_t sdl_present_samples                = logged.sdl_present_samples;
    const uint64_t sdl_present_sum_ns                 = logged.sdl_present_sum_ns;
    const uint64_t sdl_present_max_ns                 = logged.sdl_present_max_ns;
    const uint64_t udp_gap_pressure_ms                = state.udp_gap_pressure_ns.load(std::memory_order_relaxed) / 1000000ull;

    if (audio_messages_rx != 0 || state.audio_stream_negotiated)
    {
        WD_LOG_STATS(
            "[client audio/min] messages=%llu packets=%llu kib=%.1f decode_failed=%llu discontinuities=%llu late_drops=%llu "
            "underflows=%llu audio_decode_q_drops=%llu av_holds=%llu av_drops=%llu video_q=%u/%u q_overflow=%llu "
            "video_decode_q=%u/%u/%u video_decode_q_drops=%llu phase=%u wait_keyframe=%u oldest_pts_us=%llu "
            "av_delta_samples=%lld av_hold_ms=%u/%u startup_timeouts=%llu startup_hold_ms=%u audio_state=%u playing=%s",
            static_cast<unsigned long long>(audio_messages_rx), static_cast<unsigned long long>(audio_packets_rx),
            static_cast<double>(audio_bytes_rx) / 1024.0, static_cast<unsigned long long>(audio_decode_failed),
            static_cast<unsigned long long>(audio_discontinuities), static_cast<unsigned long long>(audio_late_drops),
            static_cast<unsigned long long>(audio_underflows), static_cast<unsigned long long>(audio_decode_queue_drops),
            static_cast<unsigned long long>(audio_video_sync_holds), static_cast<unsigned long long>(audio_video_sync_drops),
            static_cast<unsigned>(video_queue_depth), static_cast<unsigned>(video_queue_depth_max),
            static_cast<unsigned long long>(video_queue_overflow_drops),
            static_cast<unsigned>(video_decode_queue_depth), static_cast<unsigned>(video_decode_queue_depth_max),
            static_cast<unsigned>(video_decode_queue_capacity), static_cast<unsigned long long>(video_decode_queue_drops),
            static_cast<unsigned>(video_decoder_phase), static_cast<unsigned>(video_waiting_keyframe),
            static_cast<unsigned long long>(video_oldest_pts_usec), static_cast<long long>(audio_video_delta_samples),
            static_cast<unsigned>(audio_video_sync_hold_current_ms), static_cast<unsigned>(audio_video_sync_hold_max_ms),
            static_cast<unsigned long long>(audio_video_startup_timeouts),
            static_cast<unsigned>(audio_video_startup_hold_ms), static_cast<unsigned>(audio_playback_state),
            client_audio_playback_is_playing(state.session.audio_playback) ? "yes" : "no");
    }

    const bool udp_activity = udp_packets != 0 || udp_bytes != 0 || completed != 0 || invalid != 0 || old_gen != 0 || ignored_probe != 0 ||
                              stale_session != 0 || udp_async_posted != 0 || udp_async_completed != 0 || udp_async_failed != 0 ||
                              udp_async_submit_failed != 0 || udp_async_cancels != 0 || udp_async_inflight_max != 0;
    if (udp_activity)
    {
        WD_LOG_STATS("[client udp/min] pkts=%llu kib=%.1f completed=%llu invalid=%llu probe=%llu stale_session=%llu old_gen=%llu "
                     "async_recv_submitted=%llu async_recv_completed=%llu async_recv_failed=%llu async_recv_submit_failed=%llu "
                     "async_recv_cancels=%llu async_recv_inflight_max=%llu interarrival_avg_ms=%.2f jitter_avg_ms=%.2f max_gap_ms=%.2f "
                     "kib_per_tile=%.2f compressed_kib_per_tile=%.2f pkts_per_tile=%.2f",
                     static_cast<unsigned long long>(udp_packets), static_cast<double>(udp_bytes) / 1024.0,
                     static_cast<unsigned long long>(completed), static_cast<unsigned long long>(invalid),
                     static_cast<unsigned long long>(ignored_probe), static_cast<unsigned long long>(stale_session),
                     static_cast<unsigned long long>(old_gen), static_cast<unsigned long long>(udp_async_posted),
                     static_cast<unsigned long long>(udp_async_completed), static_cast<unsigned long long>(udp_async_failed),
                     static_cast<unsigned long long>(udp_async_submit_failed), static_cast<unsigned long long>(udp_async_cancels),
                     static_cast<unsigned long long>(udp_async_inflight_max), avg_ms(udp_interarrival_sum_ns, udp_interarrival_samples),
                     avg_ms(udp_jitter_sum_ns, udp_jitter_samples), static_cast<double>(udp_interarrival_max_ns) / 1000000.0,
                     completed ? (static_cast<double>(udp_bytes) / 1024.0) / static_cast<double>(completed) : 0.0,
                     completed ? (static_cast<double>(completed_compressed) / 1024.0) / static_cast<double>(completed) : 0.0,
                     completed ? static_cast<double>(completed_packets) / static_cast<double>(completed) : 0.0);
    }

    if (invalid != 0 || logged.stale_epoch != 0)
    {
        WD_LOG_STATS(
            "[client udp-invalid/min] short=%llu header=%llu geometry=%llu fragment=%llu blit=%llu dirty_grid=%llu stale_epoch=%llu",
            static_cast<unsigned long long>(logged.invalid_short), static_cast<unsigned long long>(logged.invalid_header),
            static_cast<unsigned long long>(logged.invalid_geometry), static_cast<unsigned long long>(logged.invalid_fragment),
            static_cast<unsigned long long>(logged.invalid_blit), static_cast<unsigned long long>(logged.invalid_dirty_grid),
            static_cast<unsigned long long>(logged.stale_epoch));
    }

    if (udp_async_posted != 0 || udp_async_retired != 0 || udp_async_receiver_generations != 0 || udp_async_accounting_errors != 0)
    {
        WD_LOG_STATS("[client udp-async/min] submitted=%llu retired=%llu inflight=%llu prepared=%llu accounted=%llu generations=%llu "
                     "drained_reconfig=%llu cancelled_reconfig=%llu accounting_errors=%llu",
                     static_cast<unsigned long long>(udp_async_posted), static_cast<unsigned long long>(udp_async_retired),
                     static_cast<unsigned long long>(udp_async_inflight_current),
                     static_cast<unsigned long long>(udp_async_prepared_current),
                     static_cast<unsigned long long>(udp_async_retired + udp_async_inflight_current),
                     static_cast<unsigned long long>(udp_async_receiver_generations),
                     static_cast<unsigned long long>(udp_async_drained_on_reconfigure),
                     static_cast<unsigned long long>(udp_async_cancelled_on_reconfigure),
                     static_cast<unsigned long long>(udp_async_accounting_errors));
    }

    const bool repair_activity = partial_timeouts != 0 || partial_missing_packets != 0 || partial_retx_queued != 0 || summaries != 0 ||
                                 retx != 0 || summary_retx_queued != 0 || summary_retx_deferred != 0 || summary_retx_throttled != 0 ||
                                 summary_retx_stale_dropped != 0 || summary_retx_pressure_dropped != 0 || summary_promote_passes != 0 ||
                                 summary_to_retx_samples != 0 || retx_response_samples != 0;
    if (repair_activity)
    {
        WD_LOG_STATS(
            "[client repair/min] summaries=%llu retx_req=%llu summary_retx_tiles=%llu summary_deferred=%llu summary_throttled=%llu "
            "stale_drop=%llu pressure_deferred=%llu summary_promote=%llu summary_scan=%llu summary_candidates=%llu partial_timeouts=%llu "
            "missing_pkts=%llu partial_retx=%llu summary_to_retx_avg_ms=%.2f retx_response_avg_ms=%.2f",
            static_cast<unsigned long long>(summaries), static_cast<unsigned long long>(retx),
            static_cast<unsigned long long>(summary_retx_queued), static_cast<unsigned long long>(summary_retx_deferred),
            static_cast<unsigned long long>(summary_retx_throttled), static_cast<unsigned long long>(summary_retx_stale_dropped),
            static_cast<unsigned long long>(summary_retx_pressure_dropped), static_cast<unsigned long long>(summary_promote_passes),
            static_cast<unsigned long long>(summary_promote_scanned), static_cast<unsigned long long>(summary_promote_candidates),
            static_cast<unsigned long long>(partial_timeouts), static_cast<unsigned long long>(partial_missing_packets),
            static_cast<unsigned long long>(partial_retx_queued), avg_ms(summary_to_retx_sum_ns, summary_to_retx_samples),
            avg_ms(retx_response_sum_ns, retx_response_samples));
    }

    const bool input_activity = keys != 0 || pointer != 0 || input_events != 0 || input_channel_events != 0 ||
                                selection_channel_events != 0 || tcp_async_coalesced != 0;
    if (input_activity)
    {
        WD_LOG_STATS("[client input/min] keys=%llu pointer_queued=%llu pointer_coalesced=%llu input_events_queued=%llu input_channel=%llu "
                     "selection_channel=%llu",
                     static_cast<unsigned long long>(keys), static_cast<unsigned long long>(pointer),
                     static_cast<unsigned long long>(tcp_async_coalesced), static_cast<unsigned long long>(input_events),
                     static_cast<unsigned long long>(input_channel_events), static_cast<unsigned long long>(selection_channel_events));
    }

    const bool client_video_activity = video_messages_rx != 0 || video_data_frames_rx != 0 || video_frames_decoded != 0 ||
                                       video_frames_presented != 0 || video_decode_failed != 0 || video_publish_failed != 0 ||
                                       video_control_frames_rx != 0 || video_invalid_frames_rx != 0 || video_stale_frames_dropped != 0 ||
                                       video_need_keyframe_drops != 0 || video_decoder_resets != 0 || tile_frames_presented != 0;
    if (client_video_activity)
    {
        WD_LOG_STATS("[client video/min] messages=%llu data=%llu legacy_rx=%llu decoded=%llu presented=%llu tile_presented=%llu "
                     "control=%llu invalid=%llu stale_drop=%llu kib=%.1f decode_avg_ms=%.2f present_age_avg_ms=%.2f decode_failed=%llu "
                     "publish_failed=%llu need_keyframe_drops=%llu resets=%llu last_rx=%llu last_decoded=%llu last_presented=%llu",
                     static_cast<unsigned long long>(video_messages_rx), static_cast<unsigned long long>(video_data_frames_rx),
                     static_cast<unsigned long long>(video_frames_rx), static_cast<unsigned long long>(video_frames_decoded),
                     static_cast<unsigned long long>(video_frames_presented), static_cast<unsigned long long>(tile_frames_presented),
                     static_cast<unsigned long long>(video_control_frames_rx), static_cast<unsigned long long>(video_invalid_frames_rx),
                     static_cast<unsigned long long>(video_stale_frames_dropped), static_cast<double>(video_bytes_rx) / 1024.0,
                     avg_ms(video_decode_sum_ns, video_decode_samples), avg_ms(video_present_latency_sum_ns, video_present_latency_samples),
                     static_cast<unsigned long long>(video_decode_failed), static_cast<unsigned long long>(video_publish_failed),
                     static_cast<unsigned long long>(video_need_keyframe_drops), static_cast<unsigned long long>(video_decoder_resets),
                     static_cast<unsigned long long>(video_last_frame_id_rx),
                     static_cast<unsigned long long>(video_last_frame_id_decoded),
                     static_cast<unsigned long long>(video_last_frame_id_presented));
    }

    const bool tcp_async_activity = tcp_async_queued != 0 || tcp_async_completed != 0 || tcp_async_failed != 0 || tcp_async_overflow != 0 ||
                                    tcp_async_partial != 0 || tcp_async_coalesced != 0 || tcp_async_inflight_max != 0;
    if (tcp_async_activity)
    {
        WD_LOG_STATS(
            "[client tcp_async/min] queued=%llu completed=%llu failed=%llu overflow=%llu partial=%llu coalesced=%llu inflight_max=%llu",
            static_cast<unsigned long long>(tcp_async_queued), static_cast<unsigned long long>(tcp_async_completed),
            static_cast<unsigned long long>(tcp_async_failed), static_cast<unsigned long long>(tcp_async_overflow),
            static_cast<unsigned long long>(tcp_async_partial), static_cast<unsigned long long>(tcp_async_coalesced),
            static_cast<unsigned long long>(tcp_async_inflight_max));
    }

    uint64_t   timeout_ms       = state.tile_reassembly_timeout_ns.load(std::memory_order_relaxed) / 1000000ull;
    const bool latency_activity = timeout_updates != 0 || timeout_ms != state.stats_log.prev_timeout_ms ||
                                  udp_gap_pressure_ms != state.stats_log.prev_udp_gap_pressure_ms || tile_assembly_samples != 0 ||
                                  tile_present_samples != 0 || input_to_present_samples != 0 || input_seq_present_samples != 0;
    if (latency_activity)
    {
        WD_LOG_STATS("[client latency/min] tile_assembly_avg_ms=%.2f reassembly_timeout_ms=%llu udp_gap_pressure_ms=%llu "
                     "timeout_updates=%llu tile_present_avg_ms=%.2f input_to_present_avg_ms=%.2f input_seq_to_present_avg_ms=%.2f",
                     avg_ms(tile_assembly_sum_ns, tile_assembly_samples), static_cast<unsigned long long>(timeout_ms),
                     static_cast<unsigned long long>(udp_gap_pressure_ms), static_cast<unsigned long long>(timeout_updates),
                     avg_ms(tile_present_sum_ns, tile_present_samples), avg_ms(input_to_present_sum_ns, input_to_present_samples),
                     avg_ms(input_seq_present_sum_ns, input_seq_present_samples));
        state.stats_log.prev_timeout_ms          = timeout_ms;
        state.stats_log.prev_udp_gap_pressure_ms = udp_gap_pressure_ms;
    }

    if (sdl_render_frames != 0 || sdl_texture_upload_samples != 0 || sdl_present_samples != 0)
    {
        WD_LOG_STATS(
            "[client render/min] frames=%llu remote_frames=%llu empty_remote=%llu texture_full=%llu texture_partial=%llu video_full=%llu "
            "texture_locks=%llu texture_updates=%llu dirty_rects=%llu source_rects=%llu coalesced_rects=%llu bounds_uploads=%llu "
            "cost_full=%llu model_update_us=%.2f model_lock_us=%.2f model_pixel_ns=%.3f source_mpix=%.2f upload_mpix=%.2f "
            "video_upload_mpix=%.2f snapshot_mpix=%.2f snapshot_avg_ms=%.2f snapshot_max_ms=%.2f fb_direct=%llu fb_staged=%llu "
            "fb_lock_wait_avg_ms=%.3f fb_lock_wait_max_ms=%.3f fb_lock_hold_avg_ms=%.3f fb_lock_hold_max_ms=%.3f upload_avg_ms=%.2f "
            "upload_max_ms=%.2f present_avg_ms=%.2f present_max_ms=%.2f",
            static_cast<unsigned long long>(sdl_render_frames), static_cast<unsigned long long>(sdl_remote_frames),
            static_cast<unsigned long long>(sdl_empty_remote_wakeups), static_cast<unsigned long long>(sdl_texture_full_uploads),
            static_cast<unsigned long long>(sdl_texture_partial_uploads), static_cast<unsigned long long>(sdl_video_texture_uploads),
            static_cast<unsigned long long>(sdl_texture_lock_calls), static_cast<unsigned long long>(sdl_texture_update_calls),
            static_cast<unsigned long long>(sdl_texture_dirty_rects), static_cast<unsigned long long>(sdl_texture_source_dirty_rects),
            static_cast<unsigned long long>(sdl_texture_coalesced_dirty_rects), static_cast<unsigned long long>(sdl_texture_bounds_uploads),
            static_cast<unsigned long long>(sdl_texture_cost_full_uploads), static_cast<double>(sdl_texture_model_update_call_ns) / 1000.0,
            static_cast<double>(sdl_texture_model_lock_call_ns) / 1000.0, static_cast<double>(sdl_texture_model_pixel_cost_q16) / 65536.0,
            static_cast<double>(sdl_texture_source_pixels) / 1000000.0, static_cast<double>(sdl_texture_upload_pixels) / 1000000.0,
            static_cast<double>(sdl_video_texture_upload_pixels) / 1000000.0, static_cast<double>(framebuffer_snapshot_pixels) / 1000000.0,
            avg_ms(framebuffer_snapshot_sum_ns, framebuffer_snapshot_samples), static_cast<double>(framebuffer_snapshot_max_ns) / 1000000.0,
            static_cast<unsigned long long>(framebuffer_direct_uploads), static_cast<unsigned long long>(framebuffer_staged_uploads),
            avg_ms(framebuffer_lock_wait_sum_ns, framebuffer_lock_wait_samples),
            static_cast<double>(framebuffer_lock_wait_max_ns) / 1000000.0,
            avg_ms(framebuffer_lock_hold_sum_ns, framebuffer_lock_hold_samples),
            static_cast<double>(framebuffer_lock_hold_max_ns) / 1000000.0, avg_ms(sdl_texture_upload_sum_ns, sdl_texture_upload_samples),
            static_cast<double>(sdl_texture_upload_max_ns) / 1000000.0, avg_ms(sdl_present_sum_ns, sdl_present_samples),
            static_cast<double>(sdl_present_max_ns) / 1000000.0);
    }
}

} // namespace

void record_atomic_max(std::atomic<uint64_t>& value, uint64_t sample) {
    uint64_t current = value.load(std::memory_order_relaxed);

    while (sample > current && !value.compare_exchange_weak(current, sample, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
}

bool take_input_timestamp(ClientState& state, uint64_t sequence, uint64_t& timestamp_ns) {
    if (sequence == 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(state.input_timestamp_mutex);

    for (auto it = state.recent_input_timestamps.begin(); it != state.recent_input_timestamps.end(); ++it)
    {
        if (it->sequence == sequence)
        {
            timestamp_ns = it->timestamp_ns;
            state.recent_input_timestamps.erase(state.recent_input_timestamps.begin(), std::next(it));
            return timestamp_ns != 0;
        }
    }

    return false;
}

void sample_client_stats(ClientState& state, bool log_stats) {
    client_reap_async_sends(state);

    const uint64_t udp_packets                   = take_stat(state.stats.udp_packets_rx);
    const uint64_t udp_bytes                     = take_stat(state.stats.udp_bytes_rx);
    const uint64_t udp_interarrival_samples      = take_stat(state.stats.udp_interarrival_samples);
    const uint64_t udp_interarrival_sum_ns       = take_stat(state.stats.udp_interarrival_sum_ns);
    const uint64_t udp_jitter_samples            = take_stat(state.stats.udp_interarrival_jitter_samples);
    const uint64_t udp_jitter_sum_ns             = take_stat(state.stats.udp_interarrival_jitter_sum_ns);
    const uint64_t udp_interarrival_max_ns       = take_stat(state.stats.udp_interarrival_max_ns);
    const uint64_t invalid                       = take_stat(state.stats.udp_ignored_invalid);
    const uint64_t invalid_short                 = take_stat(state.stats.udp_invalid_short);
    const uint64_t invalid_header                = take_stat(state.stats.udp_invalid_header);
    const uint64_t invalid_geometry              = take_stat(state.stats.udp_invalid_geometry);
    const uint64_t invalid_fragment              = take_stat(state.stats.udp_invalid_fragment);
    const uint64_t invalid_blit                  = take_stat(state.stats.udp_invalid_blit);
    const uint64_t invalid_dirty_grid            = take_stat(state.stats.udp_invalid_dirty_grid);
    const uint64_t ignored_probe                 = take_stat(state.stats.udp_ignored_probe);
    const uint64_t stale_session                 = take_stat(state.stats.udp_ignored_stale_session);
    const uint64_t stale_epoch                   = take_stat(state.stats.udp_ignored_stale_epoch);
    const uint64_t old_gen                       = take_stat(state.stats.udp_ignored_old_generation);
    const uint64_t completed                     = take_stat(state.stats.udp_tiles_completed);
    const uint64_t completed_compressed          = take_stat(state.stats.udp_completed_compressed_bytes);
    const uint64_t completed_packets             = take_stat(state.stats.udp_completed_packets);
    const uint64_t partial_timeouts              = take_stat(state.stats.partial_tiles_timed_out);
    const uint64_t partial_missing_packets       = take_stat(state.stats.partial_tile_missing_packets);
    const uint64_t partial_retx_queued           = take_stat(state.stats.partial_tile_retx_queued);
    const uint64_t retx_response_samples         = take_stat(state.stats.retx_response_samples);
    const uint64_t retx_response_sum_ns          = take_stat(state.stats.retx_response_sum_ns);
    const uint64_t timeout_updates               = take_stat(state.stats.tile_reassembly_timeout_updates);
    const uint64_t summaries                     = take_stat(state.stats.tcp_summaries_rx);
    const uint64_t retx                          = take_stat(state.stats.tcp_retx_requests_tx);
    const uint64_t summary_retx_queued           = take_stat(state.stats.summary_retx_tiles_queued);
    const uint64_t summary_retx_deferred         = take_stat(state.stats.summary_retx_tiles_deferred);
    const uint64_t summary_retx_throttled        = take_stat(state.stats.summary_retx_tiles_throttled);
    const uint64_t summary_retx_stale_dropped    = take_stat(state.stats.summary_retx_tiles_stale_dropped);
    const uint64_t summary_retx_pressure_dropped = take_stat(state.stats.summary_retx_pressure_dropped);
    const uint64_t summary_promote_passes        = take_stat(state.stats.summary_promote_passes);
    const uint64_t summary_promote_scanned       = take_stat(state.stats.summary_promote_scanned);
    const uint64_t summary_promote_candidates    = take_stat(state.stats.summary_promote_candidates);
    const uint64_t summary_to_retx_samples       = take_stat(state.stats.summary_to_retx_samples);
    const uint64_t summary_to_retx_sum_ns        = take_stat(state.stats.summary_to_retx_sum_ns);
    const uint64_t keys                          = take_stat(state.stats.tcp_keyboard_tx);
    const uint64_t pointer                       = take_stat(state.stats.tcp_pointer_tx);
    const uint64_t input_events                  = take_stat(state.stats.tcp_input_events_tx);
    const uint64_t input_channel_events          = take_stat(state.stats.tcp_input_channel_tx);
    const uint64_t selection_channel_events      = take_stat(state.stats.tcp_selection_channel_tx);
    const uint64_t tcp_async_queued              = take_stat(state.stats.tcp_async_queued);
    const uint64_t tcp_async_completed           = take_stat(state.stats.tcp_async_completed);
    const uint64_t tcp_async_failed              = take_stat(state.stats.tcp_async_failed);
    const uint64_t tcp_async_overflow            = take_stat(state.stats.tcp_async_overflow);
    const uint64_t tcp_async_partial             = take_stat(state.stats.tcp_async_partial);
    const uint64_t tcp_async_coalesced           = take_stat(state.stats.tcp_async_coalesced);
    const uint64_t tcp_async_inflight_max        = take_stat(state.stats.tcp_async_inflight_max);
    const uint64_t video_frames_rx               = take_stat(state.stats.video_frames_rx);
    const uint64_t video_bytes_rx                = take_stat(state.stats.video_bytes_rx);
    const uint64_t video_frames_decoded          = take_stat(state.stats.video_frames_decoded);
    const uint64_t video_frames_presented        = take_stat(state.stats.video_frames_presented);
    const uint64_t video_decode_failed           = take_stat(state.stats.video_decode_failed);
    const uint64_t video_publish_failed          = take_stat(state.stats.video_publish_failed);
    const uint64_t video_control_frames_rx       = take_stat(state.stats.video_control_frames_rx);
    const uint64_t video_need_keyframe_drops     = take_stat(state.stats.video_need_keyframe_drops);
    const uint64_t video_decoder_resets          = take_stat(state.stats.video_decoder_resets);
    const uint64_t video_decode_samples          = take_stat(state.stats.video_decode_samples);
    const uint64_t video_decode_sum_ns           = take_stat(state.stats.video_decode_sum_ns);
    const uint64_t video_messages_rx             = take_stat(state.stats.video_messages_rx);
    const uint64_t video_data_frames_rx          = take_stat(state.stats.video_data_frames_rx);
    const uint64_t video_invalid_frames_rx       = take_stat(state.stats.video_invalid_frames_rx);
    const uint64_t video_stale_frames_dropped    = take_stat(state.stats.video_stale_frames_dropped);
    const uint64_t video_last_frame_id_rx        = state.stats.video_last_frame_id_rx.load(std::memory_order_relaxed);
    const uint64_t video_last_frame_id_decoded   = state.stats.video_last_frame_id_decoded.load(std::memory_order_relaxed);
    const uint64_t video_last_frame_id_presented = state.stats.video_last_frame_id_presented.load(std::memory_order_relaxed);
    const uint64_t video_present_latency_samples = take_stat(state.stats.video_present_latency_samples);
    const uint64_t video_present_latency_sum_ns  = take_stat(state.stats.video_present_latency_sum_ns);
    const uint64_t audio_video_sync_holds        = take_stat(state.stats.audio_video_sync_holds);
    const uint64_t audio_video_sync_drops        = take_stat(state.stats.audio_video_sync_drops);
    const uint64_t audio_video_startup_timeouts   = take_stat(state.stats.audio_video_startup_timeouts);
    const uint32_t audio_video_startup_hold_ms    = state.stats.audio_video_startup_hold_ms.load(std::memory_order_relaxed);
    const uint8_t  audio_playback_state           = state.stats.audio_playback_state.load(std::memory_order_relaxed);
    const uint64_t video_queue_overflow_drops    = take_stat(state.stats.video_queue_overflow_drops);
    const uint64_t video_decode_queue_drops      = take_stat(state.stats.video_decode_queue_drops);
    const uint32_t video_decode_queue_depth = state.stats.video_decode_queue_depth.load(std::memory_order_relaxed);
    const uint32_t video_decode_queue_depth_max = state.stats.video_decode_queue_depth_max.exchange(0, std::memory_order_relaxed);
    const uint16_t video_decode_queue_capacity = static_cast<uint16_t>(WD_CLIENT_VIDEO_DECODE_INPUT_QUEUE_CAPACITY);
    uint8_t video_decoder_phase = 0;
    uint8_t video_waiting_keyframe = 0;
    {
        std::lock_guard<std::mutex> lock(state.session.video_decoder_mutex);
        video_decoder_phase = static_cast<uint8_t>(state.session.video_phase);
    }
    {
        std::lock_guard<std::mutex> lock(state.session.media_queue_mutex);
        video_waiting_keyframe = state.session.video_decode_wait_keyframe ? 1u : 0u;
    }
    const uint32_t audio_video_sync_hold_current_ms = state.stats.audio_video_sync_hold_current_ms.load(std::memory_order_relaxed);
    const uint32_t audio_video_sync_hold_max_ms = state.stats.audio_video_sync_hold_max_ms.exchange(0, std::memory_order_relaxed);
    const uint32_t video_queue_depth_max =
        static_cast<uint32_t>(std::min<uint64_t>(take_stat(state.stats.video_queue_depth_max), UINT32_MAX));
    uint32_t video_queue_depth     = 0;
    uint64_t video_oldest_pts_usec = 0;
    {
        std::lock_guard<std::mutex> video_lock(state.video_frame_mutex);
        video_queue_depth                    = static_cast<uint32_t>(std::min<size_t>(state.video_present_queue.size(), UINT32_MAX));
        const ClientQueuedVideoFrame* oldest = state.video_present_queue.front();
        video_oldest_pts_usec                = oldest ? oldest->pts_usec : 0;
    }
    const int64_t  audio_video_delta_samples = state.stats.audio_video_delta_samples.load(std::memory_order_relaxed);
    const uint64_t tile_frames_presented           = take_stat(state.stats.tile_frames_presented);
    const uint64_t tile_content_epoch_presented     = state.stats.tile_content_epoch_presented.load(std::memory_order_relaxed);
    const uint64_t video_content_epoch_presented    = state.stats.video_content_epoch_presented.load(std::memory_order_relaxed);
    const uint64_t audio_messages_rx         = take_stat(state.stats.audio_messages_rx);
    const uint64_t audio_packets_rx          = take_stat(state.stats.audio_packets_rx);
    const uint64_t audio_bytes_rx            = take_stat(state.stats.audio_bytes_rx);
    const uint64_t audio_decode_failed       = take_stat(state.stats.audio_decode_failed);
    const uint64_t audio_decode_queue_drops   = take_stat(state.stats.audio_decode_queue_drops);
    const uint64_t audio_discontinuities     = take_stat(state.stats.audio_discontinuities);
    const uint64_t audio_late_drops          = take_stat(state.stats.audio_late_drops);
    const uint64_t audio_underflows          = take_stat(state.stats.audio_underflows);
    client_reap_async_udp_receives(state);
    const uint64_t udp_async_posted                   = take_stat(state.stats.udp_async_posted);
    const uint64_t udp_async_retired                  = take_stat(state.stats.udp_async_retired);
    const uint64_t udp_async_completed                = take_stat(state.stats.udp_async_completed);
    const uint64_t udp_async_failed                   = take_stat(state.stats.udp_async_failed);
    const uint64_t udp_async_submit_failed            = take_stat(state.stats.udp_async_submit_failed);
    const uint64_t udp_async_cancels                  = take_stat(state.stats.udp_async_cancels);
    const uint64_t udp_async_inflight_current         = state.stats.udp_async_inflight_current.load(std::memory_order_relaxed);
    const uint64_t udp_async_prepared_current         = state.stats.udp_async_prepared_current.load(std::memory_order_relaxed);
    const uint64_t udp_async_inflight_max             = take_stat(state.stats.udp_async_inflight_max);
    const uint64_t udp_async_drained_on_reconfigure   = take_stat(state.stats.udp_async_drained_on_reconfigure);
    const uint64_t udp_async_cancelled_on_reconfigure = take_stat(state.stats.udp_async_cancelled_on_reconfigure);
    const uint64_t udp_async_receiver_generations     = take_stat(state.stats.udp_async_receiver_generations);
    const uint64_t udp_async_accounting_errors        = take_stat(state.stats.udp_async_accounting_errors);
    const uint64_t tile_assembly_samples              = take_stat(state.stats.tile_assembly_samples);
    const uint64_t tile_assembly_sum_ns               = take_stat(state.stats.tile_assembly_sum_ns);
    const uint64_t tile_present_samples               = take_stat(state.stats.tile_present_latency_samples);
    const uint64_t tile_present_sum_ns                = take_stat(state.stats.tile_present_latency_sum_ns);
    const uint64_t input_to_present_samples           = take_stat(state.stats.input_to_present_latency_samples);
    const uint64_t input_to_present_sum_ns            = take_stat(state.stats.input_to_present_latency_sum_ns);
    const uint64_t input_seq_present_samples          = take_stat(state.stats.input_sequence_present_latency_samples);
    const uint64_t input_seq_present_sum_ns           = take_stat(state.stats.input_sequence_present_latency_sum_ns);
    const uint64_t sdl_render_frames                  = take_stat(state.stats.sdl_render_frames);
    const uint64_t sdl_remote_frames                  = take_stat(state.stats.sdl_remote_frames);
    const uint64_t sdl_empty_remote_wakeups           = take_stat(state.stats.sdl_empty_remote_wakeups);
    const uint64_t sdl_texture_full_uploads           = take_stat(state.stats.sdl_texture_full_uploads);
    const uint64_t sdl_texture_partial_uploads        = take_stat(state.stats.sdl_texture_partial_uploads);
    const uint64_t sdl_texture_dirty_rects            = take_stat(state.stats.sdl_texture_dirty_rects);
    const uint64_t sdl_texture_source_dirty_rects     = take_stat(state.stats.sdl_texture_source_dirty_rects);
    const uint64_t sdl_texture_coalesced_dirty_rects  = take_stat(state.stats.sdl_texture_coalesced_dirty_rects);
    const uint64_t sdl_texture_bounds_uploads         = take_stat(state.stats.sdl_texture_bounds_uploads);
    const uint64_t sdl_texture_cost_full_uploads      = take_stat(state.stats.sdl_texture_cost_full_uploads);
    const uint64_t sdl_texture_lock_calls             = take_stat(state.stats.sdl_texture_lock_calls);
    const uint64_t sdl_texture_update_calls           = take_stat(state.stats.sdl_texture_update_calls);
    const uint64_t sdl_texture_model_update_call_ns   = state.stats.sdl_texture_model_update_call_ns.load(std::memory_order_relaxed);
    const uint64_t sdl_texture_model_lock_call_ns     = state.stats.sdl_texture_model_lock_call_ns.load(std::memory_order_relaxed);
    const uint64_t sdl_texture_model_pixel_cost_q16   = state.stats.sdl_texture_model_pixel_cost_q16.load(std::memory_order_relaxed);
    const uint64_t sdl_texture_source_pixels          = take_stat(state.stats.sdl_texture_source_pixels);
    const uint64_t sdl_texture_upload_pixels          = take_stat(state.stats.sdl_texture_upload_pixels);
    const uint64_t sdl_texture_upload_samples         = take_stat(state.stats.sdl_texture_upload_samples);
    const uint64_t sdl_texture_upload_sum_ns          = take_stat(state.stats.sdl_texture_upload_sum_ns);
    const uint64_t sdl_texture_upload_max_ns          = take_stat(state.stats.sdl_texture_upload_max_ns);
    const uint64_t framebuffer_snapshot_pixels        = take_stat(state.stats.framebuffer_snapshot_pixels);
    const uint64_t framebuffer_snapshot_samples       = take_stat(state.stats.framebuffer_snapshot_samples);
    const uint64_t framebuffer_snapshot_sum_ns        = take_stat(state.stats.framebuffer_snapshot_sum_ns);
    const uint64_t framebuffer_snapshot_max_ns        = take_stat(state.stats.framebuffer_snapshot_max_ns);
    const uint64_t framebuffer_direct_uploads         = take_stat(state.stats.framebuffer_direct_uploads);
    const uint64_t framebuffer_staged_uploads         = take_stat(state.stats.framebuffer_staged_uploads);
    const uint64_t framebuffer_lock_wait_samples      = take_stat(state.stats.framebuffer_lock_wait_samples);
    const uint64_t framebuffer_lock_wait_sum_ns       = take_stat(state.stats.framebuffer_lock_wait_sum_ns);
    const uint64_t framebuffer_lock_wait_max_ns       = take_stat(state.stats.framebuffer_lock_wait_max_ns);
    const uint64_t framebuffer_lock_hold_samples      = take_stat(state.stats.framebuffer_lock_hold_samples);
    const uint64_t framebuffer_lock_hold_sum_ns       = take_stat(state.stats.framebuffer_lock_hold_sum_ns);
    const uint64_t framebuffer_lock_hold_max_ns       = take_stat(state.stats.framebuffer_lock_hold_max_ns);
    const uint64_t sdl_video_texture_uploads          = take_stat(state.stats.sdl_video_texture_uploads);
    const uint64_t sdl_video_texture_upload_pixels    = take_stat(state.stats.sdl_video_texture_upload_pixels);
    const uint64_t sdl_present_samples                = take_stat(state.stats.sdl_present_samples);
    const uint64_t sdl_present_sum_ns                 = take_stat(state.stats.sdl_present_sum_ns);
    const uint64_t sdl_present_max_ns                 = take_stat(state.stats.sdl_present_max_ns);

    ClientStatsSnapshot sample{};
    sample.udp_packets                        = udp_packets;
    sample.udp_bytes                          = udp_bytes;
    sample.udp_interarrival_samples           = udp_interarrival_samples;
    sample.udp_interarrival_sum_ns            = udp_interarrival_sum_ns;
    sample.udp_jitter_samples                 = udp_jitter_samples;
    sample.udp_jitter_sum_ns                  = udp_jitter_sum_ns;
    sample.udp_interarrival_max_ns            = udp_interarrival_max_ns;
    sample.invalid                            = invalid;
    sample.invalid_short                      = invalid_short;
    sample.invalid_header                     = invalid_header;
    sample.invalid_geometry                   = invalid_geometry;
    sample.invalid_fragment                   = invalid_fragment;
    sample.invalid_blit                       = invalid_blit;
    sample.invalid_dirty_grid                 = invalid_dirty_grid;
    sample.ignored_probe                      = ignored_probe;
    sample.stale_session                      = stale_session;
    sample.stale_epoch                        = stale_epoch;
    sample.old_gen                            = old_gen;
    sample.completed                          = completed;
    sample.completed_compressed               = completed_compressed;
    sample.completed_packets                  = completed_packets;
    sample.partial_timeouts                   = partial_timeouts;
    sample.partial_missing_packets            = partial_missing_packets;
    sample.partial_retx_queued                = partial_retx_queued;
    sample.retx_response_samples              = retx_response_samples;
    sample.retx_response_sum_ns               = retx_response_sum_ns;
    sample.timeout_updates                    = timeout_updates;
    sample.summaries                          = summaries;
    sample.retx                               = retx;
    sample.summary_retx_queued                = summary_retx_queued;
    sample.summary_retx_deferred              = summary_retx_deferred;
    sample.summary_retx_throttled             = summary_retx_throttled;
    sample.summary_retx_stale_dropped         = summary_retx_stale_dropped;
    sample.summary_retx_pressure_dropped      = summary_retx_pressure_dropped;
    sample.summary_promote_passes             = summary_promote_passes;
    sample.summary_promote_scanned            = summary_promote_scanned;
    sample.summary_promote_candidates         = summary_promote_candidates;
    sample.summary_to_retx_samples            = summary_to_retx_samples;
    sample.summary_to_retx_sum_ns             = summary_to_retx_sum_ns;
    sample.keys                               = keys;
    sample.pointer                            = pointer;
    sample.input_events                       = input_events;
    sample.input_channel_events               = input_channel_events;
    sample.selection_channel_events           = selection_channel_events;
    sample.tcp_async_queued                   = tcp_async_queued;
    sample.tcp_async_completed                = tcp_async_completed;
    sample.tcp_async_failed                   = tcp_async_failed;
    sample.tcp_async_overflow                 = tcp_async_overflow;
    sample.tcp_async_partial                  = tcp_async_partial;
    sample.tcp_async_coalesced                = tcp_async_coalesced;
    sample.tcp_async_inflight_max             = tcp_async_inflight_max;
    sample.video_frames_rx                    = video_frames_rx;
    sample.video_bytes_rx                     = video_bytes_rx;
    sample.video_frames_decoded               = video_frames_decoded;
    sample.video_frames_presented             = video_frames_presented;
    sample.video_decode_failed                = video_decode_failed;
    sample.video_publish_failed               = video_publish_failed;
    sample.video_control_frames_rx            = video_control_frames_rx;
    sample.video_need_keyframe_drops          = video_need_keyframe_drops;
    sample.video_decoder_resets               = video_decoder_resets;
    sample.video_decode_samples               = video_decode_samples;
    sample.video_decode_sum_ns                = video_decode_sum_ns;
    sample.video_messages_rx                  = video_messages_rx;
    sample.video_data_frames_rx               = video_data_frames_rx;
    sample.video_invalid_frames_rx            = video_invalid_frames_rx;
    sample.video_stale_frames_dropped         = video_stale_frames_dropped;
    sample.video_last_frame_id_rx             = video_last_frame_id_rx;
    sample.video_last_frame_id_decoded        = video_last_frame_id_decoded;
    sample.video_last_frame_id_presented      = video_last_frame_id_presented;
    sample.video_present_latency_samples      = video_present_latency_samples;
    sample.video_present_latency_sum_ns       = video_present_latency_sum_ns;
    sample.audio_video_sync_holds             = audio_video_sync_holds;
    sample.audio_video_sync_drops             = audio_video_sync_drops;
    sample.audio_video_startup_timeouts        = audio_video_startup_timeouts;
    sample.audio_video_startup_hold_ms         = audio_video_startup_hold_ms;
    sample.audio_playback_state                = audio_playback_state;
    sample.video_queue_overflow_drops         = video_queue_overflow_drops;
    sample.video_decode_queue_drops           = video_decode_queue_drops;
    sample.video_decode_queue_depth           = video_decode_queue_depth;
    sample.video_decode_queue_depth_max       = video_decode_queue_depth_max;
    sample.video_decode_queue_capacity        = video_decode_queue_capacity;
    sample.video_decoder_phase                = video_decoder_phase;
    sample.video_waiting_keyframe             = video_waiting_keyframe;
    sample.audio_video_sync_hold_current_ms   = audio_video_sync_hold_current_ms;
    sample.audio_video_sync_hold_max_ms       = audio_video_sync_hold_max_ms;
    sample.video_queue_depth                  = video_queue_depth;
    sample.video_queue_depth_max              = video_queue_depth_max;
    sample.video_oldest_pts_usec              = video_oldest_pts_usec;
    sample.audio_video_delta_samples          = audio_video_delta_samples;
    sample.tile_frames_presented              = tile_frames_presented;
    sample.tile_content_epoch_presented        = tile_content_epoch_presented;
    sample.video_content_epoch_presented       = video_content_epoch_presented;
    sample.audio_messages_rx                  = audio_messages_rx;
    sample.audio_packets_rx                   = audio_packets_rx;
    sample.audio_bytes_rx                     = audio_bytes_rx;
    sample.audio_decode_failed                = audio_decode_failed;
    sample.audio_decode_queue_drops           = audio_decode_queue_drops;
    sample.audio_discontinuities              = audio_discontinuities;
    sample.audio_late_drops                   = audio_late_drops;
    sample.audio_underflows                   = audio_underflows;
    sample.udp_async_posted                   = udp_async_posted;
    sample.udp_async_retired                  = udp_async_retired;
    sample.udp_async_completed                = udp_async_completed;
    sample.udp_async_failed                   = udp_async_failed;
    sample.udp_async_submit_failed            = udp_async_submit_failed;
    sample.udp_async_cancels                  = udp_async_cancels;
    sample.udp_async_inflight_current         = udp_async_inflight_current;
    sample.udp_async_prepared_current         = udp_async_prepared_current;
    sample.udp_async_inflight_max             = udp_async_inflight_max;
    sample.udp_async_drained_on_reconfigure   = udp_async_drained_on_reconfigure;
    sample.udp_async_cancelled_on_reconfigure = udp_async_cancelled_on_reconfigure;
    sample.udp_async_receiver_generations     = udp_async_receiver_generations;
    sample.udp_async_accounting_errors        = udp_async_accounting_errors;
    sample.tile_assembly_samples              = tile_assembly_samples;
    sample.tile_assembly_sum_ns               = tile_assembly_sum_ns;
    sample.tile_present_samples               = tile_present_samples;
    sample.tile_present_sum_ns                = tile_present_sum_ns;
    sample.input_to_present_samples           = input_to_present_samples;
    sample.input_to_present_sum_ns            = input_to_present_sum_ns;
    sample.input_seq_present_samples          = input_seq_present_samples;
    sample.input_seq_present_sum_ns           = input_seq_present_sum_ns;
    sample.sdl_render_frames                  = sdl_render_frames;
    sample.sdl_remote_frames                  = sdl_remote_frames;
    sample.sdl_empty_remote_wakeups           = sdl_empty_remote_wakeups;
    sample.sdl_texture_full_uploads           = sdl_texture_full_uploads;
    sample.sdl_texture_partial_uploads        = sdl_texture_partial_uploads;
    sample.sdl_texture_dirty_rects            = sdl_texture_dirty_rects;
    sample.sdl_texture_source_dirty_rects     = sdl_texture_source_dirty_rects;
    sample.sdl_texture_coalesced_dirty_rects  = sdl_texture_coalesced_dirty_rects;
    sample.sdl_texture_bounds_uploads         = sdl_texture_bounds_uploads;
    sample.sdl_texture_cost_full_uploads      = sdl_texture_cost_full_uploads;
    sample.sdl_texture_lock_calls             = sdl_texture_lock_calls;
    sample.sdl_texture_update_calls           = sdl_texture_update_calls;
    sample.sdl_texture_model_update_call_ns   = sdl_texture_model_update_call_ns;
    sample.sdl_texture_model_lock_call_ns     = sdl_texture_model_lock_call_ns;
    sample.sdl_texture_model_pixel_cost_q16   = sdl_texture_model_pixel_cost_q16;
    sample.sdl_texture_source_pixels          = sdl_texture_source_pixels;
    sample.sdl_texture_upload_pixels          = sdl_texture_upload_pixels;
    sample.sdl_texture_upload_samples         = sdl_texture_upload_samples;
    sample.sdl_texture_upload_sum_ns          = sdl_texture_upload_sum_ns;
    sample.sdl_texture_upload_max_ns          = sdl_texture_upload_max_ns;
    sample.framebuffer_snapshot_pixels        = framebuffer_snapshot_pixels;
    sample.framebuffer_snapshot_samples       = framebuffer_snapshot_samples;
    sample.framebuffer_snapshot_sum_ns        = framebuffer_snapshot_sum_ns;
    sample.framebuffer_snapshot_max_ns        = framebuffer_snapshot_max_ns;
    sample.framebuffer_direct_uploads         = framebuffer_direct_uploads;
    sample.framebuffer_staged_uploads         = framebuffer_staged_uploads;
    sample.framebuffer_lock_wait_samples      = framebuffer_lock_wait_samples;
    sample.framebuffer_lock_wait_sum_ns       = framebuffer_lock_wait_sum_ns;
    sample.framebuffer_lock_wait_max_ns       = framebuffer_lock_wait_max_ns;
    sample.framebuffer_lock_hold_samples      = framebuffer_lock_hold_samples;
    sample.framebuffer_lock_hold_sum_ns       = framebuffer_lock_hold_sum_ns;
    sample.framebuffer_lock_hold_max_ns       = framebuffer_lock_hold_max_ns;
    sample.sdl_video_texture_uploads          = sdl_video_texture_uploads;
    sample.sdl_video_texture_upload_pixels    = sdl_video_texture_upload_pixels;
    sample.sdl_present_samples                = sdl_present_samples;
    sample.sdl_present_sum_ns                 = sdl_present_sum_ns;
    sample.sdl_present_max_ns                 = sdl_present_max_ns;
    client_stats_accumulate(state.stats_log.totals, sample);

    const bool video_feedback_heartbeat = state.video_stream_negotiated || state.session.video_tcp_connected.load(std::memory_order_acquire);
    const bool feedback_activity =
        video_feedback_heartbeat || udp_packets != 0 || udp_bytes != 0 || completed != 0 || partial_timeouts != 0 || invalid != 0 ||
        old_gen != 0 || retx != 0 || udp_interarrival_samples != 0 || video_messages_rx != 0 || video_data_frames_rx != 0 ||
        video_frames_rx != 0 || video_frames_decoded != 0 || video_frames_presented != 0 || video_decode_failed != 0 ||
        video_publish_failed != 0 || video_control_frames_rx != 0 || video_invalid_frames_rx != 0 || video_stale_frames_dropped != 0 ||
        video_need_keyframe_drops != 0 || video_decoder_resets != 0 || audio_messages_rx != 0 || audio_packets_rx != 0 ||
        audio_decode_failed != 0 || audio_decode_queue_drops != 0 || audio_discontinuities != 0 || audio_late_drops != 0 || audio_video_sync_holds != 0 ||
        audio_video_sync_drops != 0 || audio_video_startup_timeouts != 0 || video_queue_depth != 0 || video_queue_overflow_drops != 0 || video_decode_queue_drops != 0 || tile_frames_presented != 0;

    if (feedback_activity)
    {
        wd_client_stats_payload feedback{};
        {
            std::lock_guard<std::mutex> lock(state.config_mutex);
            feedback.session_id       = state.config.session_id;
            feedback.connection_token = state.config.connection_token;
        }
        if (state.render_feedback_visible.load(std::memory_order_relaxed))
        {
            feedback.flags |= WD_CLIENT_STATS_RENDER_VISIBLE;
        }
        feedback.udp_packets_rx                  = udp_packets;
        feedback.udp_bytes_rx                    = udp_bytes;
        feedback.udp_tiles_completed             = completed;
        feedback.udp_completed_packets           = completed_packets;
        feedback.partial_tiles_timed_out         = partial_timeouts;
        feedback.udp_ignored_old_generation      = old_gen;
        feedback.retx_requests_tx                = retx;
        feedback.udp_interarrival_samples        = udp_interarrival_samples;
        feedback.udp_interarrival_sum_ns         = udp_interarrival_sum_ns;
        feedback.udp_interarrival_jitter_samples = udp_jitter_samples;
        feedback.udp_interarrival_jitter_sum_ns  = udp_jitter_sum_ns;
        feedback.udp_interarrival_max_ns         = udp_interarrival_max_ns;
        /* Render-pressure feedback must count presentations caused by remote
         * texture updates only. Local exposes, menus, and window-system redraws
         * do not represent capacity consumed by the tile stream. */
        feedback.render_frames                 = sdl_remote_frames;
        feedback.present_samples               = sdl_present_samples;
        feedback.present_sum_ns                = sdl_present_sum_ns;
        feedback.present_max_ns                = sdl_present_max_ns;
        feedback.input_present_samples         = input_seq_present_samples;
        feedback.input_present_sum_ns          = input_seq_present_sum_ns;
        feedback.video_frames_rx               = video_frames_rx;
        feedback.video_bytes_rx                = video_bytes_rx;
        feedback.video_frames_decoded          = video_frames_decoded;
        feedback.video_frames_presented        = video_frames_presented;
        feedback.video_decode_failed           = video_decode_failed;
        feedback.video_publish_failed          = video_publish_failed;
        feedback.video_control_frames_rx       = video_control_frames_rx;
        feedback.video_need_keyframe_drops     = video_need_keyframe_drops;
        feedback.video_decoder_resets          = video_decoder_resets;
        feedback.video_decode_samples          = video_decode_samples;
        feedback.video_decode_sum_ns           = video_decode_sum_ns;
        feedback.video_messages_rx             = video_messages_rx;
        feedback.video_data_frames_rx          = video_data_frames_rx;
        feedback.video_invalid_frames_rx       = video_invalid_frames_rx;
        feedback.video_stale_frames_dropped    = video_stale_frames_dropped;
        feedback.video_last_frame_id_rx        = video_last_frame_id_rx;
        feedback.video_last_frame_id_decoded   = video_last_frame_id_decoded;
        feedback.video_last_frame_id_presented = video_last_frame_id_presented;
        feedback.video_present_latency_samples = video_present_latency_samples;
        feedback.video_present_latency_sum_ns  = video_present_latency_sum_ns;
        feedback.audio_messages_rx             = audio_messages_rx;
        feedback.audio_packets_rx              = audio_packets_rx;
        feedback.audio_bytes_rx                = audio_bytes_rx;
        feedback.audio_decode_failed           = audio_decode_failed;
        feedback.audio_discontinuities         = audio_discontinuities;
        feedback.audio_late_drops              = audio_late_drops;
        feedback.audio_underflows              = audio_underflows;
        feedback.audio_video_sync_holds        = audio_video_sync_holds;
        feedback.audio_video_sync_drops        = audio_video_sync_drops;
        feedback.video_decode_queue_drops       = video_decode_queue_drops;
        feedback.video_decode_queue_depth       = video_decode_queue_depth;
        feedback.video_decode_queue_depth_max   = video_decode_queue_depth_max;
        feedback.video_decode_queue_capacity    = video_decode_queue_capacity;
        feedback.video_decoder_phase            = video_decoder_phase;
        feedback.video_waiting_keyframe          = video_waiting_keyframe;
        feedback.audio_video_sync_hold_current_ms = audio_video_sync_hold_current_ms;
        feedback.audio_video_sync_hold_max_ms     = audio_video_sync_hold_max_ms;
        feedback.audio_video_startup_timeouts   = audio_video_startup_timeouts;
        feedback.audio_video_startup_hold_ms    = audio_video_startup_hold_ms;
        feedback.audio_playback_state           = audio_playback_state;
        feedback.video_queue_overflow_drops    = video_queue_overflow_drops;
        feedback.video_queue_depth             = video_queue_depth;
        feedback.video_queue_depth_max         = video_queue_depth_max;
        feedback.video_oldest_pts_usec         = video_oldest_pts_usec;
        feedback.audio_video_delta_samples     = audio_video_delta_samples;
        feedback.tile_frames_presented         = tile_frames_presented;
        feedback.tile_content_epoch_presented   = tile_content_epoch_presented;
        feedback.video_content_epoch_presented  = video_content_epoch_presented;
        if (feedback.session_id != 0)
        {
            client_send_stats(state, feedback);
        }
    }

    update_udp_gap_pressure(state, udp_interarrival_max_ns, udp_interarrival_samples, udp_packets);
    if (!log_stats)
    {
        return;
    }

    const ClientStatsSnapshot logged = state.stats_log.totals;
    state.stats_log.totals           = {};
    log_client_stats_snapshot(state, logged);
}

} // namespace waydisplay
