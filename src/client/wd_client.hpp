#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"

namespace waydisplay {

    enum class ClientStreamMode : uint16_t {
        Full = WD_STREAM_MODE_FULL,
        Partial = WD_STREAM_MODE_PARTIAL,
        Limited = WD_STREAM_MODE_LIMITED,
        Live = WD_STREAM_MODE_LIVE,
    };

    struct ClientStreamConfig {
        ClientStreamMode mode = ClientStreamMode::Partial;
        uint16_t target_fps = 30;
        uint32_t max_tiles_per_second = 120;
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
    };

    struct ClientState {
        std::atomic<bool> running{false};

        int tcp_fd = -1;
        int udp_fd = -1;

        std::string server_host;
        uint16_t tcp_port = 0;
        uint16_t client_udp_port = 0;
        uint16_t desired_width = 0;
        uint16_t desired_height = 0;

        ClientStreamConfig stream_config;

        wd_server_config_payload config{};

        std::vector<uint32_t> framebuffer;
        std::vector<uint64_t> displayed_generation;

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
            return static_cast<uint32_t>(config.tile_width) *
                   static_cast<uint32_t>(config.tile_height) *
                   WD_BYTES_PER_PIXEL;
        }

        std::mutex generation_mutex;

        std::mutex retx_mutex;
        std::deque<wd_retransmit_entry> retx_queue;

        std::mutex selection_mutex;
        std::string pending_clipboard_text;
        bool pending_clipboard_text_valid = false;
        std::string pending_primary_text;
        bool pending_primary_text_valid = false;

        std::atomic<uint16_t> pending_cursor_shape{WD_CURSOR_SHAPE_DEFAULT};
        std::atomic<bool> pending_cursor_shape_dirty{true};

        ClientStats stats;
        std::thread tcp_thread;
    };

} // namespace waydisplay
