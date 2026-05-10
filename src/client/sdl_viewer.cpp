#include "sdl_viewer.hpp"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include <SDL2/SDL.h>

#include "client_net.hpp"
#include "sdl_input.hpp"
#include "tile_reassembly.hpp"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"

namespace waydisplay {
namespace {

constexpr uint64_t STATS_INTERVAL_NS = 1000000000ull;
constexpr int SDL_FRAME_DELAY_MS = 8;

uint64_t take_stat(std::atomic<uint64_t>& value) {
    return value.exchange(0, std::memory_order_relaxed);
}

void print_client_stats(ClientState& state) {
    const uint64_t udp_packets = take_stat(state.stats.udp_packets_rx);
    const uint64_t udp_bytes = take_stat(state.stats.udp_bytes_rx);
    const uint64_t invalid = take_stat(state.stats.udp_ignored_invalid);
    const uint64_t old_gen = take_stat(state.stats.udp_ignored_old_generation);
    const uint64_t completed = take_stat(state.stats.udp_tiles_completed);
    const uint64_t summaries = take_stat(state.stats.tcp_summaries_rx);
    const uint64_t retx = take_stat(state.stats.tcp_retx_requests_tx);
    const uint64_t keys = take_stat(state.stats.tcp_keyboard_tx);

    std::printf(
        "[client stats/s] udp_pkts=%llu udp_kib=%.1f completed_tiles=%llu "
        "invalid=%llu old_gen=%llu summaries=%llu retx_req=%llu keys=%llu\n",
        static_cast<unsigned long long>(udp_packets),
        static_cast<double>(udp_bytes) / 1024.0,
        static_cast<unsigned long long>(completed),
        static_cast<unsigned long long>(invalid),
        static_cast<unsigned long long>(old_gen),
        static_cast<unsigned long long>(summaries),
        static_cast<unsigned long long>(retx),
        static_cast<unsigned long long>(keys));
}

bool drain_udp(ClientState& state,
               TileReassembler& reassembler,
               bool& out_frame_dirty) {
    std::vector<uint8_t> recvbuf(sizeof(wd_udp_tile_packet_header) +
                                 WD_UDP_PAYLOAD_TARGET +
                                 512);

    for (;;) {
        ssize_t n = ::recv(state.udp_fd,
                           recvbuf.data(),
                           recvbuf.size(),
                           0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }

            if (errno == EINTR) {
                continue;
            }

            std::perror("recv UDP");
            return false;
        }

        if (n == 0) {
            return true;
        }

        state.stats.udp_packets_rx.fetch_add(1, std::memory_order_relaxed);
        state.stats.udp_bytes_rx.fetch_add(static_cast<uint64_t>(n),
                                           std::memory_order_relaxed);

        CompletedTile completed =
            reassembler.process_udp_packet(state,
                                           recvbuf.data(),
                                           static_cast<size_t>(n));

        if (!completed.valid) {
            continue;
        }

        if (!wd_blit_tile_xrgb8888(state.framebuffer.data(),
                                   completed.tile_id,
                                   completed.tile_bytes.data())) {
            state.stats.udp_ignored_invalid.fetch_add(1,
                                                      std::memory_order_relaxed);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(state.generation_mutex);

            if (completed.generation >
                state.displayed_generation[completed.tile_id]) {
                state.displayed_generation[completed.tile_id] =
                    completed.generation;
            }
        }

        state.stats.udp_tiles_completed.fetch_add(1, std::memory_order_relaxed);
        out_frame_dirty = true;
    }
}

void handle_sdl_event(ClientState& state, const SDL_Event& event) {
    if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP) {
        return;
    }

    if (event.type == SDL_KEYDOWN && event.key.repeat != 0) {
        return;
    }

    const SDL_Scancode scancode = event.key.keysym.scancode;
    const uint16_t evdev_key_code = sdl_scancode_to_evdev(scancode);

    if (evdev_key_code == 0) {
        return;
    }

    const bool pressed = event.type == SDL_KEYDOWN;

    if (!client_send_keyboard_key(state, evdev_key_code, pressed)) {
        std::fprintf(stderr,
                     "failed to send keyboard event evdev=%u pressed=%u\n",
                     evdev_key_code,
                     pressed ? 1 : 0);
        state.running.store(false, std::memory_order_relaxed);
    }
}

} // namespace

int run_sdl_viewer(ClientState& state) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "WayDisplay Client",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        WD_DISPLAY_WIDTH,
        WD_DISPLAY_HEIGHT,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        WD_DISPLAY_WIDTH,
        WD_DISPLAY_HEIGHT);

    if (!texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    TileReassembler reassembler;

    uint64_t last_stats_ns = wd_now_ns();
    bool frame_dirty = true;

    while (state.running.load(std::memory_order_relaxed)) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                state.running.store(false, std::memory_order_relaxed);
                break;
            }

            handle_sdl_event(state, event);
        }

        if (!drain_udp(state, reassembler, frame_dirty)) {
            state.running.store(false, std::memory_order_relaxed);
            break;
        }

        if (!client_flush_retransmit_requests(state)) {
            std::fprintf(stderr, "failed to send retransmit request\n");
            state.running.store(false, std::memory_order_relaxed);
            break;
        }

        if (frame_dirty) {
            SDL_UpdateTexture(texture,
                              nullptr,
                              state.framebuffer.data(),
                              WD_DISPLAY_WIDTH * WD_BYTES_PER_PIXEL);

            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);

            frame_dirty = false;
        }

        const uint64_t now = wd_now_ns();
        if (now - last_stats_ns >= STATS_INTERVAL_NS) {
            print_client_stats(state);
            last_stats_ns = now;
        }

        SDL_Delay(SDL_FRAME_DELAY_MS);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

} // namespace waydisplay
