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

        ClientStreamConfig stream_config;

        wd_server_config_payload config{};

        std::vector<uint32_t> framebuffer;
        std::vector<uint64_t> displayed_generation;

        std::mutex generation_mutex;

        std::mutex retx_mutex;
        std::deque<wd_retransmit_entry> retx_queue;

        ClientStats stats;
        std::thread tcp_thread;
    };

} // namespace waydisplay
