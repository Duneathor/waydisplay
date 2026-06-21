#pragma once
#include "present_telemetry.hpp"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"
#include "render_planning.hpp"
#include "render_wakeup.hpp"
#include "stream_ownership.hpp"
#include "video_decoder.hpp"
#include "audio_playback.hpp"
#include "video_transition.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace waydisplay {

struct ClientAsyncTcpSender;
struct ClientAsyncUdpReceiver;

struct ClientAsyncTcpStatsSeen {
    uint64_t queued            = 0;
    uint64_t completed         = 0;
    uint64_t failed            = 0;
    uint64_t overflows         = 0;
    uint64_t partial_resubmits = 0;
    uint64_t coalesced         = 0;
    uint64_t inflight_max      = 0;
};

struct ClientAsyncUdpStatsSeen {
    uint64_t posted            = 0;
    uint64_t retired           = 0;
    uint64_t completed         = 0;
    uint64_t failed            = 0;
    uint64_t submit_failed     = 0;
    uint64_t cancels           = 0;
    uint64_t inflight_max      = 0;
    uint64_t accounting_errors = 0;
};

struct ClientStreamConfig {
    uint16_t target_fps                    = WD_CLIENT_DEFAULT_TARGET_FPS;
    uint32_t limited_udp_kib_per_second    = 0;
    uint8_t  video_mode                    = WD_VIDEO_MODE_AUTO;
    uint8_t  video_min_dirty_percent       = WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT;
    uint16_t video_enter_seconds           = WD_VIDEO_ENTER_SECONDS_DEFAULT;
    uint8_t  video_exit_dirty_percent      = WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT;
    uint16_t video_exit_seconds            = WD_VIDEO_EXIT_SECONDS_DEFAULT;
    uint32_t video_bitrate_kib_per_second  = 0;
    uint8_t  video_hwdecode_mode           = WD_CLIENT_VIDEO_HWDECODE_AUTO;
    uint32_t video_codec_mask              = WD_VIDEO_CODEC_H265;
    bool     disable_vsync                 = false;
    bool     disable_audio                 = false;
};

