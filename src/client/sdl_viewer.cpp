#include "sdl_viewer.hpp"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
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

const wd_server_config_payload* g_client_config = nullptr;
int g_window_width = 1;
int g_window_height = 1;
SDL_Rect g_content_rect{0, 0, 1, 1};

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

bool send_host_clipboard_to_server(ClientState& state, bool primary) {
    char* text = SDL_GetClipboardText();
    if (!text) {
        return false;
    }

    const bool ok = primary
        ? client_send_primary_text(state, text)
        : client_send_clipboard_text(state, text);

    SDL_free(text);
    return ok;
}

void drain_remote_selection_updates(ClientState& state) {
    std::string clipboard;
    std::string primary;
    bool have_clipboard = false;
    bool have_primary = false;

    {
        std::lock_guard<std::mutex> lock(state.selection_mutex);

        if (state.pending_clipboard_text_valid) {
            clipboard = std::move(state.pending_clipboard_text);
            state.pending_clipboard_text.clear();
            state.pending_clipboard_text_valid = false;
            have_clipboard = true;
        }

        if (state.pending_primary_text_valid) {
            primary = std::move(state.pending_primary_text);
            state.pending_primary_text.clear();
            state.pending_primary_text_valid = false;
            have_primary = true;
        }
    }

    if (have_clipboard) {
        SDL_SetClipboardText(clipboard.c_str());
    }

    if (have_primary) {
        /* SDL2 does not expose a primary-selection API. Keep this for future
         * backends or an SDL3 migration, but don't discard the remote update
         * silently in the networking layer. */
        std::printf("remote primary selection updated: %zu bytes\n",
                    primary.size());
    }
}

SDL_SystemCursor sdl_cursor_for_wd_shape(uint16_t shape) {
    switch (shape) {
        case WD_CURSOR_SHAPE_POINTER:
            return SDL_SYSTEM_CURSOR_HAND;

        case WD_CURSOR_SHAPE_TEXT:
            return SDL_SYSTEM_CURSOR_IBEAM;

        case WD_CURSOR_SHAPE_MOVE:
            return SDL_SYSTEM_CURSOR_SIZEALL;

        case WD_CURSOR_SHAPE_EW_RESIZE:
            return SDL_SYSTEM_CURSOR_SIZEWE;

        case WD_CURSOR_SHAPE_NS_RESIZE:
            return SDL_SYSTEM_CURSOR_SIZENS;

        case WD_CURSOR_SHAPE_NWSE_RESIZE:
            return SDL_SYSTEM_CURSOR_SIZENWSE;

        case WD_CURSOR_SHAPE_NESW_RESIZE:
            return SDL_SYSTEM_CURSOR_SIZENESW;

        case WD_CURSOR_SHAPE_WAIT:
            return SDL_SYSTEM_CURSOR_WAIT;

        case WD_CURSOR_SHAPE_NOT_ALLOWED:
            return SDL_SYSTEM_CURSOR_NO;

        case WD_CURSOR_SHAPE_DEFAULT:
        default:
            return SDL_SYSTEM_CURSOR_ARROW;
    }
}

void apply_pending_cursor_shape(ClientState& state) {
    static std::array<SDL_Cursor*, WD_CURSOR_SHAPE_COUNT> cursors{};
    static uint16_t current_shape = 0xffffu;

    if (!state.pending_cursor_shape_dirty.exchange(false,
                                                   std::memory_order_acq_rel)) {
        return;
    }

    uint16_t shape =
        state.pending_cursor_shape.load(std::memory_order_relaxed);

    if (shape >= WD_CURSOR_SHAPE_COUNT) {
        shape = WD_CURSOR_SHAPE_DEFAULT;
    }

    if (shape == current_shape) {
        return;
    }

    if (!cursors[shape]) {
        cursors[shape] =
            SDL_CreateSystemCursor(sdl_cursor_for_wd_shape(shape));
    }

    if (cursors[shape]) {
        SDL_SetCursor(cursors[shape]);
        SDL_ShowCursor(SDL_ENABLE);
        current_shape = shape;
    }
}

