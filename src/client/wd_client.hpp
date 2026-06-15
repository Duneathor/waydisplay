#pragma once

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"

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
    uint64_t posted        = 0;
    uint64_t completed     = 0;
    uint64_t failed        = 0;
    uint64_t submit_failed = 0;
    uint64_t cancels       = 0;
    uint64_t inflight_max  = 0;
};

struct ClientStreamConfig {
    uint16_t target_fps                 = WD_CLIENT_DEFAULT_TARGET_FPS;
    uint32_t limited_udp_kib_per_second = 0;
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
    std::atomic<uint64_t> udp_ignored_probe{0};
    std::atomic<uint64_t> udp_ignored_stale_session{0};
    std::atomic<uint64_t> udp_ignored_old_generation{0};
    std::atomic<uint64_t> udp_tiles_completed{0};
    std::atomic<uint64_t> udp_async_posted{0};
    std::atomic<uint64_t> udp_async_completed{0};
    std::atomic<uint64_t> udp_async_failed{0};
    std::atomic<uint64_t> udp_async_submit_failed{0};
    std::atomic<uint64_t> udp_async_cancels{0};
    std::atomic<uint64_t> udp_async_inflight_max{0};
    std::atomic<uint64_t> udp_completed_compressed_bytes{0};
    std::atomic<uint64_t> udp_completed_packets{0};
    std::atomic<uint64_t> partial_tiles_timed_out{0};
    std::atomic<uint64_t> partial_tile_missing_packets{0};
    std::atomic<uint64_t> partial_tile_retx_queued{0};
    std::atomic<uint64_t> retx_response_samples{0};
    std::atomic<uint64_t> retx_response_sum_ns{0};
    std::atomic<uint64_t> tile_reassembly_timeout_updates{0};
    std::atomic<uint64_t> tcp_summaries_rx{0};
    std::atomic<uint64_t> tcp_retx_requests_tx{0};
    std::atomic<uint64_t> summary_retx_tiles_queued{0};
    std::atomic<uint64_t> summary_retx_tiles_deferred{0};
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

    std::atomic<uint64_t> summary_latency_samples{0};
    std::atomic<uint64_t> summary_latency_sum_ns{0};
    std::atomic<uint64_t> tile_assembly_samples{0};
    std::atomic<uint64_t> tile_assembly_sum_ns{0};
    std::atomic<uint64_t> tile_present_latency_samples{0};
    std::atomic<uint64_t> tile_present_latency_sum_ns{0};
    std::atomic<uint64_t> input_to_present_latency_samples{0};
    std::atomic<uint64_t> input_to_present_latency_sum_ns{0};
    std::atomic<uint64_t> input_sequence_present_latency_samples{0};
    std::atomic<uint64_t> input_sequence_present_latency_sum_ns{0};
    std::atomic<uint64_t> latest_input_event_timestamp_ns{0};
};

struct ClientInputEventStamp {
    uint64_t sequence     = 0;
    uint64_t timestamp_ns = 0;
};

struct ClientState {
    std::atomic<bool> running{false};

    int tcp_fd           = -1;
    int input_tcp_fd     = -1;
    int selection_tcp_fd = -1;
    int udp_fd           = -1;

    ClientAsyncTcpSender* control_tcp_sender   = nullptr;
    ClientAsyncTcpSender* input_tcp_sender     = nullptr;
    ClientAsyncTcpSender* selection_tcp_sender = nullptr;
    ClientAsyncUdpReceiver* udp_receiver       = nullptr;
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

    wd_server_config_payload config{};

    std::vector<uint32_t> framebuffer;
    std::vector<uint64_t> displayed_generation;

    std::mutex udp_processing_mutex;
    std::mutex framebuffer_mutex;
    std::atomic<bool> frame_dirty{false};
    std::atomic<uint64_t> client_config_generation{1};

    std::mutex present_mutex;
    std::vector<uint64_t> pending_present_tile_timestamps;
    std::vector<uint64_t> pending_present_input_sequences;

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
    uint64_t                        retx_inflight_grace_ns = WD_LINK_RETRANSMIT_INFLIGHT_DEFAULT_NS;
    std::atomic<uint64_t>             summary_retransmit_grace_ns{WD_LINK_SUMMARY_GRACE_DEFAULT_NS};
    std::atomic<uint64_t>             retransmit_rerequest_interval_ns{WD_LINK_RETRANSMIT_REREQUEST_DEFAULT_NS};

    std::mutex            tile_reassembly_timeout_mutex;
    double                tile_reassembly_ewma_ns       = 0.0;
    double                tile_reassembly_deviation_ns  = 0.0;
    std::atomic<uint64_t> tile_reassembly_timeout_ns{WD_LINK_TILE_REASSEMBLY_DEFAULT_NS};
    std::atomic<uint64_t> tile_reassembly_floor_ns{WD_LINK_TILE_REASSEMBLY_MIN_NS};

    std::vector<uint8_t> udp_recv_buffer;

    std::mutex  selection_mutex;
    std::string pending_clipboard_text;
    bool        pending_clipboard_text_valid = false;
    std::string pending_primary_text;
    bool        pending_primary_text_valid = false;

    std::atomic<uint16_t> pending_cursor_shape{WD_CURSOR_SHAPE_DEFAULT};
    std::atomic<bool>     pending_cursor_shape_dirty{true};

    std::atomic<uint64_t>          next_input_sequence{1};
    std::mutex                    input_timestamp_mutex;
    std::deque<ClientInputEventStamp> recent_input_timestamps;

    ClientStats stats;
    std::thread tcp_thread;
};

} // namespace waydisplay