struct ClientStats {
    std::atomic<uint64_t> udp_packets_rx{0};
    std::atomic<uint64_t> udp_bytes_rx{0};
    std::atomic<uint64_t> udp_interarrival_samples{0};
    std::atomic<uint64_t> udp_interarrival_sum_ns{0};
    std::atomic<uint64_t> udp_interarrival_jitter_samples{0};
    std::atomic<uint64_t> udp_interarrival_jitter_sum_ns{0};
    std::atomic<uint64_t> udp_interarrival_max_ns{0};
    std::atomic<uint64_t> last_udp_packet_rx_ns{0};
    std::atomic<uint64_t> last_udp_interarrival_ns{0};
    std::atomic<uint64_t> udp_ignored_invalid{0};
    std::atomic<uint64_t> udp_invalid_short{0};
    std::atomic<uint64_t> udp_invalid_header{0};
    std::atomic<uint64_t> udp_invalid_geometry{0};
    std::atomic<uint64_t> udp_invalid_fragment{0};
    std::atomic<uint64_t> udp_invalid_blit{0};
    std::atomic<uint64_t> udp_invalid_dirty_grid{0};
    std::atomic<uint64_t> udp_ignored_probe{0};
    std::atomic<uint64_t> udp_ignored_stale_session{0};
    std::atomic<uint64_t> udp_ignored_stale_epoch{0};
    std::atomic<uint64_t> udp_ignored_old_generation{0};
    std::atomic<uint64_t> udp_tiles_completed{0};
    std::atomic<uint64_t> udp_async_posted{0};
    std::atomic<uint64_t> udp_async_retired{0};
    std::atomic<uint64_t> udp_async_completed{0};
    std::atomic<uint64_t> udp_async_failed{0};
    std::atomic<uint64_t> udp_async_submit_failed{0};
    std::atomic<uint64_t> udp_async_cancels{0};
    std::atomic<uint64_t> udp_async_inflight_current{0};
    std::atomic<uint64_t> udp_async_prepared_current{0};
    std::atomic<uint64_t> udp_async_inflight_max{0};
    std::atomic<uint64_t> udp_async_drained_on_reconfigure{0};
    std::atomic<uint64_t> udp_async_cancelled_on_reconfigure{0};
    std::atomic<uint64_t> udp_async_receiver_generations{0};
    std::atomic<uint64_t> udp_async_accounting_errors{0};
    std::atomic<uint64_t> udp_completed_compressed_bytes{0};
    std::atomic<uint64_t> udp_completed_packets{0};
    std::atomic<uint64_t> partial_tiles_timed_out{0};
    std::atomic<uint64_t> reassembly_budget_evictions{0};
    std::atomic<uint64_t> reassembly_budget_drops{0};
    std::atomic<uint64_t> tile_decompress_failures{0};
    std::atomic<uint64_t> tile_fragment_conflicts{0};
    std::atomic<uint64_t> partial_tile_missing_packets{0};
    std::atomic<uint64_t> partial_tile_retx_queued{0};
    std::atomic<uint64_t> retx_response_samples{0};
    std::atomic<uint64_t> retx_response_sum_ns{0};
    std::atomic<uint64_t> tile_reassembly_timeout_updates{0};
    std::atomic<uint64_t> tcp_summaries_rx{0};
    std::atomic<uint64_t> tcp_retx_requests_tx{0};
    std::atomic<uint64_t> summary_retx_tiles_queued{0};
    std::atomic<uint64_t> summary_retx_tiles_deferred{0};
    std::atomic<uint64_t> summary_retx_tiles_throttled{0};
    std::atomic<uint64_t> summary_retx_tiles_stale_dropped{0};
    std::atomic<uint64_t> summary_retx_pressure_dropped{0};
    std::atomic<uint64_t> summary_promote_passes{0};
    std::atomic<uint64_t> summary_promote_scanned{0};
    std::atomic<uint64_t> summary_promote_candidates{0};
    std::atomic<uint64_t> summary_to_retx_samples{0};
    std::atomic<uint64_t> summary_to_retx_sum_ns{0};
    std::atomic<uint64_t> tcp_keyboard_tx{0};
    std::atomic<uint64_t> tcp_pointer_tx{0};
    std::atomic<uint64_t> tcp_input_events_tx{0};
    std::atomic<uint64_t> tcp_input_channel_tx{0};
    std::atomic<uint64_t> tcp_input_channel_fallback_tx{0};
    std::atomic<uint64_t> tcp_selection_channel_tx{0};
    std::atomic<uint64_t> tcp_selection_channel_fallback_tx{0};
    std::atomic<uint64_t> tcp_async_queued{0};
    std::atomic<uint64_t> tcp_async_completed{0};
    std::atomic<uint64_t> tcp_async_failed{0};
    std::atomic<uint64_t> tcp_async_overflow{0};
    std::atomic<uint64_t> tcp_async_partial{0};
    std::atomic<uint64_t> tcp_async_coalesced{0};
    std::atomic<uint64_t> tcp_async_inflight_max{0};
    std::atomic<uint64_t> video_frames_rx{0};
    std::atomic<uint64_t> video_bytes_rx{0};
    std::atomic<uint64_t> video_frames_decoded{0};
    std::atomic<uint64_t> video_frames_presented{0};
    std::atomic<uint64_t> video_decode_failed{0};
    std::atomic<uint64_t> video_publish_failed{0};
    std::atomic<uint64_t> video_control_frames_rx{0};
    std::atomic<uint64_t> video_need_keyframe_drops{0};
    std::atomic<uint64_t> video_decoder_resets{0};
    std::atomic<uint64_t> video_decode_samples{0};
    std::atomic<uint64_t> video_decode_sum_ns{0};
    std::atomic<uint64_t> video_messages_rx{0};
    std::atomic<uint64_t> video_data_frames_rx{0};
    std::atomic<uint64_t> video_invalid_frames_rx{0};
    std::atomic<uint64_t> video_stale_frames_dropped{0};
    std::atomic<uint64_t> video_last_frame_id_rx{0};
    std::atomic<uint64_t> video_last_frame_id_presented{0};
    std::atomic<uint64_t> video_present_latency_samples{0};
    std::atomic<uint64_t> video_present_latency_sum_ns{0};
    std::atomic<uint64_t> video_audio_sync_holds{0};
    std::atomic<uint64_t> video_audio_sync_drops{0};
    std::atomic<uint64_t> audio_messages_rx{0};
    std::atomic<uint64_t> audio_packets_rx{0};
    std::atomic<uint64_t> audio_bytes_rx{0};
    std::atomic<uint64_t> audio_decode_failed{0};
    std::atomic<uint64_t> audio_discontinuities{0};
    std::atomic<uint64_t> audio_late_drops{0};
    std::atomic<uint64_t> audio_underflows{0};

