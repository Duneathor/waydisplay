#include "wd_stream_pipeline_internal.h"

#include "waydisplay/wd_log.h"
#include "wd_audio_stream.h"
#include "wd_tile_policy.h"
#include "wd_video_encoder.h"

#include <stdint.h>

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

static void wd_stats_accumulate(struct wd_stats* dst, const struct wd_stats* src) {
    if (!dst || !src)
    {
        return;
    }

    dst->dirty_tiles += src->dirty_tiles;
    dst->udp_tiles_sent += src->udp_tiles_sent;
    dst->udp_fresh_tiles_sent += src->udp_fresh_tiles_sent;
    dst->udp_fresh_bytes_sent += src->udp_fresh_bytes_sent;
    dst->udp_retx_tiles_sent += src->udp_retx_tiles_sent;
    dst->udp_retx_bytes_sent += src->udp_retx_bytes_sent;
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
    dst->tile_choice_covered_base_tiles += src->tile_choice_covered_base_tiles;
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
    dst->stream_mode_bootstrap_suppressed_samples += src->stream_mode_bootstrap_suppressed_samples;
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
    dst->audio_captured_frames += src->audio_captured_frames;
    dst->audio_capture_overruns += src->audio_capture_overruns;
    dst->audio_encoded_packets += src->audio_encoded_packets;
    dst->audio_encoded_bytes += src->audio_encoded_bytes;
    dst->audio_queue_drops += src->audio_queue_drops;
    dst->audio_discontinuities += src->audio_discontinuities;
    dst->audio_encode_failures += src->audio_encode_failures;
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
    if (src->client_video_last_frame_id_decoded > dst->client_video_last_frame_id_decoded)
    {
        dst->client_video_last_frame_id_decoded = src->client_video_last_frame_id_decoded;
    }
    if (src->client_video_last_frame_id_presented > dst->client_video_last_frame_id_presented)
    {
        dst->client_video_last_frame_id_presented = src->client_video_last_frame_id_presented;
    }
    dst->client_video_present_latency_samples += src->client_video_present_latency_samples;
    dst->client_video_present_latency_sum_ns += src->client_video_present_latency_sum_ns;
    dst->client_audio_messages_rx += src->client_audio_messages_rx;
    dst->client_audio_packets_rx += src->client_audio_packets_rx;
    dst->client_audio_bytes_rx += src->client_audio_bytes_rx;
    dst->client_audio_decode_failed += src->client_audio_decode_failed;
    dst->client_audio_discontinuities += src->client_audio_discontinuities;
    dst->client_audio_late_drops += src->client_audio_late_drops;
    dst->client_audio_underflows += src->client_audio_underflows;
    dst->client_audio_video_sync_holds += src->client_audio_video_sync_holds;
    dst->client_audio_video_sync_drops += src->client_audio_video_sync_drops;
    dst->client_video_decode_queue_drops += src->client_video_decode_queue_drops;
    dst->client_video_decode_queue_depth = src->client_video_decode_queue_depth;
    if (src->client_video_decode_queue_depth_max > dst->client_video_decode_queue_depth_max)
    {
        dst->client_video_decode_queue_depth_max = src->client_video_decode_queue_depth_max;
    }
    dst->client_video_decode_queue_capacity = src->client_video_decode_queue_capacity;
    dst->client_video_decoder_phase = src->client_video_decoder_phase;
    dst->client_video_waiting_keyframe = src->client_video_waiting_keyframe;
    dst->client_audio_video_sync_hold_current_ms = src->client_audio_video_sync_hold_current_ms;
    if (src->client_audio_video_sync_hold_max_ms > dst->client_audio_video_sync_hold_max_ms)
    {
        dst->client_audio_video_sync_hold_max_ms = src->client_audio_video_sync_hold_max_ms;
    }
    dst->client_audio_video_startup_timeouts += src->client_audio_video_startup_timeouts;
    dst->client_audio_video_startup_hold_ms = src->client_audio_video_startup_hold_ms;
    dst->client_audio_playback_state = src->client_audio_playback_state;
    dst->client_video_queue_overflow_drops += src->client_video_queue_overflow_drops;
    dst->client_video_queue_depth = src->client_video_queue_depth;
    if (src->client_video_queue_depth_max > dst->client_video_queue_depth_max)
    {
        dst->client_video_queue_depth_max = src->client_video_queue_depth_max;
    }
    dst->client_video_oldest_pts_usec     = src->client_video_oldest_pts_usec;
    dst->client_audio_video_delta_samples = src->client_audio_video_delta_samples;
    dst->client_tile_frames_presented += src->client_tile_frames_presented;
    if (src->client_tile_content_epoch_presented > dst->client_tile_content_epoch_presented)
    {
        dst->client_tile_content_epoch_presented = src->client_tile_content_epoch_presented;
    }
    if (src->client_video_content_epoch_presented > dst->client_video_content_epoch_presented)
    {
        dst->client_video_content_epoch_presented = src->client_video_content_epoch_presented;
    }
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
    dst->udp_async_sqe_exhausted += src->udp_async_sqe_exhausted;
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

static uint64_t wd_counter_delta(uint64_t current, uint64_t* seen) {
    if (!seen)
    {
        return current;
    }
    const uint64_t delta = current >= *seen ? current - *seen : current;
    *seen                = current;
    return delta;
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

    struct wd_audio_stream_stats audio_stats;
    wd_audio_stream_get_stats(net->audio_stream, &audio_stats);
    s.audio_captured_frames += wd_counter_delta(audio_stats.captured_frames, &net->audio_captured_frames_seen);
    s.audio_capture_overruns += wd_counter_delta(audio_stats.capture_overruns, &net->audio_capture_overruns_seen);
    s.audio_encoded_packets += wd_counter_delta(audio_stats.encoded_packets, &net->audio_encoded_packets_seen);
    s.audio_encoded_bytes += wd_counter_delta(audio_stats.encoded_bytes, &net->audio_encoded_bytes_seen);
    s.audio_queue_drops += wd_counter_delta(audio_stats.queue_drops, &net->audio_queue_drops_seen);
    s.audio_discontinuities += wd_counter_delta(audio_stats.discontinuities, &net->audio_discontinuities_seen);
    s.audio_encode_failures += wd_counter_delta(audio_stats.encode_failures, &net->audio_encode_failures_seen);
    uint64_t            safe_link_kib_per_second       = net->stream_policy.safe_link_bytes_per_second / 1024ull;
    uint64_t            recent_link_kib_per_second     = net->stream_policy.recent_link_bytes_per_second / 1024ull;
    uint64_t            tile_media_rate_kib_per_second = net->stream_policy.tile_media_bytes_per_second / 1024ull;
    uint64_t            tile_fresh_kib_per_second      = net->stream_policy.adaptive_tile_fresh_bytes_per_second / 1024ull;
    uint64_t            tile_repair_kib_per_second     = net->stream_policy.tile_repair_bytes_per_second / 1024ull;
    uint64_t            video_kib_per_second           = net->stream_policy.video_bytes_per_second / 1024ull;
    uint64_t            control_kib_per_second         = net->stream_policy.control_bytes_per_second / 1024ull;
    uint64_t            audio_reserved_kib_per_second  = net->stream_policy.audio_reserved_bytes_per_second / 1024ull;
    uint64_t            audio_cap_kib_per_second       = net->stream_policy.audio_cap_bytes_per_second / 1024ull;
    uint64_t            overhead_kib_per_second        = net->stream_policy.overhead_bytes_per_second / 1024ull;
    uint16_t            requested_capture_fps          = net->stream_policy.requested_capture_fps;
    uint16_t            adaptive_capture_fps        = wd_stream_policy_effective_fps_locked(&net->stream_policy);
    uint16_t            compositor_refresh_hz       = (uint16_t)((server->output_refresh_mhz + 500u) / 1000u);
    uint16_t            capture_pacing_fps          = wd_stream_policy_capture_pacing_fps_locked(&net->stream_policy, compositor_refresh_hz);
    uint16_t            client_present_cap_fps      = requested_capture_fps != 0 ? requested_capture_fps : WD_DEFAULT_CAPTURE_FPS;
    bool                client_render_visible       = net->stream_policy.client_render_visible;
    enum wd_stream_mode stream_mode                 = net->stream_policy.stream_mode;
    uint16_t            tile_width                  = server->tile_width;
    uint16_t            tile_height                 = server->tile_height;
    bool                input_channel_connected     = net->input_tcp_fd >= 0;
    bool                selection_channel_connected = net->selection_tcp_fd >= 0;
    bool                video_channel_connected     = net->video_tcp_fd >= 0;
    bool                video_negotiated            = net->video_stream_negotiated;
    bool                video_encoder_available     = wd_video_encoder_available(net->video_encoder);
    uint8_t             video_mode                  = net->stream_policy.video_mode;
    uint8_t             video_min_dirty_percent     = net->stream_policy.video_min_dirty_percent;
    uint16_t            video_enter_seconds         = net->stream_policy.video_enter_seconds;
    uint8_t             video_exit_dirty_percent    = net->stream_policy.video_exit_dirty_percent;
    uint16_t            video_exit_seconds          = net->stream_policy.video_exit_seconds;
    uint32_t            video_bitrate_kib           = wd_stream_video_bitrate_kib_locked(&net->stream_policy);
    uint64_t            video_decode_ewma_ns        = net->stream_policy.video_decode_ewma_ns;
    uint16_t            video_decode_safe_fps       = net->stream_policy.video_decode_safe_fps;
    bool                planned_recovery_resume_video = net->stream_policy.planned_recovery_resume_video;
    uint8_t             planned_recovery_source_mode = net->stream_policy.planned_recovery_source_mode;
    uint64_t            tile_recovery_framebuffer_generation = net->stream_policy.tile_recovery_framebuffer_generation;
    bool                tile_recovery_live_damage_deferred = net->stream_policy.tile_recovery_live_damage_deferred;
    uint8_t             compression_benchmark_mode  = server->tile_compression_benchmark_mode;

    pthread_mutex_unlock(&net->lock);

    struct wd_stats_log_state* stats_log = &server->stats_log;
    wd_stats_accumulate(&stats_log->totals, &s);
    if (!log_stats)
    {
        return;
    }

    s = stats_log->totals;
    memset(&stats_log->totals, 0, sizeof(stats_log->totals));

    bool state_changed =
        !stats_log->have_prev_state || stats_log->prev_requested_capture_fps != requested_capture_fps ||
        stats_log->prev_adaptive_capture_fps != adaptive_capture_fps || stats_log->prev_capture_pacing_fps != capture_pacing_fps ||
        stats_log->prev_compositor_refresh_hz != compositor_refresh_hz ||
        stats_log->prev_client_present_cap_fps != client_present_cap_fps ||
        stats_log->prev_client_render_visible != client_render_visible ||
        stats_log->prev_safe_link_kib_per_second != safe_link_kib_per_second ||
        stats_log->prev_recent_link_kib_per_second != recent_link_kib_per_second ||
        stats_log->prev_tile_media_rate_kib_per_second != tile_media_rate_kib_per_second ||
        stats_log->prev_tile_fresh_kib_per_second != tile_fresh_kib_per_second ||
        stats_log->prev_tile_repair_kib_per_second != tile_repair_kib_per_second ||
        stats_log->prev_video_kib_per_second != video_kib_per_second ||
        stats_log->prev_control_kib_per_second != control_kib_per_second ||
        stats_log->prev_audio_reserved_kib_per_second != audio_reserved_kib_per_second ||
        stats_log->prev_audio_cap_kib_per_second != audio_cap_kib_per_second ||
        stats_log->prev_overhead_kib_per_second != overhead_kib_per_second || stats_log->prev_tile_width != tile_width ||
        stats_log->prev_tile_height != tile_height ||
        stats_log->prev_input_channel != input_channel_connected || stats_log->prev_selection_channel != selection_channel_connected ||
        stats_log->prev_video_channel != video_channel_connected || stats_log->prev_video_negotiated != video_negotiated ||
        stats_log->prev_video_encoder != video_encoder_available || stats_log->prev_video_mode != video_mode ||
        stats_log->prev_video_min_dirty_percent != video_min_dirty_percent || stats_log->prev_video_enter_seconds != video_enter_seconds ||
        stats_log->prev_video_exit_dirty_percent != video_exit_dirty_percent || stats_log->prev_video_exit_seconds != video_exit_seconds ||
        stats_log->prev_video_bitrate_kib != video_bitrate_kib ||
        stats_log->prev_video_decode_ewma_ns != video_decode_ewma_ns ||
        stats_log->prev_video_decode_safe_fps != video_decode_safe_fps ||
        stats_log->prev_planned_recovery_resume_video != planned_recovery_resume_video ||
        stats_log->prev_planned_recovery_source_mode != planned_recovery_source_mode ||
        stats_log->prev_tile_recovery_framebuffer_generation != tile_recovery_framebuffer_generation ||
        stats_log->prev_tile_recovery_live_damage_deferred != tile_recovery_live_damage_deferred ||
        stats_log->prev_stream_mode != stream_mode;

    if (state_changed)
    {
        WD_LOG_STATS("state: requested_capture_fps=%u adaptive_capture_fps=%u capture_pacing_fps=%u compositor_refresh_hz=%u "
                     "client_present_cap_fps=%u client_visible=%s stream_mode=%s owner=%s fresh_udp_tiles=%s tile_repair=%s video_mode=%s "
                     "video_bitrate_kib=%u video_min_dirty_pct=%u video_enter_seconds=%u video_exit_dirty_pct=%u video_exit_seconds=%u "
                     "video_decode_ewma_ms=%.2f video_decode_safe_fps=%u planned_resize_resume=%s resume_from=%s "
                     "recovery_framebuffer_generation=%llu recovery_damage_deferred=%s "
                     "link_safe_kib=%llu link_recent_kib=%llu tile_media_kib=%llu tile_fresh_kib=%llu tile_repair_kib=%llu "
                     "video_alloc_kib=%llu audio_need_kib=%llu audio_cap_kib=%llu control_kib=%llu overhead_kib=%llu "
                     "base_tile=%ux%u wire_tiles=128x64,64x64,32x32,16x16 tile_compression=%s input_channel=%s "
                     "selection_channel=%s video_negotiated=%s video_channel=%s video_encoder=%s",
                     (unsigned)requested_capture_fps, (unsigned)adaptive_capture_fps, (unsigned)capture_pacing_fps,
                     (unsigned)compositor_refresh_hz, (unsigned)client_present_cap_fps, client_render_visible ? "yes" : "no",
                     wd_stream_mode_name(stream_mode), wd_stream_mode_owner_name(stream_mode),
                     wd_stream_mode_video_owns_display(stream_mode) ? "paused" : "enabled",
                     wd_stream_mode_video_owns_display(stream_mode) ? "paused" : "enabled", wd_video_mode_name(video_mode),
                     (unsigned)video_bitrate_kib, (unsigned)video_min_dirty_percent, (unsigned)video_enter_seconds,
                     (unsigned)video_exit_dirty_percent, (unsigned)video_exit_seconds, (double)video_decode_ewma_ns / 1000000.0,
                     (unsigned)video_decode_safe_fps, planned_recovery_resume_video ? "yes" : "no",
                     wd_stream_mode_name((enum wd_stream_mode)planned_recovery_source_mode),
                     (unsigned long long)tile_recovery_framebuffer_generation,
                     tile_recovery_live_damage_deferred ? "yes" : "no", (unsigned long long)safe_link_kib_per_second,
                     (unsigned long long)recent_link_kib_per_second, (unsigned long long)tile_media_rate_kib_per_second,
                     (unsigned long long)tile_fresh_kib_per_second, (unsigned long long)tile_repair_kib_per_second,
                     (unsigned long long)video_kib_per_second, (unsigned long long)audio_reserved_kib_per_second,
                     (unsigned long long)audio_cap_kib_per_second, (unsigned long long)control_kib_per_second,
                     (unsigned long long)overhead_kib_per_second, (unsigned)tile_width, (unsigned)tile_height,
                     wd_tile_compression_benchmark_mode_name(compression_benchmark_mode),
                     input_channel_connected ? "yes" : "no", selection_channel_connected ? "yes" : "no", video_negotiated ? "yes" : "no",
                     video_channel_connected ? "yes" : "no", video_encoder_available ? "yes" : "no");

        stats_log->have_prev_state                      = true;
        stats_log->prev_requested_capture_fps           = requested_capture_fps;
        stats_log->prev_adaptive_capture_fps            = adaptive_capture_fps;
        stats_log->prev_capture_pacing_fps              = capture_pacing_fps;
        stats_log->prev_compositor_refresh_hz           = compositor_refresh_hz;
        stats_log->prev_client_present_cap_fps          = client_present_cap_fps;
        stats_log->prev_client_render_visible           = client_render_visible;
        stats_log->prev_safe_link_kib_per_second        = safe_link_kib_per_second;
        stats_log->prev_recent_link_kib_per_second      = recent_link_kib_per_second;
        stats_log->prev_tile_media_rate_kib_per_second  = tile_media_rate_kib_per_second;
        stats_log->prev_tile_fresh_kib_per_second       = tile_fresh_kib_per_second;
        stats_log->prev_tile_repair_kib_per_second      = tile_repair_kib_per_second;
        stats_log->prev_video_kib_per_second            = video_kib_per_second;
        stats_log->prev_control_kib_per_second          = control_kib_per_second;
        stats_log->prev_audio_reserved_kib_per_second   = audio_reserved_kib_per_second;
        stats_log->prev_audio_cap_kib_per_second        = audio_cap_kib_per_second;
        stats_log->prev_overhead_kib_per_second         = overhead_kib_per_second;
        stats_log->prev_tile_width                      = tile_width;
        stats_log->prev_tile_height                     = tile_height;
        stats_log->prev_input_channel            = input_channel_connected;
        stats_log->prev_selection_channel        = selection_channel_connected;
        stats_log->prev_video_channel            = video_channel_connected;
        stats_log->prev_video_negotiated         = video_negotiated;
        stats_log->prev_video_encoder            = video_encoder_available;
        stats_log->prev_video_mode               = video_mode;
        stats_log->prev_video_min_dirty_percent  = video_min_dirty_percent;
        stats_log->prev_video_enter_seconds      = video_enter_seconds;
        stats_log->prev_video_exit_dirty_percent = video_exit_dirty_percent;
        stats_log->prev_video_exit_seconds       = video_exit_seconds;
        stats_log->prev_video_bitrate_kib                   = video_bitrate_kib;
        stats_log->prev_video_decode_ewma_ns                  = video_decode_ewma_ns;
        stats_log->prev_video_decode_safe_fps                 = video_decode_safe_fps;
        stats_log->prev_planned_recovery_resume_video         = planned_recovery_resume_video;
        stats_log->prev_planned_recovery_source_mode          = planned_recovery_source_mode;
        stats_log->prev_tile_recovery_framebuffer_generation  = tile_recovery_framebuffer_generation;
        stats_log->prev_tile_recovery_live_damage_deferred    = tile_recovery_live_damage_deferred;
        stats_log->prev_stream_mode                           = stream_mode;
    }

    bool stream_mode_activity = s.stream_mode_full_refresh_samples != 0 || s.stream_mode_bootstrap_suppressed_samples != 0 ||
                                (s.stream_mode_frame_samples != 0 &&
                                 (s.stream_mode_dirty_coverage_per_mille_sum != 0 || s.stream_mode_pending_coverage_per_mille_sum != 0 ||
                                  s.stream_mode_budget_pressure_frames != 0 || adaptive_capture_fps < requested_capture_fps));
    if (stream_mode_activity)
    {
        const double   total_tiles              = server->total_tiles != 0 ? (double)server->total_tiles : 1.0;
        const uint64_t sample_count             = s.stream_mode_frame_samples != 0 ? s.stream_mode_frame_samples : 1ull;
        const double   dirty_avg_pct            = wd_stream_coverage_pct(s.stream_mode_dirty_coverage_per_mille_sum / sample_count);
        const double   dirty_peak_pct           = wd_stream_coverage_pct(s.stream_mode_dirty_coverage_per_mille_peak);
        const double   pending_avg_pct          = wd_stream_coverage_pct(s.stream_mode_pending_coverage_per_mille_sum / sample_count);
        const double   pending_peak_pct         = wd_stream_coverage_pct(s.stream_mode_pending_coverage_per_mille_peak);
        const double   budget_pressure_pct      = (double)s.stream_mode_budget_pressure_frames * 100.0 / (double)sample_count;
        const double   wire_avg_bytes           = s.udp_tiles_sent ? (double)s.udp_bytes_sent / (double)s.udp_tiles_sent : 0.0;
        const double   estimated_full_frame_mib = wire_avg_bytes * total_tiles / 1024.0 / 1024.0;
        const double   estimated_budget_fps =
            wire_avg_bytes > 0.0 ? ((double)tile_fresh_kib_per_second * 1024.0) / (wire_avg_bytes * total_tiles) : 0.0;
        const uint32_t fallback_wire_per_base =
            WD_BASE_TILE_WIDTH * WD_BASE_TILE_HEIGHT * WD_BYTES_PER_PIXEL + WD_UDP_TILE_HEADER_MAX_SIZE;
        const uint64_t predicted_fresh_bytes_per_second = wd_tile_estimate_demand_bytes_per_second(
            s.stream_mode_frame_samples, s.stream_mode_dirty_coverage_per_mille_sum, s.tile_choice_chosen_wire_sum,
            s.tile_choice_covered_base_tiles, server->total_tiles, requested_capture_fps, fallback_wire_per_base);
        const double predicted_fresh_budget_pct = tile_fresh_kib_per_second != 0
                                                      ? (double)predicted_fresh_bytes_per_second * 100.0 /
                                                            ((double)tile_fresh_kib_per_second * 1024.0)
                                                      : 0.0;

        WD_LOG_STATS(
            "stream-mode/min: capture_samples=%llu changed_samples=%llu full_refresh_samples=%llu bootstrap_suppressed=%llu "
            "dirty_avg_pct=%.1f dirty_peak_pct=%.1f pending_avg_pct=%.1f pending_peak_pct=%.1f budget_pressure_frames=%llu "
            "full_refresh_budget_pressure_frames=%llu budget_pressure_pct=%.1f video_mode=%s video_min_dirty_pct=%u video_enter_seconds=%u "
            "video_exit_dirty_pct=%u video_exit_seconds=%u est_tile_full_refresh_mib=%.2f est_full_refreshes_per_sec=%.1f "
            "predicted_fresh_kib=%.1f predicted_fresh_budget_pct=%.1f",
            (unsigned long long)s.stream_mode_frame_samples, (unsigned long long)s.stream_mode_changed_frame_samples,
            (unsigned long long)s.stream_mode_full_refresh_samples, (unsigned long long)s.stream_mode_bootstrap_suppressed_samples,
            dirty_avg_pct, dirty_peak_pct, pending_avg_pct, pending_peak_pct, (unsigned long long)s.stream_mode_budget_pressure_frames,
            (unsigned long long)s.stream_mode_full_refresh_budget_pressure_frames, budget_pressure_pct, wd_video_mode_name(video_mode),
            (unsigned)video_min_dirty_percent, (unsigned)video_enter_seconds, (unsigned)video_exit_dirty_percent,
            (unsigned)video_exit_seconds, estimated_full_frame_mib, estimated_budget_fps,
            (double)predicted_fresh_bytes_per_second / 1024.0, predicted_fresh_budget_pct);
    }

    bool video_activity =
        s.dirty_tiles != 0 || s.dirty_tiles_stale_skipped != 0 || s.udp_tiles_sent != 0 || s.udp_fresh_tiles_sent != 0 ||
        s.udp_retx_tiles_sent != 0 || s.udp_packets_sent != 0 || s.udp_bytes_sent != 0 || s.udp_send_pressure_drops != 0 ||
        s.udp_async_send_failed != 0 || s.udp_async_queued != 0 || s.udp_async_completed != 0 || s.udp_async_completion_failed != 0 ||
        s.udp_async_sqe_exhausted != 0 || s.tile_choice_compressed != 0 || s.tile_choice_uncompressed != 0 || s.compression_attempts != 0 ||
        s.compression_entropy_skips != 0 || s.compression_adaptive_skips != 0 || s.compression_nonwins != 0 ||
        s.compression_forced_choices != 0 || s.dirty_queue_age_samples != 0 || s.retx_queue_age_samples != 0 ||
        s.dirty_region_probes != 0 || s.dirty_region_hits != 0 || s.dirty_budget_blocked != 0 || s.partial_tile_sends != 0 ||
        s.dirty_detect_ns != 0 || s.framebuffer_diff_candidates != 0 || s.dirty_region_select_ns != 0 || s.tile_encode_ns != 0 ||
        s.summary_build_ns != 0 || s.udp_send_ns != 0 || s.encode_jobs_submitted != 0 || s.encode_jobs_completed != 0 ||
        s.encode_jobs_stale != 0 || s.encode_wait_ns != 0 || s.encode_batches != 0;
    if (video_activity)
    {
        uint64_t choices = s.tile_choice_compressed + s.tile_choice_uncompressed;
        WD_LOG_STATS(
            "tile-stream/min: dirty=%llu stale_skip=%llu udp_tiles=%llu fresh=%llu retx=%llu pkts=%llu kib=%.1f "
            "fresh_kib=%.1f repair_kib=%.1f wire_avg_B=%.1f "
            "comp_sent=%llu uncomp_sent=%llu comp_payload_avg_B=%.1f uncomp_payload_avg_B=%.1f choice_comp=%llu choice_uncomp=%llu "
            "choice_comp_payload_avg_B=%.1f choice_raw_payload_avg_B=%.1f choice_comp_wire_avg_B=%.1f choice_uncomp_wire_avg_B=%.1f "
            "choice_chosen_wire_avg_B=%.1f choice_saved_kib=%.1f pressure_drops=%llu async_queued=%llu async_completed=%llu "
            "async_failed=%llu async_completion_failed=%llu async_fallback=%llu async_inflight_max=%llu submit_calls=%llu "
            "partial_submits=%llu pkts_per_submit=%.2f zstd_mode=%s zstd_attempts=%llu zstd_wins=%llu zstd_nonwins=%llu zstd_forced=%llu "
            "zstd_entropy_skip=%llu zstd_adaptive_skip=%llu zstd_ms=%.2f zstd_saved_kib=%.1f dirty_q_avg_ms=%.2f retx_q_avg_ms=%.2f "
            "dirty_region_probes=%llu dirty_region_hits=%llu dirty_budget_blocked=%llu dirty_budget_blocked_full_refresh=%llu "
            "partial_tiles=%llu partial_pkts=%llu detect_ms=%.2f diff_candidates=%llu diff_changed=%llu diff_unchanged=%llu diff_full=%llu "
            "diff_ms=%.2f region_pick_ms=%.2f encode_ms=%.2f udp_send_ms=%.2f summary_ms=%.2f "
            "tile_sizes=128x64:%llu,64x64:%llu,32x32:%llu,16x16:%llu encode_jobs=%llu/%llu stale=%llu encode_wait_ms=%.2f "
            "encode_worker_ms=%.2f encode_batches=%llu encode_batch_peak=%llu encode_workers_avg=%.1f encode_wakeups=%llu",
            (unsigned long long)s.dirty_tiles, (unsigned long long)s.dirty_tiles_stale_skipped, (unsigned long long)s.udp_tiles_sent,
            (unsigned long long)s.udp_fresh_tiles_sent, (unsigned long long)s.udp_retx_tiles_sent, (unsigned long long)s.udp_packets_sent,
            (double)s.udp_bytes_sent / 1024.0, (double)s.udp_fresh_bytes_sent / 1024.0,
            (double)s.udp_retx_bytes_sent / 1024.0,
            s.udp_tiles_sent ? (double)s.udp_bytes_sent / (double)s.udp_tiles_sent : 0.0,
            (unsigned long long)s.udp_compressed_tiles_sent, (unsigned long long)s.udp_uncompressed_tiles_sent,
            s.udp_compressed_tiles_sent ? (double)s.udp_compressed_tile_bytes_sent / (double)s.udp_compressed_tiles_sent : 0.0,
            s.udp_uncompressed_tiles_sent ? (double)s.udp_uncompressed_tile_bytes_sent / (double)s.udp_uncompressed_tiles_sent : 0.0,
            (unsigned long long)s.tile_choice_compressed, (unsigned long long)s.tile_choice_uncompressed,
            choices ? (double)s.tile_choice_compressed_payload_sum / (double)choices : 0.0,
            choices ? (double)s.tile_choice_uncompressed_payload_sum / (double)choices : 0.0,
            choices ? (double)s.tile_choice_compressed_wire_sum / (double)choices : 0.0,
            choices ? (double)s.tile_choice_uncompressed_wire_sum / (double)choices : 0.0,
            choices ? (double)s.tile_choice_chosen_wire_sum / (double)choices : 0.0, (double)s.tile_choice_saved_wire_sum / 1024.0,
            (unsigned long long)s.udp_send_pressure_drops, (unsigned long long)s.udp_async_queued,
            (unsigned long long)s.udp_async_completed, (unsigned long long)s.udp_async_send_failed,
            (unsigned long long)s.udp_async_completion_failed, (unsigned long long)s.udp_async_sqe_exhausted,
            (unsigned long long)s.udp_async_inflight_max, (unsigned long long)s.udp_async_submit_calls,
            (unsigned long long)s.udp_async_partial_submits,
            s.udp_async_submit_calls ? (double)s.udp_async_queued / (double)s.udp_async_submit_calls : 0.0,
            wd_tile_compression_benchmark_mode_name(compression_benchmark_mode), (unsigned long long)s.compression_attempts,
            (unsigned long long)s.compression_wins, (unsigned long long)s.compression_nonwins,
            (unsigned long long)s.compression_forced_choices, (unsigned long long)s.compression_entropy_skips,
            (unsigned long long)s.compression_adaptive_skips, (double)s.compression_ns / 1000000.0,
            (double)s.compression_saved_wire_bytes / 1024.0, wd_avg_ms(s.dirty_queue_age_sum_ns, s.dirty_queue_age_samples),
            wd_avg_ms(s.retx_queue_age_sum_ns, s.retx_queue_age_samples), (unsigned long long)s.dirty_region_probes,
            (unsigned long long)s.dirty_region_hits, (unsigned long long)s.dirty_budget_blocked,
            (unsigned long long)s.dirty_budget_blocked_full_refresh, (unsigned long long)s.partial_tile_sends,
            (unsigned long long)s.partial_tile_packets_sent, (double)s.dirty_detect_ns / 1000000.0,
            (unsigned long long)s.framebuffer_diff_candidates, (unsigned long long)s.framebuffer_diff_changed,
            (unsigned long long)s.framebuffer_diff_unchanged, (unsigned long long)s.framebuffer_diff_full_refreshes,
            (double)s.framebuffer_diff_ns / 1000000.0, (double)s.dirty_region_select_ns / 1000000.0, (double)s.tile_encode_ns / 1000000.0,
            (double)s.udp_send_ns / 1000000.0, (double)s.summary_build_ns / 1000000.0, (unsigned long long)s.tile_size_128x64_sent,
            (unsigned long long)s.tile_size_64x64_sent, (unsigned long long)s.tile_size_32x32_sent,
            (unsigned long long)s.tile_size_16x16_sent, (unsigned long long)s.encode_jobs_completed,
            (unsigned long long)s.encode_jobs_submitted, (unsigned long long)s.encode_jobs_stale, (double)s.encode_wait_ns / 1000000.0,
            (double)s.encode_worker_ns / 1000000.0, (unsigned long long)s.encode_batches, (unsigned long long)s.encode_batch_jobs_peak,
            s.encode_batches ? (double)s.encode_worker_threads / (double)s.encode_batches : 0.0,
            (unsigned long long)s.encode_thread_wakeups);
    }

    if (s.server_frame_timer_samples != 0 || s.server_render_readback_samples != 0)
    {
        WD_LOG_STATS(
            "server-loop/min: service_ticks=%llu tick_avg_ms=%.2f tick_max_ms=%.2f render_readback=%llu render_readback_avg_ms=%.2f "
            "render_readback_max_ms=%.2f scene_promote=%llu render_idle=%llu render_failed=%llu encode_avg_ms=%.2f",
            (unsigned long long)s.server_frame_timer_samples,
            s.server_frame_timer_samples ? (double)s.server_frame_timer_sum_ns / (double)s.server_frame_timer_samples / 1000000.0 : 0.0,
            (double)s.server_frame_timer_max_ns / 1000000.0, (unsigned long long)s.server_render_readback_samples,
            s.server_render_readback_samples
                ? (double)s.server_render_readback_sum_ns / (double)s.server_render_readback_samples / 1000000.0
                : 0.0,
            (double)s.server_render_readback_max_ns / 1000000.0, (unsigned long long)s.server_scene_damage_promotions,
            (unsigned long long)s.server_render_idle_results, (unsigned long long)s.server_render_failed_results,
            s.video_frame_attempts ? (double)s.video_encode_ns / (double)s.video_frame_attempts / 1000000.0 : 0.0);
    }

    bool video_stream_activity = s.video_frames_published != 0 || s.video_frames_superseded != 0 || s.video_worker_stale_drops != 0 ||
                                 s.video_tile_detection_skipped != 0 || s.video_frame_attempts != 0 || s.video_frames_tx != 0 ||
                                 s.video_keyframe_attempts != 0 || s.video_keyframes_tx != 0 || s.video_tcp_bytes_tx != 0 ||
                                 s.video_encode_failed != 0 || s.video_tcp_send_failed != 0 || s.video_keyframe_skipped_pending != 0 ||
                                 s.video_control_frames_tx != 0 || s.video_end_of_stream_tx != 0 || s.video_resize_resets != 0 ||
                                 s.video_resets != 0;
    if (video_stream_activity)
    {
        WD_LOG_STATS(
            "video-stream/min: mode=%s configured_bitrate_kib=%u published=%llu superseded=%llu worker_stale=%llu tile_detect_skipped=%llu "
            "publish_copy_avg_ms=%.2f worker_queue_avg_ms=%.2f frame_attempts=%llu frames_tx=%llu keyframe_attempts=%llu keyframes_tx=%llu "
            "control_tx=%llu eos_tx=%llu tcp_kib=%.1f encode_ms=%.2f encode_failed=%llu tcp_send_failed=%llu skipped_pending=%llu "
            "resets=%llu resize_resets=%llu",
            wd_video_mode_name(video_mode), (unsigned)video_bitrate_kib, (unsigned long long)s.video_frames_published,
            (unsigned long long)s.video_frames_superseded, (unsigned long long)s.video_worker_stale_drops,
            (unsigned long long)s.video_tile_detection_skipped,
            s.video_publish_copy_samples ? (double)s.video_publish_copy_ns / (double)s.video_publish_copy_samples / 1000000.0 : 0.0,
            s.video_worker_queue_samples ? (double)s.video_worker_queue_ns / (double)s.video_worker_queue_samples / 1000000.0 : 0.0,
            (unsigned long long)s.video_frame_attempts, (unsigned long long)s.video_frames_tx,
            (unsigned long long)s.video_keyframe_attempts, (unsigned long long)s.video_keyframes_tx,
            (unsigned long long)s.video_control_frames_tx, (unsigned long long)s.video_end_of_stream_tx,
            (double)s.video_tcp_bytes_tx / 1024.0, (double)s.video_encode_ns / 1000000.0, (unsigned long long)s.video_encode_failed,
            (unsigned long long)s.video_tcp_send_failed, (unsigned long long)s.video_keyframe_skipped_pending,
            (unsigned long long)s.video_resets, (unsigned long long)s.video_resize_resets);
    }

    bool repair_activity = s.retx_req_rx != 0 || s.retx_tiles_req != 0 || s.retx_req_ignored_live != 0 ||
                           s.retx_req_stale_generation != 0 || s.retx_req_upgraded_generation != 0 ||
                           s.retx_tiles_superseded_by_fresh != 0 || s.tcp_summary_tx != 0 || s.tcp_summary_delta_tx != 0 ||
                           s.tcp_summary_delta_tiles != 0 || s.tcp_summary_coalesced != 0 || s.tcp_summary_repair_backoff != 0 ||
                           s.tcp_summary_budget_interval_ns != 0 || s.rate_decreases != 0 || s.rate_increases != 0 ||
                           s.frame_rate_downshifts != 0 || s.frame_rate_upshifts != 0;
    if (repair_activity)
    {
        WD_LOG_STATS(
            "repair/min: summaries=%llu full=%llu delta=%llu delta_tiles=%llu summary_coalesced=%llu summary_interval_ms=%llu "
            "repair_backoff=%llu retx_req=%llu retx_tiles=%llu stale_drop=%llu stale_upgraded=%llu ignored_live=%llu superseded=%llu "
            "rate_down=%llu rate_up=%llu capture_down=%llu capture_up=%llu",
            (unsigned long long)s.tcp_summary_tx, (unsigned long long)s.tcp_summary_full_tx, (unsigned long long)s.tcp_summary_delta_tx,
            (unsigned long long)s.tcp_summary_delta_tiles, (unsigned long long)s.tcp_summary_coalesced,
            (unsigned long long)(s.tcp_summary_budget_interval_ns / 1000000ull), (unsigned long long)s.tcp_summary_repair_backoff,
            (unsigned long long)s.retx_req_rx, (unsigned long long)s.retx_tiles_req, (unsigned long long)s.retx_req_stale_generation,
            (unsigned long long)s.retx_req_upgraded_generation, (unsigned long long)s.retx_req_ignored_live,
            (unsigned long long)s.retx_tiles_superseded_by_fresh, (unsigned long long)s.rate_decreases,
            (unsigned long long)s.rate_increases, (unsigned long long)s.frame_rate_downshifts, (unsigned long long)s.frame_rate_upshifts);
    }

    bool client_activity = s.client_tiles_completed != 0 || s.client_udp_bytes_rx != 0 || s.client_partial_tiles_timed_out != 0 ||
                           s.client_old_generation_tiles != 0 || s.client_retx_requests_tx != 0 || s.client_udp_interarrival_samples != 0 ||
                           s.client_render_frames != 0 || s.client_present_samples != 0 || s.client_input_present_samples != 0 ||
                           s.client_render_visible_reports != 0 || s.client_render_hidden_reports != 0 ||
                           s.client_tile_frames_presented != 0;
    if (client_activity)
    {
        WD_LOG_STATS("client/min: reports=%llu visible=%llu hidden=%llu completed=%llu udp_kib=%.1f partial_timeouts=%llu old_gen=%llu "
                     "retx_req_tx=%llu interarrival_avg_ms=%.2f jitter_avg_ms=%.2f max_gap_ms=%.2f remote_render_frames=%llu "
                     "tile_presented=%llu present_avg_ms=%.2f present_max_ms=%.2f input_present_avg_ms=%.2f",
                     (unsigned long long)s.client_stats_rx, (unsigned long long)s.client_render_visible_reports,
                     (unsigned long long)s.client_render_hidden_reports, (unsigned long long)s.client_tiles_completed,
                     (double)s.client_udp_bytes_rx / 1024.0, (unsigned long long)s.client_partial_tiles_timed_out,
                     (unsigned long long)s.client_old_generation_tiles, (unsigned long long)s.client_retx_requests_tx,
                     wd_avg_ms(s.client_udp_interarrival_sum_ns, s.client_udp_interarrival_samples),
                     wd_avg_ms(s.client_udp_interarrival_jitter_sum_ns, s.client_udp_interarrival_jitter_samples),
                     (double)s.client_udp_interarrival_max_ns / 1000000.0, (unsigned long long)s.client_render_frames,
                     (unsigned long long)s.client_tile_frames_presented, wd_avg_ms(s.client_present_sum_ns, s.client_present_samples),
                     (double)s.client_present_max_ns / 1000000.0, wd_avg_ms(s.client_input_present_sum_ns, s.client_input_present_samples));
    }

    if (s.client_video_messages_rx != 0 || s.client_video_data_frames_rx != 0 || s.client_video_frames_rx != 0 ||
        s.client_video_frames_decoded != 0 || s.client_video_frames_presented != 0 || s.client_video_decode_failed != 0 ||
        s.client_video_publish_failed != 0 || s.client_video_control_frames_rx != 0 || s.client_video_invalid_frames_rx != 0 ||
        s.client_video_stale_frames_dropped != 0 || s.client_video_need_keyframe_drops != 0 || s.client_video_decoder_resets != 0 ||
        s.client_video_queue_depth != 0 || s.client_video_queue_overflow_drops != 0 ||
        s.client_video_decode_queue_depth != 0 || s.client_video_decode_queue_drops != 0 ||
        s.client_audio_video_sync_hold_current_ms != 0)
    {
        WD_LOG_STATS("client-video/min: messages=%llu data=%llu legacy_rx=%llu decoded=%llu presented=%llu control=%llu invalid=%llu "
                     "stale_drop=%llu kib=%.1f decode_avg_ms=%.2f present_age_avg_ms=%.2f decode_failed=%llu publish_failed=%llu "
                     "need_keyframe_drops=%llu resets=%llu last_rx=%llu last_decoded=%llu last_presented=%llu "
                     "decode_q=%u/%u/%u phase=%u wait_keyframe=%u present_q=%u/%u present_q_overflow=%llu "
                     "oldest_pts_us=%llu av_delta_samples=%lld av_hold_ms=%u/%u",
                     (unsigned long long)s.client_video_messages_rx, (unsigned long long)s.client_video_data_frames_rx,
                     (unsigned long long)s.client_video_frames_rx, (unsigned long long)s.client_video_frames_decoded,
                     (unsigned long long)s.client_video_frames_presented, (unsigned long long)s.client_video_control_frames_rx,
                     (unsigned long long)s.client_video_invalid_frames_rx, (unsigned long long)s.client_video_stale_frames_dropped,
                     (double)s.client_video_bytes_rx / 1024.0, wd_avg_ms(s.client_video_decode_sum_ns, s.client_video_decode_samples),
                     wd_avg_ms(s.client_video_present_latency_sum_ns, s.client_video_present_latency_samples),
                     (unsigned long long)s.client_video_decode_failed, (unsigned long long)s.client_video_publish_failed,
                     (unsigned long long)s.client_video_need_keyframe_drops, (unsigned long long)s.client_video_decoder_resets,
                     (unsigned long long)s.client_video_last_frame_id_rx, (unsigned long long)s.client_video_last_frame_id_decoded,
                     (unsigned long long)s.client_video_last_frame_id_presented,
                     (unsigned)s.client_video_decode_queue_depth, (unsigned)s.client_video_decode_queue_depth_max,
                     (unsigned)s.client_video_decode_queue_capacity, (unsigned)s.client_video_decoder_phase,
                     (unsigned)s.client_video_waiting_keyframe, (unsigned)s.client_video_queue_depth,
                     (unsigned)s.client_video_queue_depth_max, (unsigned long long)s.client_video_queue_overflow_drops,
                     (unsigned long long)s.client_video_oldest_pts_usec, (long long)s.client_audio_video_delta_samples,
                     (unsigned)s.client_audio_video_sync_hold_current_ms, (unsigned)s.client_audio_video_sync_hold_max_ms);
    }

    const bool audio_stream_running = wd_audio_stream_running(net->audio_stream);
    if (audio_stream_running || s.audio_captured_frames != 0 || s.audio_capture_overruns != 0 || s.audio_encoded_packets != 0 ||
        s.audio_queue_drops != 0 || s.audio_discontinuities != 0 || s.audio_encode_failures != 0)
    {
        WD_LOG_STATS("audio-stream/min: running=%s captured_frames=%llu encoded_packets=%llu encoded_kib=%.1f capture_overruns=%llu "
                     "queue_drops=%llu discontinuities=%llu encode_failed=%llu",
                     audio_stream_running ? "yes" : "no", (unsigned long long)s.audio_captured_frames,
                     (unsigned long long)s.audio_encoded_packets, (double)s.audio_encoded_bytes / 1024.0,
                     (unsigned long long)s.audio_capture_overruns, (unsigned long long)s.audio_queue_drops,
                     (unsigned long long)s.audio_discontinuities, (unsigned long long)s.audio_encode_failures);
    }

    if (s.client_audio_messages_rx != 0 || s.client_audio_packets_rx != 0 || s.client_audio_decode_failed != 0 ||
        s.client_audio_discontinuities != 0 || s.client_audio_late_drops != 0 || s.client_audio_underflows != 0 ||
        s.client_audio_video_sync_holds != 0 || s.client_audio_video_sync_drops != 0)
    {
        WD_LOG_STATS("client-audio/min: messages=%llu packets=%llu kib=%.1f decode_failed=%llu discontinuities=%llu late_drops=%llu "
                     "underflows=%llu av_holds=%llu av_drops=%llu av_hold_ms=%u/%u startup_timeouts=%llu "
                     "startup_hold_ms=%u audio_state=%u",
                     (unsigned long long)s.client_audio_messages_rx, (unsigned long long)s.client_audio_packets_rx,
                     (double)s.client_audio_bytes_rx / 1024.0, (unsigned long long)s.client_audio_decode_failed,
                     (unsigned long long)s.client_audio_discontinuities, (unsigned long long)s.client_audio_late_drops,
                     (unsigned long long)s.client_audio_underflows, (unsigned long long)s.client_audio_video_sync_holds,
                     (unsigned long long)s.client_audio_video_sync_drops,
                     (unsigned)s.client_audio_video_sync_hold_current_ms, (unsigned)s.client_audio_video_sync_hold_max_ms,
                     (unsigned long long)s.client_audio_video_startup_timeouts,
                     (unsigned)s.client_audio_video_startup_hold_ms, (unsigned)s.client_audio_playback_state);
    }

    bool control_activity = s.tcp_hello_rx != 0 || s.tcp_config_tx != 0 || s.tcp_config_applied_ack_rx != 0 ||
                            s.tcp_input_channel_rx != 0 || s.tcp_input_channel_accepted != 0 || s.tcp_input_channel_closed != 0 ||
                            s.tcp_selection_channel_rx != 0 || s.tcp_selection_channel_accepted != 0 ||
                            s.tcp_selection_channel_closed != 0 || s.tcp_video_channel_rx != 0 || s.tcp_video_channel_accepted != 0 ||
                            s.tcp_video_channel_closed != 0 || s.tcp_async_send_failed != 0 || s.tcp_async_queued != 0 ||
                            s.tcp_async_completed != 0 || s.tcp_async_completion_failed != 0 || s.tcp_async_queue_overflow != 0 ||
                            s.tcp_async_partial_resubmits != 0 || s.tcp_control_bytes_sent != 0 || s.tcp_control_bytes_refunded != 0 ||
                            s.tcp_budget_blocked != 0;
    if (control_activity)
    {
        WD_LOG_STATS("control/min: hello=%llu config=%llu config_ack=%llu config_ack_avg_ms=%.2f config_ack_max_ms=%.2f input_rx=%llu "
                     "input_accepted=%llu input_closed=%llu selection_rx=%llu selection_accepted=%llu selection_closed=%llu video_rx=%llu "
                     "video_accepted=%llu video_closed=%llu async_queued=%llu async_completed=%llu async_send_failed=%llu "
                     "async_completion_failed=%llu async_overflow=%llu async_partial=%llu async_inflight_max=%llu tcp_kib=%.1f "
                     "tcp_refund_kib=%.1f tcp_budget_blocked=%llu",
                     (unsigned long long)s.tcp_hello_rx, (unsigned long long)s.tcp_config_tx,
                     (unsigned long long)s.tcp_config_applied_ack_rx,
                     wd_avg_ms(s.tcp_config_apply_ack_sum_ns, s.tcp_config_apply_ack_samples),
                     (double)s.tcp_config_apply_ack_max_ns / 1000000.0, (unsigned long long)s.tcp_input_channel_rx,
                     (unsigned long long)s.tcp_input_channel_accepted, (unsigned long long)s.tcp_input_channel_closed,
                     (unsigned long long)s.tcp_selection_channel_rx, (unsigned long long)s.tcp_selection_channel_accepted,
                     (unsigned long long)s.tcp_selection_channel_closed, (unsigned long long)s.tcp_video_channel_rx,
                     (unsigned long long)s.tcp_video_channel_accepted, (unsigned long long)s.tcp_video_channel_closed,
                     (unsigned long long)s.tcp_async_queued, (unsigned long long)s.tcp_async_completed,
                     (unsigned long long)s.tcp_async_send_failed, (unsigned long long)s.tcp_async_completion_failed,
                     (unsigned long long)s.tcp_async_queue_overflow, (unsigned long long)s.tcp_async_partial_resubmits,
                     (unsigned long long)s.tcp_async_inflight_max, (double)s.tcp_control_bytes_sent / 1024.0,
                     (double)s.tcp_control_bytes_refunded / 1024.0, (unsigned long long)s.tcp_budget_blocked);
    }

    bool input_activity = s.key_events_rx != 0 || s.key_events_injected != 0 || s.key_events_dropped != 0 ||
                          s.key_state_duplicate_presses != 0 || s.key_state_release_without_press != 0 || s.keyboard_enter_events != 0 ||
                          s.pointer_events_rx != 0 || s.pointer_events_injected != 0 || s.pointer_events_dropped != 0 ||
                          s.pointer_button_grab_started != 0 || s.pointer_button_grab_ended != 0 || s.pointer_button_grab_cleared != 0 ||
                          s.pointer_button_grab_surface_destroyed != 0 || s.input_queue_latency_samples != 0 ||
                          s.input_wakeup_signals != 0 || s.input_wakeup_callbacks != 0 || s.input_wakeup_failures != 0 ||
                          s.input_to_summary_samples != 0 || s.input_to_first_fresh_tile_samples != 0 ||
                          s.input_correlation_delivery_failed != 0;
    if (input_activity)
    {
        WD_LOG_STATS(
            "input/min: key_rx=%llu key_injected=%llu key_dropped=%llu dup_press=%llu release_without_press=%llu keyboard_enter=%llu "
            "pointer_rx=%llu pointer_injected=%llu pointer_dropped=%llu grabs_start=%llu grabs_end=%llu grabs_clear=%llu "
            "grab_surface_destroyed=%llu wake_signals=%llu wake_callbacks=%llu wake_events=%llu wake_coalesced=%llu wake_failures=%llu "
            "queue_avg_ms=%.2f input_to_summary_avg_ms=%.2f input_to_first_tile_avg_ms=%.2f input_delivery_failed=%llu",
            (unsigned long long)s.key_events_rx, (unsigned long long)s.key_events_injected, (unsigned long long)s.key_events_dropped,
            (unsigned long long)s.key_state_duplicate_presses, (unsigned long long)s.key_state_release_without_press,
            (unsigned long long)s.keyboard_enter_events, (unsigned long long)s.pointer_events_rx,
            (unsigned long long)s.pointer_events_injected, (unsigned long long)s.pointer_events_dropped,
            (unsigned long long)s.pointer_button_grab_started, (unsigned long long)s.pointer_button_grab_ended,
            (unsigned long long)s.pointer_button_grab_cleared, (unsigned long long)s.pointer_button_grab_surface_destroyed,
            (unsigned long long)s.input_wakeup_signals, (unsigned long long)s.input_wakeup_callbacks,
            (unsigned long long)s.input_wakeup_events, (unsigned long long)s.input_wakeup_coalesced,
            (unsigned long long)s.input_wakeup_failures, wd_avg_ms(s.input_queue_latency_sum_ns, s.input_queue_latency_samples),
            wd_avg_ms(s.input_to_summary_sum_ns, s.input_to_summary_samples),
            wd_avg_ms(s.input_to_first_fresh_tile_sum_ns, s.input_to_first_fresh_tile_samples),
            (unsigned long long)s.input_correlation_delivery_failed);
    }

    bool compositor_activity = s.xdg_move_invalid_serial != 0 || s.xdg_resize_invalid_serial != 0 || s.popup_explicit_scene_trees != 0 ||
                               s.popup_explicit_scene_tree_failures != 0 || s.cursor_shape_requests != 0 || s.cursor_shape_tx != 0 ||
                               s.cursor_shape_coalesced != 0 || s.cursor_set_cursor_requests != 0 || s.cursor_set_cursor_rejected != 0 ||
                               s.cursor_set_cursor_hidden != 0 || s.cursor_set_cursor_fallback != 0;
    if (compositor_activity)
    {
        WD_LOG_STATS(
            "compositor/min: xdg_move_bad_serial=%llu xdg_resize_bad_serial=%llu popup_scene=%llu popup_scene_fail=%llu cursor_shape=%llu "
            "cursor_shape_tx=%llu cursor_shape_coalesced=%llu cursor_set=%llu cursor_reject=%llu cursor_hidden=%llu cursor_fallback=%llu",
            (unsigned long long)s.xdg_move_invalid_serial, (unsigned long long)s.xdg_resize_invalid_serial,
            (unsigned long long)s.popup_explicit_scene_trees, (unsigned long long)s.popup_explicit_scene_tree_failures,
            (unsigned long long)s.cursor_shape_requests, (unsigned long long)s.cursor_shape_tx,
            (unsigned long long)s.cursor_shape_coalesced, (unsigned long long)s.cursor_set_cursor_requests,
            (unsigned long long)s.cursor_set_cursor_rejected, (unsigned long long)s.cursor_set_cursor_hidden,
            (unsigned long long)s.cursor_set_cursor_fallback);
    }
}
