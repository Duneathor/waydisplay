#include "client_net.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_net.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_time.h"

namespace waydisplay {
namespace {

constexpr size_t MAX_RETRANSMIT_ENTRIES_PER_MESSAGE = 32;
constexpr size_t MAX_RETRANSMIT_QUEUE_DEPTH = 256;

bool set_socket_rcvbuf(int fd, int requested_bytes) {
    if (fd < 0) {
        return false;
    }

    if (::setsockopt(fd,
        SOL_SOCKET,
        SO_RCVBUF,
        &requested_bytes,
        sizeof(requested_bytes)) != 0) {
        std::perror("setsockopt SO_RCVBUF");
    return false;
        }

        int actual_bytes = 0;
        socklen_t actual_len = sizeof(actual_bytes);

        if (::getsockopt(fd,
            SOL_SOCKET,
            SO_RCVBUF,
            &actual_bytes,
            &actual_len) == 0) {
            std::printf("UDP receive buffer: requested=%d actual=%d\n",
                        requested_bytes,
                        actual_bytes);
            }

            return true;
}

bool open_udp_socket(ClientState& state) {
    state.udp_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (state.udp_fd < 0) {
        std::perror("socket UDP");
        return false;
    }

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(state.client_udp_port);

    if (::bind(state.udp_fd,
               reinterpret_cast<sockaddr*>(&bind_addr),
               sizeof(bind_addr)) < 0) {
        std::perror("bind UDP");
        return false;
    }

    if (wd_set_nonblocking(state.udp_fd) < 0) {
        std::perror("set UDP nonblocking");
        return false;
    }

    set_socket_rcvbuf(state.udp_fd, 16 * 1024 * 1024);

    return true;
}

bool open_tcp_socket(ClientState& state) {
    state.tcp_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (state.tcp_fd < 0) {
        std::perror("socket TCP");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(state.tcp_port);

    if (::inet_pton(AF_INET, state.server_host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "invalid IPv4 address: %s\n", state.server_host.c_str());
        return false;
    }

    if (::connect(state.tcp_fd,
                  reinterpret_cast<sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        std::perror("connect TCP");
        return false;
    }

    return true;
}

bool handle_mtu_probe_start(ClientState& state,
                            const uint8_t* payload,
                            uint32_t payload_size) {
    if (payload_size < sizeof(wd_mtu_probe_start_payload)) {
        return false;
    }

    wd_mtu_probe_start_payload start{};
    std::memcpy(&start, payload, sizeof(start));

    uint16_t max_received = 0;
    const uint64_t deadline_ns = wd_now_ns() + 500000000ull;

    std::vector<uint8_t> recvbuf(sizeof(wd_udp_tile_packet_header) + 65535);

    while (wd_now_ns() < deadline_ns) {
        ssize_t n = ::recv(state.udp_fd, recvbuf.data(), recvbuf.size(), 0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }

            if (errno == EINTR) {
                continue;
            }

            break;
        }

        if (static_cast<size_t>(n) < sizeof(wd_udp_tile_packet_header)) {
            continue;
        }

        wd_udp_tile_packet_header h{};
        std::memcpy(&h, recvbuf.data(), sizeof(h));

        if (h.tile_id != WD_UDP_TILE_ID_MTU_PROBE) {
            continue;
        }

        if (static_cast<size_t>(n) < sizeof(wd_udp_tile_packet_header) + h.payload_size) {
            continue;
        }

        if (h.payload_size > max_received) {
            max_received = h.payload_size;
        }
    }

    if (max_received == 0) {
        max_received = WD_UDP_PAYLOAD_TARGET;
    }

    wd_mtu_probe_result_payload result{};
    result.session_id = start.session_id;
    result.max_udp_payload_received = max_received;

    return wd_send_tcp_message(state.tcp_fd,
                               WD_MSG_MTU_PROBE_RESULT,
                               &result,
                               sizeof(result));
}

bool receive_server_config(ClientState& state) {
    wd_client_hello_payload hello{};
    hello.client_udp_port = state.client_udp_port;
    hello.stream_mode = static_cast<uint16_t>(state.stream_config.mode);
    hello.target_fps = state.stream_config.target_fps;
    hello.max_tiles_per_second = state.stream_config.max_tiles_per_second;

    if (!wd_send_tcp_message(state.tcp_fd,
                             WD_MSG_CLIENT_HELLO,
                             &hello,
                             sizeof(hello))) {
        std::fprintf(stderr, "failed to send CLIENT_HELLO\n");
        return false;
    }

    for (;;) {
        uint16_t message_type = 0;
        uint8_t* payload = nullptr;
        uint32_t payload_size = 0;

        if (!wd_recv_tcp_message(state.tcp_fd,
                                 &message_type,
                                 &payload,
                                 &payload_size)) {
            std::fprintf(stderr, "failed to receive SERVER_CONFIG\n");
            return false;
        }

        if (message_type == WD_MSG_MTU_PROBE_START) {
            const bool ok = handle_mtu_probe_start(state, payload, payload_size);
            std::free(payload);

            if (!ok) {
                std::fprintf(stderr, "failed UDP MTU probe\n");
                return false;
            }

            continue;
        }

        if (message_type == WD_MSG_SERVER_CONFIG &&
            payload_size >= sizeof(wd_server_config_payload)) {
            std::memcpy(&state.config, payload, sizeof(state.config));
            std::free(payload);
            break;
        }

        std::fprintf(stderr,
                     "unexpected TCP message while waiting for SERVER_CONFIG: %u\n",
                     message_type);
        std::free(payload);
        return false;
    }

    if (state.config.width != WD_DISPLAY_WIDTH ||
        state.config.height != WD_DISPLAY_HEIGHT ||
        state.config.tile_width != WD_TILE_WIDTH ||
        state.config.tile_height != WD_TILE_HEIGHT ||
        state.config.tiles_x != WD_TILES_X ||
        state.config.tiles_y != WD_TILES_Y ||
        state.config.total_tiles != WD_TOTAL_TILES ||
        state.config.pixel_format != WD_PIXEL_FORMAT_XRGB8888 ||
        state.config.compression_mode != WD_COMPRESSION_ZSTD) {
        std::fprintf(stderr,
                     "server config does not match this client build\n"
                     "server: %ux%u tiles=%ux%u total=%u pixel=%u compression=%u\n",
                     state.config.width,
                     state.config.height,
                     state.config.tile_width,
                     state.config.tile_height,
                     state.config.total_tiles,
                     state.config.pixel_format,
                     state.config.compression_mode);
        return false;
    }

    if (state.config.udp_payload_target == 0) {
        state.config.udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    std::printf("UDP payload target: %u\n", state.config.udp_payload_target);

    return true;
}

void queue_retransmits_from_summary(ClientState& state,
                                    const uint8_t* payload,
                                    uint32_t payload_size) {
    if (payload_size < sizeof(wd_tile_summary_payload_header)) {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (state.stream_config.mode == ClientStreamMode::Live) {
        return;
    }

    wd_tile_summary_payload_header summary{};
    std::memcpy(&summary, payload, sizeof(summary));

    if (summary.session_id != state.config.session_id) {
        return;
    }

    const size_t needed =
    sizeof(wd_tile_summary_payload_header) +
    static_cast<size_t>(summary.tile_count) * sizeof(wd_tile_generation_entry);

    if (payload_size < needed) {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto* entries =
    reinterpret_cast<const wd_tile_generation_entry*>(
        payload + sizeof(wd_tile_summary_payload_header));

    std::vector<wd_retransmit_entry> missing;
    missing.reserve(32);

    {
        std::lock_guard<std::mutex> gen_lock(state.generation_mutex);
        std::lock_guard<std::mutex> retx_lock(state.retx_mutex);

        for (uint16_t i = 0; i < summary.tile_count; ++i) {
            const wd_tile_generation_entry& entry = entries[i];

            if (entry.tile_id >= WD_TOTAL_TILES) {
                continue;
            }

            if (entry.tile_generation <= state.displayed_generation[entry.tile_id]) {
                continue;
            }

            /*
             * Do not queue duplicate requests for the same tile/generation.
             */
            bool already_queued = false;

            for (const wd_retransmit_entry& queued : state.retx_queue) {
                if (queued.tile_id == entry.tile_id &&
                    queued.desired_generation >= entry.tile_generation) {
                    already_queued = true;
                break;
                    }
            }

            if (already_queued) {
                continue;
            }

            wd_retransmit_entry retx{};
            retx.tile_id = entry.tile_id;
            retx.reserved = 0;
            retx.desired_generation = entry.tile_generation;

            missing.push_back(retx);

            if (missing.size() >= MAX_RETRANSMIT_ENTRIES_PER_MESSAGE) {
                break;
            }
        }

        /*
         * Keep the repair queue bounded. If it grows too large, prefer newer
         * summary-derived requests by dropping oldest queued repair work.
         */
        while (state.retx_queue.size() + missing.size() >
            MAX_RETRANSMIT_QUEUE_DEPTH) {
            state.retx_queue.pop_front();
            }

            for (const wd_retransmit_entry& retx : missing) {
                state.retx_queue.push_back(retx);
            }
    }
                                    }

void tcp_reader_main(ClientState* state) {
    while (state->running.load(std::memory_order_relaxed)) {
        uint16_t message_type = 0;
        uint8_t* payload = nullptr;
        uint32_t payload_size = 0;

        if (!wd_recv_tcp_message(state->tcp_fd,
                                 &message_type,
                                 &payload,
                                 &payload_size)) {
            break;
        }

        if (message_type == WD_MSG_TILE_GENERATION_SUMMARY) {
            queue_retransmits_from_summary(*state, payload, payload_size);
            state->stats.tcp_summaries_rx.fetch_add(1, std::memory_order_relaxed);
        } else if (message_type == WD_MSG_ERROR) {
            std::fprintf(stderr, "server sent MSG_ERROR\n");
        }

        std::free(payload);
    }

    state->running.store(false, std::memory_order_relaxed);
}

} // namespace

bool client_connect(ClientState& state,
                    const char* server_host,
                    uint16_t tcp_port,
                    uint16_t client_udp_port,
                    const ClientStreamConfig& stream_config) {
    state.server_host = server_host ? server_host : "";
    state.tcp_port = tcp_port;
    state.client_udp_port = client_udp_port;
    state.stream_config = stream_config;

    state.framebuffer.assign(WD_FRAMEBUFFER_PIXELS, 0xff202020u);
    state.displayed_generation.assign(WD_TOTAL_TILES, 0);

    if (!open_udp_socket(state)) {
        return false;
    }

    if (!open_tcp_socket(state)) {
        return false;
    }

    if (!receive_server_config(state)) {
        return false;
    }

    state.running.store(true, std::memory_order_relaxed);

    std::printf("connected: session=%u display=%ux%u tiles=%ux%u total=%u\n",
                state.config.session_id,
                state.config.width,
                state.config.height,
                state.config.tiles_x,
                state.config.tiles_y,
                state.config.total_tiles);

    return true;
}

void client_disconnect(ClientState& state) {
    state.running.store(false, std::memory_order_relaxed);

    if (state.tcp_fd >= 0) {
        ::shutdown(state.tcp_fd, SHUT_RDWR);
    }

    if (state.tcp_thread.joinable()) {
        state.tcp_thread.join();
    }

    if (state.udp_fd >= 0) {
        ::close(state.udp_fd);
        state.udp_fd = -1;
    }

    if (state.tcp_fd >= 0) {
        ::close(state.tcp_fd);
        state.tcp_fd = -1;
    }
}

bool client_start_tcp_reader(ClientState& state) {
    if (state.tcp_fd < 0) {
        return false;
    }

    state.tcp_thread = std::thread(tcp_reader_main, &state);
    return true;
}

bool client_send_keyboard_key(ClientState& state,
                              uint16_t evdev_key_code,
                              bool pressed) {
    if (state.tcp_fd < 0 || evdev_key_code == 0) {
        return false;
    }

    wd_keyboard_event_payload event{};
    event.session_id = state.config.session_id;
    event.client_timestamp_ns = wd_now_ns();
    event.evdev_key_code = evdev_key_code;
    event.pressed = pressed ? 1 : 0;

    const bool ok = wd_send_tcp_message(state.tcp_fd,
                                        WD_MSG_KEYBOARD_KEY,
                                        &event,
                                        sizeof(event));

    if (ok) {
        state.stats.tcp_keyboard_tx.fetch_add(1, std::memory_order_relaxed);
    }

    return ok;
}

bool client_send_pointer_event(ClientState& state,
                               const wd_pointer_event_payload& event) {
    if (state.tcp_fd < 0) {
        return false;
    }

    return wd_send_tcp_message(state.tcp_fd,
                               WD_MSG_POINTER_EVENT,
                               &event,
                               sizeof(event));
}

bool client_flush_retransmit_requests(ClientState& state) {
    std::vector<wd_retransmit_entry> entries;

    {
        std::lock_guard<std::mutex> retx_lock(state.retx_mutex);
        std::lock_guard<std::mutex> gen_lock(state.generation_mutex);

        while (!state.retx_queue.empty() &&
            entries.size() < MAX_RETRANSMIT_ENTRIES_PER_MESSAGE) {
            wd_retransmit_entry retx = state.retx_queue.front();
        state.retx_queue.pop_front();

        if (retx.tile_id >= WD_TOTAL_TILES) {
            continue;
        }

        /*
         * Tile already arrived while this request was queued.
         */
        if (state.displayed_generation[retx.tile_id] >=
            retx.desired_generation) {
            continue;
            }

            entries.push_back(retx);
            }
    }

    if (entries.empty()) {
        return true;
    }

    wd_retransmit_request_payload_header header{};
    header.session_id = state.config.session_id;
    header.request_count = static_cast<uint16_t>(entries.size());

    const size_t payload_size =
    sizeof(header) + entries.size() * sizeof(wd_retransmit_entry);

    std::vector<uint8_t> payload(payload_size);

    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header),
                entries.data(),
                entries.size() * sizeof(wd_retransmit_entry));

    const bool ok = wd_send_tcp_message(state.tcp_fd,
                                        WD_MSG_RETRANSMIT_REQUEST,
                                        payload.data(),
                                        static_cast<uint32_t>(payload.size()));

    if (ok) {
        state.stats.tcp_retx_requests_tx.fetch_add(1, std::memory_order_relaxed);
    }

    return ok;
}

} // namespace waydisplay