    std::atomic<uint64_t> tile_assembly_samples{0};
    std::atomic<uint64_t> tile_assembly_sum_ns{0};
    std::atomic<uint64_t> tile_present_latency_samples{0};
    std::atomic<uint64_t> tile_present_latency_sum_ns{0};
    std::atomic<uint64_t> input_to_present_latency_samples{0};
    std::atomic<uint64_t> input_to_present_latency_sum_ns{0};
    std::atomic<uint64_t> input_sequence_present_latency_samples{0};
    std::atomic<uint64_t> input_sequence_present_latency_sum_ns{0};
    std::atomic<uint64_t> latest_input_event_timestamp_ns{0};

    std::atomic<uint64_t> sdl_render_frames{0};
    std::atomic<uint64_t> sdl_remote_frames{0};
    std::atomic<uint64_t> sdl_empty_remote_wakeups{0};
    std::atomic<uint64_t> sdl_texture_full_uploads{0};
    std::atomic<uint64_t> sdl_texture_partial_uploads{0};
    std::atomic<uint64_t> sdl_texture_dirty_rects{0};
    std::atomic<uint64_t> sdl_texture_source_dirty_rects{0};
    std::atomic<uint64_t> sdl_texture_coalesced_dirty_rects{0};
    std::atomic<uint64_t> sdl_texture_bounds_uploads{0};
    std::atomic<uint64_t> sdl_texture_cost_full_uploads{0};
    std::atomic<uint64_t> sdl_texture_lock_calls{0};
    std::atomic<uint64_t> sdl_texture_update_calls{0};
    std::atomic<uint64_t> sdl_texture_model_update_call_ns{0};
    std::atomic<uint64_t> sdl_texture_model_lock_call_ns{0};
    std::atomic<uint64_t> sdl_texture_model_pixel_cost_q16{0};
    std::atomic<uint64_t> sdl_texture_source_pixels{0};
    std::atomic<uint64_t> sdl_texture_upload_pixels{0};
    std::atomic<uint64_t> sdl_texture_upload_samples{0};
    std::atomic<uint64_t> sdl_texture_upload_sum_ns{0};
    std::atomic<uint64_t> sdl_texture_upload_max_ns{0};
    std::atomic<uint64_t> framebuffer_snapshot_pixels{0};
    std::atomic<uint64_t> framebuffer_snapshot_samples{0};
    std::atomic<uint64_t> framebuffer_snapshot_sum_ns{0};
    std::atomic<uint64_t> framebuffer_snapshot_max_ns{0};
    std::atomic<uint64_t> framebuffer_direct_uploads{0};
    std::atomic<uint64_t> framebuffer_staged_uploads{0};
    std::atomic<uint64_t> framebuffer_lock_wait_samples{0};
    std::atomic<uint64_t> framebuffer_lock_wait_sum_ns{0};
    std::atomic<uint64_t> framebuffer_lock_wait_max_ns{0};
    std::atomic<uint64_t> framebuffer_lock_hold_samples{0};
    std::atomic<uint64_t> framebuffer_lock_hold_sum_ns{0};
    std::atomic<uint64_t> framebuffer_lock_hold_max_ns{0};
    std::atomic<uint64_t> sdl_video_texture_uploads{0};
    std::atomic<uint64_t> sdl_video_texture_upload_pixels{0};
    std::atomic<uint64_t> sdl_present_samples{0};
    std::atomic<uint64_t> sdl_present_sum_ns{0};
    std::atomic<uint64_t> sdl_present_max_ns{0};
};

