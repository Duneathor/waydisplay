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

constexpr uint16_t WD_BTN_LEFT = 0x110;
constexpr uint16_t WD_BTN_RIGHT = 0x111;
constexpr uint16_t WD_BTN_MIDDLE = 0x112;
constexpr uint16_t WD_BTN_SIDE = 0x113;
constexpr uint16_t WD_BTN_EXTRA = 0x114;

constexpr uint16_t WD_POINTER_MOD_ALT = 1u << 0;
constexpr uint16_t WD_POINTER_MOD_SHIFT = 1u << 1;
constexpr uint16_t WD_POINTER_MOD_CTRL = 1u << 2;
constexpr uint16_t WD_POINTER_MOD_SUPER = 1u << 3;

uint16_t current_pointer_modifiers() {
    const SDL_Keymod mods = SDL_GetModState();

    uint16_t result = 0;

    if (mods & KMOD_ALT) {
        result |= WD_POINTER_MOD_ALT;
    }

    if (mods & KMOD_SHIFT) {
        result |= WD_POINTER_MOD_SHIFT;
    }

    if (mods & KMOD_CTRL) {
        result |= WD_POINTER_MOD_CTRL;
    }

    if (mods & KMOD_GUI) {
        result |= WD_POINTER_MOD_SUPER;
    }

    return result;
}

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

uint16_t sdl_button_to_linux_button(uint8_t button) {
    switch (button) {
        case SDL_BUTTON_LEFT:
            return WD_BTN_LEFT;
        case SDL_BUTTON_RIGHT:
            return WD_BTN_RIGHT;
        case SDL_BUTTON_MIDDLE:
            return WD_BTN_MIDDLE;
        case SDL_BUTTON_X1:
            return WD_BTN_SIDE;
        case SDL_BUTTON_X2:
            return WD_BTN_EXTRA;
        default:
            return 0;
    }
}

uint16_t clamp_mouse_coord_x(int x) {
    if (x < 0) {
        return 0;
    }

    if (x >= static_cast<int>(WD_DISPLAY_WIDTH)) {
        return static_cast<uint16_t>(WD_DISPLAY_WIDTH - 1);
    }

    return static_cast<uint16_t>(x);
}

uint16_t clamp_mouse_coord_y(int y) {
    if (y < 0) {
        return 0;
    }

    if (y >= static_cast<int>(WD_DISPLAY_HEIGHT)) {
        return static_cast<uint16_t>(WD_DISPLAY_HEIGHT - 1);
    }

    return static_cast<uint16_t>(y);
}

void handle_sdl_event(ClientState& state, const SDL_Event& event) {
    if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
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

        return;
    }

    if (event.type == SDL_MOUSEMOTION) {
        wd_pointer_event_payload pointer{};
        pointer.session_id = state.config.session_id;
        pointer.client_timestamp_ns = wd_now_ns();
        pointer.event_type = WD_POINTER_EVENT_MOTION;
        pointer.x = clamp_mouse_coord_x(event.motion.x);
        pointer.y = clamp_mouse_coord_y(event.motion.y);
        pointer.modifiers = current_pointer_modifiers();

        if (!client_send_pointer_event(state, pointer)) {
            std::fprintf(stderr, "failed to send pointer motion\n");
            state.running.store(false, std::memory_order_relaxed);
        }

        return;
    }

    if (event.type == SDL_MOUSEBUTTONDOWN ||
        event.type == SDL_MOUSEBUTTONUP) {
        const uint16_t linux_button =
        sdl_button_to_linux_button(event.button.button);

    if (linux_button == 0) {
        return;
    }

    wd_pointer_event_payload pointer{};
    pointer.session_id = state.config.session_id;
    pointer.client_timestamp_ns = wd_now_ns();
    pointer.event_type = WD_POINTER_EVENT_BUTTON;
    pointer.x = clamp_mouse_coord_x(event.button.x);
    pointer.y = clamp_mouse_coord_y(event.button.y);
    pointer.button = linux_button;
    pointer.button_state =
    event.type == SDL_MOUSEBUTTONDOWN
    ? WD_POINTER_BUTTON_PRESSED
    : WD_POINTER_BUTTON_RELEASED;
    pointer.modifiers = current_pointer_modifiers();

    if (!client_send_pointer_event(state, pointer)) {
        std::fprintf(stderr, "failed to send pointer button\n");
        state.running.store(false, std::memory_order_relaxed);
    }

    return;
        }

        if (event.type == SDL_MOUSEWHEEL) {
            int mouse_x = 0;
            int mouse_y = 0;
            SDL_GetMouseState(&mouse_x, &mouse_y);

            if (event.wheel.y != 0) {
                wd_pointer_event_payload pointer{};
                pointer.session_id = state.config.session_id;
                pointer.client_timestamp_ns = wd_now_ns();
                pointer.event_type = WD_POINTER_EVENT_AXIS;
                pointer.x = clamp_mouse_coord_x(mouse_x);
                pointer.y = clamp_mouse_coord_y(mouse_y);
                pointer.axis = WD_POINTER_AXIS_VERTICAL;
                pointer.modifiers = current_pointer_modifiers();

                /*
                 * Wayland scroll convention: negative value usually means scroll down.
                 * SDL wheel y > 0 means wheel up.
                 */
                pointer.axis_value = -event.wheel.y * 15;

                if (!client_send_pointer_event(state, pointer)) {
                    std::fprintf(stderr, "failed to send vertical pointer axis\n");
                    state.running.store(false, std::memory_order_relaxed);
                }
            }

            if (event.wheel.x != 0) {
                wd_pointer_event_payload pointer{};
                pointer.session_id = state.config.session_id;
                pointer.client_timestamp_ns = wd_now_ns();
                pointer.event_type = WD_POINTER_EVENT_AXIS;
                pointer.x = clamp_mouse_coord_x(mouse_x);
                pointer.y = clamp_mouse_coord_y(mouse_y);
                pointer.axis = WD_POINTER_AXIS_HORIZONTAL;
                pointer.axis_value = event.wheel.x * 15;
                pointer.modifiers = current_pointer_modifiers();

                if (!client_send_pointer_event(state, pointer)) {
                    std::fprintf(stderr, "failed to send horizontal pointer axis\n");
                    state.running.store(false, std::memory_order_relaxed);
                }
            }

            return;
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
        SDL_WINDOW_SHOWN);

    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetLogicalSize(renderer, 0, 0);

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
            SDL_Rect dst{
                0,
                0,
                static_cast<int>(WD_DISPLAY_WIDTH),
                static_cast<int>(WD_DISPLAY_HEIGHT),
            };

            SDL_RenderCopy(renderer, texture, nullptr, &dst);
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
