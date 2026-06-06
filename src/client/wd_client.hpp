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

enum class ClientStreamMode : uint16_t {
    Full    = WD_STREAM_MODE_FULL,
    Partial = WD_STREAM_MODE_PARTIAL,
    Limited = WD_STREAM_MODE_LIMITED,
    Live    = WD_STREAM_MODE_LIVE,
};

struct ClientStreamConfig {
    ClientStreamMode mode       = ClientStreamMode::Partial;
    uint16_t         target_fps = WD_CLIENT_DEFAULT_TARGET_FPS;
};

struct ClientStats {
    std::atomic<uint64_t> udp_packets_rx{0};
    std::atomic<uint64_t> udp_bytes_rx{0};
    std::atomic<uint64_t> udp_ignored_invalid{0};
    std::atomic<uint64_t> udp_ignored_old_generation{0};
    std::atomic<uint64_t> udp_tiles_completed{0};
    std::atomic<uint64_t> tcp_summaries_rx{0};
    std::atomic<uint64_t> tcp_retx_requests_tx{0};
    std::atomic<uint64_t> tcp_keyboard_tx{0};
    std::atomic<uint64_t> tcp_pointer_tx{0};
    std::atomic<uint64_t> tcp_input_events_tx{0};

    std::atomic<uint64_t> summary_latency_samples{0};
    std::atomic<uint64_t> summary_latency_sum_ns{0};
    std::atomic<uint64_t> tile_assembly_samples{0};
    std::atomic<uint64_t> tile_assembly_sum_ns{0};
    std::atomic<uint64_t> tile_present_latency_samples{0};
    std::atomic<uint64_t> tile_present_latency_sum_ns{0};
    std::atomic<uint64_t> input_to_present_latency_samples{0};
    std::atomic<uint64_t> input_to_present_latency_sum_ns{0};
    std::atomic<uint64_t> latest_input_event_timestamp_ns{0};
};

struct ClientState {
    std::atomic<bool> running{false};

    int tcp_fd = -1;
    int udp_fd = -1;

    std::string server_host;
    uint16_t    tcp_port        = 0;
    uint16_t    client_udp_port = 0;
    uint16_t    desired_width   = 0;
    uint16_t    desired_height  = 0;

    ClientStreamConfig stream_config;

    wd_server_config_payload config{};

    std::vector<uint32_t> framebuffer;
    std::vector<uint64_t> displayed_generation;

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
    double                          retx_request_tokens = 0.0;
    uint64_t                        retx_request_last_refill_ns = 0;
    uint64_t                        retx_inflight_grace_ns = 250ull * 1000ull * 1000ull;

    std::vector<uint8_t> udp_recv_buffer;

    std::mutex  selection_mutex;
    std::string pending_clipboard_text;
    bool        pending_clipboard_text_valid = false;
    std::string pending_primary_text;
    bool        pending_primary_text_valid = false;

    std::atomic<uint16_t> pending_cursor_shape{WD_CURSOR_SHAPE_DEFAULT};
    std::atomic<bool>     pending_cursor_shape_dirty{true};

    ClientStats stats;
    std::thread tcp_thread;
};

} // namespace waydisplay