struct ClientStatsSnapshot {
    uint64_t udp_packets = 0;
    uint64_t udp_bytes = 0;
    uint64_t udp_interarrival_samples = 0;
    uint64_t udp_interarrival_sum_ns = 0;
    uint64_t udp_jitter_samples = 0;
    uint64_t udp_jitter_sum_ns = 0;
    uint64_t udp_interarrival_max_ns = 0;
    uint64_t invalid = 0;
    uint64_t invalid_short = 0;
    uint64_t invalid_header = 0;
    uint64_t invalid_geometry = 0;
    uint64_t invalid_fragment = 0;
    uint64_t invalid_blit = 0;
    uint64_t invalid_dirty_grid = 0;
    uint64_t ignored_probe = 0;
    uint64_t stale_session = 0;
    uint64_t stale_epoch = 0;
    uint64_t old_gen = 0;
    uint64_t completed = 0;
    uint64_t completed_compressed = 0;
    uint64_t completed_packets = 0;
    uint64_t partial_timeouts = 0;
    uint64_t partial_missing_packets = 0;
    uint64_t partial_retx_queued = 0;
    uint64_t retx_response_samples = 0;
    uint64_t retx_response_sum_ns = 0;
    uint64_t timeout_updates = 0;
    uint64_t summaries = 0;
    uint64_t retx = 0;
    uint64_t summary_retx_queued = 0;
    uint64_t summary_retx_deferred = 0;
    uint64_t summary_retx_throttled = 0;
    uint64_t summary_retx_stale_dropped = 0;
    uint64_t summary_retx_pressure_dropped = 0;
    uint64_t summary_promote_passes = 0;
    uint64_t summary_promote_scanned = 0;
    uint64_t summary_promote_candidates = 0;
    uint64_t summary_to_retx_samples = 0;
    uint64_t summary_to_retx_sum_ns = 0;
    uint64_t keys = 0;
    uint64_t pointer = 0;
    uint64_t input_events = 0;
    uint64_t input_channel_events = 0;
    uint64_t input_fallback_events = 0;
    uint64_t selection_channel_events = 0;
    uint64_t selection_fallback_events = 0;
    uint64_t tcp_async_queued = 0;
    uint64_t tcp_async_completed = 0;
    uint64_t tcp_async_failed = 0;
    uint64_t tcp_async_overflow = 0;
    uint64_t tcp_async_partial = 0;
    uint64_t tcp_async_coalesced = 0;
    uint64_t tcp_async_inflight_max = 0;
    uint64_t video_frames_rx = 0;
    uint64_t video_bytes_rx = 0;
    uint64_t video_frames_decoded = 0;
    uint64_t video_frames_presented = 0;
    uint64_t video_decode_failed = 0;
    uint64_t video_publish_failed = 0;
    uint64_t video_control_frames_rx = 0;
    uint64_t video_need_keyframe_drops = 0;
    uint64_t video_decoder_resets = 0;
    uint64_t video_decode_samples = 0;
    uint64_t video_decode_sum_ns = 0;
    uint64_t video_messages_rx = 0;
    uint64_t video_data_frames_rx = 0;
    uint64_t video_invalid_frames_rx = 0;
    uint64_t video_stale_frames_dropped = 0;
    uint64_t video_last_frame_id_rx = 0;
    uint64_t video_last_frame_id_presented = 0;
    uint64_t video_present_latency_samples = 0;
    uint64_t video_present_latency_sum_ns = 0;
    uint64_t video_audio_sync_holds = 0;
    uint64_t video_audio_sync_drops = 0;
    uint64_t audio_messages_rx = 0;
    uint64_t audio_packets_rx = 0;
    uint64_t audio_bytes_rx = 0;
    uint64_t audio_decode_failed = 0;
    uint64_t audio_discontinuities = 0;
    uint64_t audio_late_drops = 0;
    uint64_t audio_underflows = 0;
    uint64_t udp_async_posted = 0;
    uint64_t udp_async_retired = 0;
    uint64_t udp_async_completed = 0;
    uint64_t udp_async_failed = 0;
    uint64_t udp_async_submit_failed = 0;
    uint64_t udp_async_cancels = 0;
    uint64_t udp_async_inflight_current = 0;
    uint64_t udp_async_prepared_current = 0;
    uint64_t udp_async_inflight_max = 0;
    uint64_t udp_async_drained_on_reconfigure = 0;
    uint64_t udp_async_cancelled_on_reconfigure = 0;
    uint64_t udp_async_receiver_generations = 0;
    uint64_t udp_async_accounting_errors = 0;
    uint64_t tile_assembly_samples = 0;
    uint64_t tile_assembly_sum_ns = 0;
    uint64_t tile_present_samples = 0;
    uint64_t tile_present_sum_ns = 0;
    uint64_t input_to_present_samples = 0;
    uint64_t input_to_present_sum_ns = 0;
    uint64_t input_seq_present_samples = 0;
    uint64_t input_seq_present_sum_ns = 0;
    uint64_t sdl_render_frames = 0;
    uint64_t sdl_remote_frames = 0;
    uint64_t sdl_empty_remote_wakeups = 0;
    uint64_t sdl_texture_full_uploads = 0;
    uint64_t sdl_texture_partial_uploads = 0;
    uint64_t sdl_texture_dirty_rects = 0;
    uint64_t sdl_texture_source_dirty_rects = 0;
    uint64_t sdl_texture_coalesced_dirty_rects = 0;
    uint64_t sdl_texture_bounds_uploads = 0;
    uint64_t sdl_texture_cost_full_uploads = 0;
    uint64_t sdl_texture_lock_calls = 0;
    uint64_t sdl_texture_update_calls = 0;
    uint64_t sdl_texture_model_update_call_ns = 0;
    uint64_t sdl_texture_model_lock_call_ns = 0;
    uint64_t sdl_texture_model_pixel_cost_q16 = 0;
    uint64_t sdl_texture_source_pixels = 0;
    uint64_t sdl_texture_upload_pixels = 0;
    uint64_t sdl_texture_upload_samples = 0;
    uint64_t sdl_texture_upload_sum_ns = 0;
    uint64_t sdl_texture_upload_max_ns = 0;
    uint64_t framebuffer_snapshot_pixels = 0;
    uint64_t framebuffer_snapshot_samples = 0;
    uint64_t framebuffer_snapshot_sum_ns = 0;
    uint64_t framebuffer_snapshot_max_ns = 0;
    uint64_t framebuffer_direct_uploads = 0;
    uint64_t framebuffer_staged_uploads = 0;
    uint64_t framebuffer_lock_wait_samples = 0;
    uint64_t framebuffer_lock_wait_sum_ns = 0;
    uint64_t framebuffer_lock_wait_max_ns = 0;
    uint64_t framebuffer_lock_hold_samples = 0;
    uint64_t framebuffer_lock_hold_sum_ns = 0;
    uint64_t framebuffer_lock_hold_max_ns = 0;
    uint64_t sdl_video_texture_uploads = 0;
    uint64_t sdl_video_texture_upload_pixels = 0;
    uint64_t sdl_present_samples = 0;
    uint64_t sdl_present_sum_ns = 0;
    uint64_t sdl_present_max_ns = 0;
};