void free_cached_cursors() {
    static_assert(WD_CURSOR_SHAPE_COUNT > 0, "cursor shape count must be nonzero");

    /*
     * Cursors are intentionally process-lifetime cached. SDL_Quit() will clean
     * them up; avoiding manual free also avoids ordering hazards with backends.
     */
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

bool blit_tile_xrgb8888(ClientState& state,
                        uint16_t tile_id,
                        const std::vector<uint8_t>& tile_bytes) {
    if (tile_id >= state.config.total_tiles) {
        return false;
    }

    const uint32_t tile_x = tile_id % state.config.tiles_x;
    const uint32_t tile_y = tile_id / state.config.tiles_x;
    const uint32_t dst_x = tile_x * state.config.tile_width;
    const uint32_t dst_y = tile_y * state.config.tile_height;
    const size_t expected_size =
        static_cast<size_t>(state.config.tile_width) *
        static_cast<size_t>(state.config.tile_height) *
        WD_BYTES_PER_PIXEL;

    if (tile_bytes.size() < expected_size ||
        dst_x >= state.config.width ||
        dst_y >= state.config.height) {
        return false;
    }

    const uint32_t visible_width =
        std::min<uint32_t>(state.config.tile_width, state.config.width - dst_x);
    const uint32_t visible_height =
        std::min<uint32_t>(state.config.tile_height, state.config.height - dst_y);

    for (uint32_t y = 0; y < visible_height; ++y) {
        const uint8_t* src =
            tile_bytes.data() +
            static_cast<size_t>(y) * state.config.tile_width * WD_BYTES_PER_PIXEL;
        uint32_t* dst =
            state.framebuffer.data() +
            static_cast<size_t>(dst_y + y) * state.config.width + dst_x;

        std::memcpy(dst, src,
                    static_cast<size_t>(visible_width) * WD_BYTES_PER_PIXEL);
    }

    return true;
}

bool drain_udp(ClientState& state,
               TileReassembler& reassembler,
               bool& out_frame_dirty) {
    uint16_t udp_payload_target = state.config.udp_payload_target;
    if (udp_payload_target == 0) {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    std::vector<uint8_t> recvbuf(sizeof(wd_udp_tile_packet_header) +
                                 udp_payload_target +
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

        if (!blit_tile_xrgb8888(state, completed.tile_id, completed.tile_bytes)) {
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

uint16_t map_mouse_coord_x(int x) {
    if (!g_client_config || g_client_config->width == 0) {
        return 0;
    }

    if (x <= g_content_rect.x || g_content_rect.w <= 0) {
        return 0;
    }

    if (x >= g_content_rect.x + g_content_rect.w) {
        return static_cast<uint16_t>(g_client_config->width - 1);
    }

    const int local_x = x - g_content_rect.x;
    const uint32_t mapped =
        static_cast<uint32_t>((static_cast<uint64_t>(local_x) *
                               g_client_config->width) /
                              static_cast<uint32_t>(g_content_rect.w));

    if (mapped >= g_client_config->width) {
        return static_cast<uint16_t>(g_client_config->width - 1);
    }

    return static_cast<uint16_t>(mapped);
}

uint16_t map_mouse_coord_y(int y) {
    if (!g_client_config || g_client_config->height == 0) {
        return 0;
    }

    if (y <= g_content_rect.y || g_content_rect.h <= 0) {
        return 0;
    }

    if (y >= g_content_rect.y + g_content_rect.h) {
        return static_cast<uint16_t>(g_client_config->height - 1);
    }

    const int local_y = y - g_content_rect.y;
    const uint32_t mapped =
        static_cast<uint32_t>((static_cast<uint64_t>(local_y) *
                               g_client_config->height) /
                              static_cast<uint32_t>(g_content_rect.h));

    if (mapped >= g_client_config->height) {
        return static_cast<uint16_t>(g_client_config->height - 1);
    }

    return static_cast<uint16_t>(mapped);
}

void update_window_size(SDL_Window* window) {
    int width = 1;
    int height = 1;

    SDL_GetWindowSize(window, &width, &height);

    if (width < 1) {
        width = 1;
    }

    if (height < 1) {
        height = 1;
    }

    g_window_width = width;
    g_window_height = height;

    if (!g_client_config ||
        g_client_config->width == 0 ||
        g_client_config->height == 0) {
        g_content_rect = SDL_Rect{0, 0, width, height};
        return;
    }

    const uint64_t width_limited_height =
        (static_cast<uint64_t>(width) * g_client_config->height) /
        g_client_config->width;

    int content_width = width;
    int content_height = height;

    if (width_limited_height <= static_cast<uint64_t>(height)) {
        content_height = static_cast<int>(width_limited_height);
    } else {
        content_width = static_cast<int>(
            (static_cast<uint64_t>(height) * g_client_config->width) /
            g_client_config->height);
    }

    if (content_width < 1) {
        content_width = 1;
    }

    if (content_height < 1) {
        content_height = 1;
    }

    g_content_rect = SDL_Rect{
        (width - content_width) / 2,
        (height - content_height) / 2,
        content_width,
        content_height,
    };
}

void handle_sdl_event(ClientState& state, const SDL_Event& event) {
    static bool suppress_paste_v_keyup = false;

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

        if (scancode == SDL_SCANCODE_V) {
            if (pressed && (SDL_GetModState() & KMOD_CTRL)) {
                /*
                 * Ctrl+V is a host-clipboard paste command. Send the clipboard
                 * payload, but do not forward the V key itself: the server will
                 * publish the selection, then synthesize V while Ctrl is already
                 * held remotely. Forwarding V here races ahead of publication.
                 */
                send_host_clipboard_to_server(state, false);
                suppress_paste_v_keyup = true;
                return;
            }

            if (!pressed && suppress_paste_v_keyup) {
                suppress_paste_v_keyup = false;
                return;
            }
        }

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
        pointer.x = map_mouse_coord_x(event.motion.x);
        pointer.y = map_mouse_coord_y(event.motion.y);
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
    pointer.x = map_mouse_coord_x(event.button.x);
    pointer.y = map_mouse_coord_y(event.button.y);
    pointer.button = linux_button;
    pointer.button_state =
    event.type == SDL_MOUSEBUTTONDOWN
    ? WD_POINTER_BUTTON_PRESSED
    : WD_POINTER_BUTTON_RELEASED;
    pointer.modifiers = current_pointer_modifiers();

    if (linux_button == WD_BTN_MIDDLE && event.type == SDL_MOUSEBUTTONDOWN) {
        /*
         * Publish the host clipboard as primary selection first, then forward
         * the middle click so the Wayland client performs its normal primary
         * paste request.
         */
        send_host_clipboard_to_server(state, true);
    }

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
                pointer.x = map_mouse_coord_x(mouse_x);
                pointer.y = map_mouse_coord_y(mouse_y);
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
                pointer.x = map_mouse_coord_x(mouse_x);
                pointer.y = map_mouse_coord_y(mouse_y);
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
    g_client_config = &state.config;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "WayDisplay Client",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        state.config.width,
        state.config.height,
        SDL_WINDOW_SHOWN);

    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_SetWindowMinimumSize(window, state.config.width, state.config.height);
    SDL_SetWindowMaximumSize(window, state.config.width, state.config.height);
    update_window_size(window);

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
        state.config.width,
        state.config.height);

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
        apply_pending_cursor_shape(state);
        drain_remote_selection_updates(state);

        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                state.running.store(false, std::memory_order_relaxed);
                break;
            }

            if (event.type == SDL_WINDOWEVENT &&
                (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                 event.window.event == SDL_WINDOWEVENT_RESIZED)) {
                update_window_size(window);
                frame_dirty = true;
                continue;
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
                              state.config.width * WD_BYTES_PER_PIXEL);

            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, &g_content_rect);
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
    free_cached_cursors();
    SDL_Quit();

    return 0;
}

} // namespace waydisplay