struct ClientStatsLogState {
    ClientStatsSnapshot totals{};
    uint64_t            prev_timeout_ms          = 0;
    uint64_t            prev_udp_gap_pressure_ms = 0;
};

struct ClientInputEventStamp {
    uint64_t sequence     = 0;
    uint64_t timestamp_ns = 0;
};

struct ClientSummaryRepairDue {
    uint16_t tile_id    = 0;
    uint64_t generation = 0;
    uint64_t due_ns     = 0;
};

struct ClientState {
    std::atomic<bool> running{false};

    int tcp_fd           = -1;
    int input_tcp_fd     = -1;
    int selection_tcp_fd = -1;
    int video_tcp_fd     = -1;
    int audio_tcp_fd     = -1;
    int udp_fd           = -1;

    ClientAsyncTcpSender* control_tcp_sender   = nullptr;
    ClientAsyncTcpSender* input_tcp_sender     = nullptr;
    ClientAsyncTcpSender* selection_tcp_sender = nullptr;
    ClientAsyncUdpReceiver* udp_receiver       = nullptr;
    ClientVideoDecoder*     video_decoder      = nullptr;
    ClientAudioPlayback*    audio_playback     = nullptr;
    std::mutex              video_decoder_mutex;
    bool                    video_decoder_needs_keyframe = true;
    ClientVideoPhase        video_phase = ClientVideoPhase::Tiles;
    std::atomic<bool>       video_tcp_connected{false};
    std::atomic<bool>       video_unavailable{false};
    std::mutex              video_tcp_mutex;
    std::atomic<bool>       audio_tcp_connected{false};
    std::mutex              audio_tcp_mutex;
    std::mutex            async_tcp_stats_mutex;
    ClientAsyncTcpStatsSeen control_tcp_seen{};
    ClientAsyncTcpStatsSeen input_tcp_seen{};
    ClientAsyncTcpStatsSeen selection_tcp_seen{};
    ClientAsyncUdpStatsSeen udp_seen{};

    std::string server_host;
    uint16_t    tcp_port        = 0;
    uint16_t    client_udp_port = 0;
    uint16_t    desired_width   = 0;
    uint16_t    desired_height  = 0;

    ClientStreamConfig stream_config;

    bool     video_stream_negotiated = false;
    uint32_t video_codecs            = 0;
    uint16_t video_transport         = 0;

    bool     audio_stream_negotiated = false;
    uint32_t audio_codec             = 0;
    uint16_t audio_transport         = 0;
    uint8_t  audio_channels          = 0;
    uint16_t audio_target_latency_ms = 0;

    wd_server_config_payload config{};
    uint64_t media_clock_id = 0;
    uint64_t media_clock_local_origin_ns = 0;

    std::vector<uint32_t> framebuffer;
    std::vector<uint64_t> received_generation;
    std::vector<uint64_t> presented_generation;
    std::vector<uint64_t> pending_present_generation;

    std::mutex              video_frame_mutex;
    ClientVideoFrameBuffer  video_framebuffer;
    uint32_t                video_frame_width  = 0;
    uint32_t                video_frame_height = 0;
    uint64_t                video_frame_id     = 0;
    uint64_t                video_frame_pts_usec = 0;
    uint64_t                video_frame_epoch = 0;
    std::atomic<bool>       pending_video_frame_dirty{false};

    std::mutex udp_processing_mutex;
    std::mutex framebuffer_mutex;
    ClientRenderWake render_wake;
    std::atomic<uint64_t> pending_dirty_rect_count{0};
    std::atomic<uint64_t> framebuffer_lock_wait_ewma_ns{0};
    std::mutex          dirty_rect_mutex;
    ClientDirtyTileGrid pending_dirty_tiles;
    uint64_t            pending_dirty_epoch = 1;
    ClientStreamOwnership stream_ownership;
    std::mutex remote_content_mutex;
    uint64_t remote_content_epoch = 0;
    ClientContentOwner remote_content_owner = ClientContentOwner::Tiles;
    std::atomic<uint64_t> client_config_generation{1};

    std::mutex present_mutex;
    std::vector<ClientPendingTileTelemetry> pending_tile_telemetry;
    uint64_t next_tile_completion_id = 1;

    std::mutex               config_mutex;
    wd_server_config_payload pending_config{};
    bool                     pending_config_valid = false;

    uint32_t framebuffer_pixels() const {
        return static_cast<uint32_t>(config.width) * static_cast<uint32_t>(config.height);
    }

    uint32_t framebuffer_bytes() const {
        return framebuffer_pixels() * WD_BYTES_PER_PIXEL;
    }

    uint32_t tile_count() const {
        return static_cast<uint32_t>(config.total_tiles);
    }

    uint32_t tile_uncompressed_bytes() const {
        return static_cast<uint32_t>(config.tile_width) * static_cast<uint32_t>(config.tile_height) * WD_BYTES_PER_PIXEL;
    }

    std::mutex generation_mutex;

    std::mutex                      retx_mutex;
    std::deque<uint16_t>             retx_queue;
    std::vector<uint64_t>           retx_queued_generation;
    std::vector<uint64_t>           retx_last_requested_generation;
    std::vector<uint64_t>           retx_last_request_ns;
    std::vector<uint64_t>           retx_inflight_generation;
    std::vector<uint64_t>           retx_inflight_since_ns;
    std::vector<uint64_t>           retx_summary_pending_generation;
    std::vector<uint64_t>           retx_summary_pending_since_ns;
    std::vector<uint16_t>           retx_summary_pending_tiles;
    std::vector<uint32_t>           retx_summary_pending_position;
    std::deque<ClientSummaryRepairDue> retx_summary_due_queue;
    uint64_t                        retx_inflight_grace_ns = WD_LINK_RETRANSMIT_INFLIGHT_DEFAULT_NS;
    std::atomic<uint64_t>             summary_retransmit_grace_ns{WD_LINK_SUMMARY_GRACE_DEFAULT_NS};
    std::atomic<uint64_t>             retransmit_rerequest_interval_ns{WD_LINK_RETRANSMIT_REREQUEST_DEFAULT_NS};
    uint32_t                        retx_summary_pending_count = 0;
    uint64_t                        next_summary_promote_ns = 0;
    uint64_t                        summary_large_repair_not_before_ns = 0;
    std::atomic<uint64_t>             summary_repair_loss_signal_until_ns{0};

    std::mutex            tile_reassembly_timeout_mutex;
    double                tile_reassembly_ewma_ns       = 0.0;
    double                tile_reassembly_deviation_ns  = 0.0;
    std::atomic<uint64_t> tile_reassembly_timeout_ns{WD_LINK_TILE_REASSEMBLY_DEFAULT_NS};
    std::atomic<uint64_t> tile_reassembly_floor_ns{WD_LINK_TILE_REASSEMBLY_MIN_NS};
    std::atomic<uint64_t> udp_gap_pressure_ns{0};

    std::vector<uint8_t> udp_recv_buffer;

    std::mutex  selection_mutex;
    std::string pending_clipboard_text;
    bool        pending_clipboard_text_valid = false;
    std::string pending_primary_text;
    bool        pending_primary_text_valid = false;

    std::atomic<uint16_t> pending_cursor_shape{WD_CURSOR_SHAPE_DEFAULT};
    std::atomic<bool>     pending_cursor_shape_dirty{true};

    std::atomic<uint64_t>          next_input_sequence{1};
    std::atomic<bool>              render_feedback_visible{true};
    std::mutex                    input_timestamp_mutex;
    std::deque<ClientInputEventStamp> recent_input_timestamps;

    ClientStats         stats;
    ClientStatsLogState stats_log;
    std::thread         tcp_thread;
    std::thread         video_thread;
    std::thread         audio_thread;
};

} // namespace waydisplay
